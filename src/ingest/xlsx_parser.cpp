// ---------------------------------------------------------------------------
// XlsxParser — Excel Open XML (.xlsx) text extraction.
//
// An .xlsx file is a ZIP archive (ECMA-376 / ISO 29500). This parser:
//
//   1. Reads xl/workbook.xml to get the sheet names and their rId order.
//   2. Reads xl/_rels/workbook.xml.rels to resolve rId -> worksheet filename.
//   3. Loads xl/sharedStrings.xml (required by most real-world spreadsheets
//      for string cells).
//   4. For each worksheet in workbook order (deduplicated paths):
//        * Reads xl/worksheets/sheetN.xml.
//        * Walks <row> elements.  Each row becomes one line of pipe-separated
//          cell values, with empty strings inserted for skipped columns to
//          preserve column alignment (sparse rows: A1, C1 -> "A |  | C").
//        * Completely empty rows are skipped.
//   5. Each sheet becomes a ParsedSection at depth 1 with the sheet name as
//      the heading and all rows as the body text.
//
// Cell types handled:
//   t="s"  — shared string (index into xl/sharedStrings.xml)
//   t="str" or absent — inline string or numeric value (rendered as-is)
//   t="inlineStr"     — inline <is><t> string
//   t="b"  — boolean (0/1 -> "FALSE"/"TRUE")
//   t="e"  — error (rendered as-is, e.g. "#DIV/0!")
//
// Security:
//   * Per-entry decompressed size cap: kXlsxMaxXmlBytes (16 MiB).
//   * Sheet count cap: kXlsxMaxSheets (500) with path deduplication.
//   * Aggregate worksheet XML cap: kXlsxMaxTotalXmlBytes (128 MiB).
//     Exceeding either of the last two caps returns an explicit error.
//   * All recursive XML walks are replaced by iterative BFS with depth
//     caps (kXlsxXmlMaxDepth = 64) to prevent stack exhaustion on
//     pathologically deep documents.
//   * ZIP magic-byte check; empty/oversized input rejected early.
//
// Thread safety: XlsxParser is stateless.
// ---------------------------------------------------------------------------

#include "wikore/ingest/parser.hpp"
#include <minizip/unzip.h>
#include <pugixml.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wikore::ingest {

namespace {

static constexpr std::size_t kXlsxMaxXmlBytes      = 16UL  * 1024UL * 1024UL;
static constexpr std::size_t kXlsxMaxTotalXmlBytes  = 128UL * 1024UL * 1024UL;
// Output text budget: caps the total bytes of rendered cell text + separators
// across all worksheets.  A 16 MiB worksheet with many short rows each ending
// at column XFD (16,384 fields) could otherwise produce tens of GiB of
// " | " separators.  16 MiB matches the ODT parser's per-document limit.
static constexpr std::size_t kXlsxMaxOutputBytes    = 16UL  * 1024UL * 1024UL;
static constexpr std::size_t kXlsxMaxSheets         = 500;
static constexpr int         kXlsxXmlMaxDepth       = 64;
// XLSX column/row limits (ECMA-376 §18.3.1.4 / §18.3.1.73)
static constexpr int         kXlsxMaxCol            = 16384;
// Per-row cell cap: at most one <c> element per column.  Protects col_vals
// from O(XML_size / 3) entries (each a pair<int,string> ≈ 36 bytes on 64-bit)
// which would amplify a 16 MiB XML entry to ~190 MiB of vector overhead.
static constexpr int         kXlsxMaxCellsPerRow    = 16384;

// ---------------------------------------------------------------------------
// In-memory ZIP backend (same pattern as docx/pptx/odt parsers).
// ---------------------------------------------------------------------------
struct XlsxMemStream { const char* data; std::size_t size; std::size_t pos; };
static voidpf xlsx_ms_open(voidpf, const char* fn, int)
    { return (voidpf)(std::uintptr_t)fn; }
static uLong xlsx_ms_read(voidpf, voidpf s, void* buf, uLong n)
{
    auto* ms = static_cast<XlsxMemStream*>(s);
    if (ms->pos >= ms->size) return 0;
    auto avail = static_cast<uLong>(ms->size - ms->pos);
    uLong r = (n < avail) ? n : avail;
    std::memcpy(buf, ms->data + ms->pos, r);
    ms->pos += r; return r;
}
static uLong xlsx_ms_write(voidpf, voidpf, const void*, uLong) { return 0; }
static long  xlsx_ms_tell (voidpf, voidpf s)
    { return static_cast<long>(static_cast<XlsxMemStream*>(s)->pos); }
static long  xlsx_ms_seek (voidpf, voidpf s, uLong off, int orig)
{
    auto* ms = static_cast<XlsxMemStream*>(s);
    std::size_t np;
    if (orig == ZLIB_FILEFUNC_SEEK_SET) np = off;
    else if (orig == ZLIB_FILEFUNC_SEEK_CUR)
        np = (off > ms->size - ms->pos) ? ms->size : ms->pos + off;
    else np = (off > ms->size) ? 0 : ms->size - off;
    ms->pos = (np > ms->size) ? ms->size : np;
    return 0;
}
static int xlsx_ms_close(voidpf, voidpf) { return 0; }
static int xlsx_ms_err  (voidpf, voidpf) { return 0; }

// ---------------------------------------------------------------------------
// xlsx_extract result: distinguishes not-found (optional entry absent) from
// errors (size cap exceeded, open/read failure — both indicate a corrupt or
// malicious file and must not be silently skipped).
// ---------------------------------------------------------------------------
enum class XlsxExtractStatus { Ok, NotFound, Error };
struct XlsxExtractResult {
    std::string       content;
    XlsxExtractStatus status = XlsxExtractStatus::Ok;
};

XlsxExtractResult xlsx_extract(const std::string& zip, const char* entry)
{
    XlsxMemStream ms{zip.data(), zip.size(), 0};
    zlib_filefunc_def ff{xlsx_ms_open, xlsx_ms_read, xlsx_ms_write,
                         xlsx_ms_tell, xlsx_ms_seek, xlsx_ms_close,
                         xlsx_ms_err, nullptr};
    unzFile uf = unzOpen2(reinterpret_cast<const char*>(&ms), &ff);
    if (!uf) return {{}, XlsxExtractStatus::Error};
    if (unzLocateFile(uf, entry, 1) != UNZ_OK) {
        unzClose(uf);
        return {{}, XlsxExtractStatus::NotFound};
    }
    unz_file_info fi{};
    if (unzGetCurrentFileInfo(uf, &fi, nullptr, 0, nullptr, 0, nullptr, 0)
            != UNZ_OK) { unzClose(uf); return {{}, XlsxExtractStatus::Error}; }
    if (fi.uncompressed_size > static_cast<uLong>(kXlsxMaxXmlBytes)) {
        spdlog::error("[xlsx-parser] entry '{}' uncompressed_size={} > cap",
                      entry, fi.uncompressed_size);
        unzClose(uf); return {{}, XlsxExtractStatus::Error};
    }
    if (unzOpenCurrentFile(uf) != UNZ_OK)
        { unzClose(uf); return {{}, XlsxExtractStatus::Error}; }
    const auto sz = static_cast<std::size_t>(fi.uncompressed_size);
    std::string out(sz, '\0');
    const int got = unzReadCurrentFile(uf, out.data(), static_cast<unsigned>(sz));
    unzCloseCurrentFile(uf); unzClose(uf);
    if (got < 0 || static_cast<std::size_t>(got) != sz)
        return {{}, XlsxExtractStatus::Error};
    return {std::move(out), XlsxExtractStatus::Ok};
}

// ---------------------------------------------------------------------------
// Strip namespace prefix (e.g., "x:foo" -> "foo").
// ---------------------------------------------------------------------------
inline std::string_view local_name(std::string_view qname)
{
    auto c = qname.rfind(':');
    return (c != std::string_view::npos) ? qname.substr(c + 1) : qname;
}

// ---------------------------------------------------------------------------
// Iterative BFS node finder: returns the first descendant whose local name
// matches target, searching no deeper than max_depth.
// ---------------------------------------------------------------------------
pugi::xml_node bfs_find(const pugi::xml_node& root,
                        std::string_view target, int max_depth = kXlsxXmlMaxDepth)
{
    // queue entries: (node, depth)
    std::queue<std::pair<pugi::xml_node, int>> q;
    q.push({root, 0});
    while (!q.empty()) {
        auto [n, d] = q.front(); q.pop();
        if (local_name(n.name()) == target) return n;
        if (d < max_depth)
            for (const auto& child : n.children())
                q.push({child, d + 1});
    }
    return {};
}

// ---------------------------------------------------------------------------
// Load the shared strings table.
// Returns Ok + vector on success, NotFound if the entry is absent (many
// workbooks have no shared strings), or Error on cap/read/parse failure.
// ---------------------------------------------------------------------------
struct SharedStringsResult {
    std::vector<std::string> strings;
    XlsxExtractStatus        status = XlsxExtractStatus::Ok;
};

SharedStringsResult load_shared_strings(const std::string& zip)
{
    auto res = xlsx_extract(zip, "xl/sharedStrings.xml");
    if (res.status == XlsxExtractStatus::NotFound)
        return {{}, XlsxExtractStatus::NotFound};
    if (res.status == XlsxExtractStatus::Error)
        return {{}, XlsxExtractStatus::Error};

    pugi::xml_document doc;
    if (!doc.load_buffer(res.content.data(), res.content.size()))
        return {{}, XlsxExtractStatus::Error};

    pugi::xml_node sst = bfs_find(doc, "sst");
    if (!sst) return {{}, XlsxExtractStatus::Error};

    std::vector<std::string> ss;
    for (const auto& si : sst.children()) {
        if (local_name(si.name()) != "si") continue;
        std::string val;
        // Iterative BFS within each <si> to collect all <t> children.
        std::queue<std::pair<pugi::xml_node, int>> q;
        q.push({si, 0});
        while (!q.empty()) {
            auto [n, d] = q.front(); q.pop();
            if (local_name(n.name()) == "t") {
                val += n.text().get();
                continue; // <t> never has meaningful children
            }
            if (d < kXlsxXmlMaxDepth)
                for (const auto& child : n.children())
                    q.push({child, d + 1});
        }
        ss.push_back(std::move(val));
    }
    return {std::move(ss), XlsxExtractStatus::Ok};
}

// ---------------------------------------------------------------------------
// Parse workbook + rels to get deduplicated, ordered (name, path) pairs.
// Returns empty vector on any error (missing or corrupt required entries).
// Uses a bool out-param to distinguish "no worksheets found" from "error".
// ---------------------------------------------------------------------------
struct SheetInfo { std::string name; std::string path; };

// Returns false if a required entry was corrupt/oversized (caller returns
// ingest.xlsx.corrupt); returns true with an empty vector if the workbook
// is valid but lists no worksheet paths.
bool load_sheet_order(const std::string& zip, std::vector<SheetInfo>& out)
{
    auto wb_res   = xlsx_extract(zip, "xl/workbook.xml");
    auto rels_res = xlsx_extract(zip, "xl/_rels/workbook.xml.rels");
    // Both are required; any failure (including cap-exceeded) is fatal.
    if (wb_res.status   != XlsxExtractStatus::Ok) return false;
    if (rels_res.status != XlsxExtractStatus::Ok) return false;

    // Worksheet relationship Type suffix, shared by all three known URI forms:
    //   Transitional: http://schemas.openxmlformats.org/.../relationships/worksheet
    //   HTTPS variant: https://schemas.openxmlformats.org/.../relationships/worksheet
    //   Strict OOXML:  http://purl.oclc.org/ooxml/officeDocument/relationships/worksheet
    static constexpr std::string_view kWsTypeSuffix = "/relationships/worksheet";
    auto is_worksheet_rel = [&](std::string_view type) {
        return type.ends_with(kWsTypeSuffix);
    };
    std::unordered_map<std::string, std::string> rid_to_path;
    {
        pugi::xml_document rd;
        if (!rd.load_buffer(rels_res.content.data(), rels_res.content.size()))
            return false;
        pugi::xml_node root = bfs_find(rd, "Relationships");
        if (root)
            for (const auto& rel : root.children()) {
                if (local_name(rel.name()) != "Relationship") continue;
                if (!is_worksheet_rel(rel.attribute("Type").value()))
                    continue;
                std::string target = rel.attribute("Target").value();
                std::string path = target.starts_with("/")
                    ? target.substr(1)
                    : "xl/" + target;
                rid_to_path[rel.attribute("Id").value()] = std::move(path);
            }
    }

    pugi::xml_document wb;
    if (!wb.load_buffer(wb_res.content.data(), wb_res.content.size()))
        return false;

    pugi::xml_node sheets_node = bfs_find(wb, "sheets");
    if (!sheets_node) return true; // valid workbook, but no sheets listed

    std::unordered_set<std::string> seen_paths;
    for (const auto& s : sheets_node.children()) {
        if (local_name(s.name()) != "sheet") continue;
        std::string rid;
        for (const auto& a : s.attributes()) {
            if (local_name(a.name()) == "id") { rid = a.value(); break; }
        }
        auto it = rid_to_path.find(rid);
        if (it == rid_to_path.end()) continue;
        if (!seen_paths.insert(it->second).second) {
            spdlog::warn("[xlsx-parser] duplicate worksheet path '{}'; skipping",
                         it->second);
            continue;
        }
        out.push_back({s.attribute("name").value(), it->second});
    }
    return true;
}

// ---------------------------------------------------------------------------
// Find the first direct child whose local name matches lname.
// Used where node names may carry a namespace prefix (e.g. <x:v>).
// ---------------------------------------------------------------------------
pugi::xml_node child_by_local(const pugi::xml_node& n, std::string_view lname)
{
    for (const auto& child : n.children())
        if (local_name(child.name()) == lname) return child;
    return {};
}

// ---------------------------------------------------------------------------
// Parse column letters from a cell reference (e.g. "C1" -> 3, "AA2" -> 27).
// Returns 1-based column index, or 0 if the reference is absent or exceeds
// kXlsxMaxCol (16384 = XFD, the XLSX column limit).  Overflow-safe: the
// intermediate value is checked before multiplication so "ZZZZZZ1" cannot
// produce a multi-gigabyte allocation.
// ---------------------------------------------------------------------------
int col_from_ref(std::string_view r)
{
    int col = 0;
    for (char c : r) {
        if (c < 'A' || c > 'Z') break;
        // Guard against overflow before multiplying: if col would exceed
        // kXlsxMaxCol after this step, the reference is invalid.
        if (col > (kXlsxMaxCol - (c - 'A' + 1)) / 26)
            return 0;
        col = col * 26 + (c - 'A' + 1);
    }
    return (col > kXlsxMaxCol) ? 0 : col;
}

// ---------------------------------------------------------------------------
// Parse one worksheet XML into rows of pipe-separated cell values.
// Sparse columns (missing cells between r="A1" and r="C1") are filled with
// empty strings to preserve column alignment.
//
// remaining: shared global output-byte budget (across all worksheets).
//   Sets remaining=0 if the budget is exceeded; caller treats that as
//   content_limit_exceeded.
//
// Returns XlsxExtractStatus::Error if the XML fails to parse or sheetData
// is absent (corrupt worksheet) — distinguishable from a legitimately
// empty sheet (Ok + empty body) and budget exhaustion (remaining==0).
// ---------------------------------------------------------------------------
struct ParseWorksheetResult {
    std::string       body;
    XlsxExtractStatus status = XlsxExtractStatus::Ok;
};

ParseWorksheetResult parse_worksheet(const std::string&              xml,
                                      const std::vector<std::string>& shared_strings,
                                      std::size_t&                    remaining)
{
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size()))
        return {{}, XlsxExtractStatus::Error};

    pugi::xml_node sheet_data = bfs_find(doc, "sheetData");
    if (!sheet_data)
        return {{}, XlsxExtractStatus::Error};

    // Helper: charge `n` bytes from the budget before materialising content.
    // Returns true if the charge succeeds; sets remaining=0 and returns false
    // if the budget would be exceeded.
    auto charge = [&](std::size_t n) -> bool {
        if (n > remaining) {
            spdlog::error("[xlsx-parser] output budget exceeded; rejecting");
            remaining = 0;
            return false;
        }
        remaining -= n;
        return true;
    };

    std::string out;
    for (const auto& row : sheet_data.children()) {
        if (local_name(row.name()) != "row") continue;

        // Collect (col_index, value) pairs so sparse rows can be padded.
        // Each value is budget-charged BEFORE it is copied into memory.
        // Per-row cell cap: at most kXlsxMaxCellsPerRow entries (XLSX spec
        // allows at most one <c> per column; more = corrupt).
        std::vector<std::pair<int, std::string>> col_vals;
        col_vals.reserve(32); // avoid small-count reallocations
        int  max_col        = 0;
        int  next_col       = 1;  // sequential column for cells without r=
        int  cells_in_row   = 0;
        bool budget_exceeded = false;

        for (const auto& c : row.children()) {
            if (local_name(c.name()) != "c") continue;

            // Resolve column index.
            // The r= attribute is optional (Open XML SDK models it as nullable).
            // When absent, cells are sequential starting at 1 (or after the
            // last seen column).  When present but malformed or overflowed,
            // skip only that cell.
            int col;
            auto r_attr = c.attribute("r");
            if (r_attr) {
                col = col_from_ref(r_attr.value());
                if (col == 0) {
                    // Explicit but malformed/overflowed reference — skip cell,
                    // but do NOT advance next_col so sequential order is kept.
                    continue;
                }
            } else {
                // r attribute absent: infer sequential column.
                col = next_col;
                if (col > kXlsxMaxCol) continue; // sequential overflow
            }
            next_col = col + 1; // advance for next sequential cell

            // Enforce per-row cell cap before any allocation.
            if (++cells_in_row > kXlsxMaxCellsPerRow)
                return {{}, XlsxExtractStatus::Error};

            if (col > max_col) max_col = col;

            std::string_view type = c.attribute("t").value();
            std::string val;

            if (type == "s") {
                auto v = child_by_local(c, "v");
                if (v) {
                    // Use strtol — std::atoi has undefined behaviour on
                    // overflow (e.g. the string "9999999999").
                    char* end = nullptr;
                    long idx_l = std::strtol(v.text().get(), &end, 10);
                    if (end != v.text().get()        // consumed at least one digit
                        && idx_l >= 0
                        && static_cast<std::size_t>(idx_l) < shared_strings.size()) {
                        const auto& sv = shared_strings[
                            static_cast<std::size_t>(idx_l)];
                        if (!charge(sv.size())) { budget_exceeded = true; break; }
                        val = sv;
                    }
                }
            } else if (type == "inlineStr") {
                // Rich inline strings use <is><r><t>...</t></r></is>;
                // plain ones use <is><t>...</t></is>.  Collect all <t>
                // descendants with a BFS bounded by kXlsxXmlMaxDepth.
                auto is = child_by_local(c, "is");
                if (is) {
                    std::queue<std::pair<pugi::xml_node, int>> q;
                    q.push({is, 0});
                    while (!q.empty() && !budget_exceeded) {
                        auto [n, d] = q.front(); q.pop();
                        if (local_name(n.name()) == "t") {
                            std::string_view tv = n.text().get();
                            if (!charge(tv.size())) { budget_exceeded = true; break; }
                            val += tv;
                            continue; // <t> has no meaningful children
                        }
                        if (d < kXlsxXmlMaxDepth)
                            for (const auto& child : n.children())
                                q.push({child, d + 1});
                    }
                }
            } else if (type == "b") {
                auto v = child_by_local(c, "v");
                val = (v && std::string_view(v.text().get()) == "1")
                      ? "TRUE" : "FALSE";
                if (!charge(val.size())) { budget_exceeded = true; break; }
            } else if (type == "e") {
                auto v = child_by_local(c, "v");
                if (v) {
                    std::string_view ev = v.text().get();
                    if (!charge(ev.size())) { budget_exceeded = true; break; }
                    val = ev;
                }
            } else {
                // Numeric or formula (t="str" or absent)
                auto v = child_by_local(c, "v");
                if (!v) {
                    auto is = child_by_local(c, "is");
                    if (is) {
                        std::queue<std::pair<pugi::xml_node, int>> q;
                        q.push({is, 0});
                        while (!q.empty() && !budget_exceeded) {
                            auto [n, d] = q.front(); q.pop();
                            if (local_name(n.name()) == "t") {
                                std::string_view tv = n.text().get();
                                if (!charge(tv.size())) { budget_exceeded = true; break; }
                                val += tv;
                                continue;
                            }
                            if (d < kXlsxXmlMaxDepth)
                                for (const auto& child : n.children())
                                    q.push({child, d + 1});
                        }
                    }
                } else {
                    std::string_view nv = v.text().get();
                    if (!charge(nv.size())) { budget_exceeded = true; break; }
                    val = nv;
                }
            }
            if (budget_exceeded) break;
            col_vals.push_back({col, std::move(val)});
        }
        if (budget_exceeded) return {{}, XlsxExtractStatus::Ok}; // remaining==0, caller detects

        if (max_col == 0) continue; // entirely empty row

        // Pre-charge separators: (max_col - 1) * 3 bytes for " | " between
        // columns. This must happen before the dense expansion so that wide
        // sparse rows (e.g. one cell at XFD) are budgeted before allocation.
        const std::size_t sep_cost =
            (max_col > 1) ? static_cast<std::size_t>(max_col - 1) * 3 : 0;
        if (!charge(sep_cost)) return {{}, XlsxExtractStatus::Ok};
        // Also charge the newline between rows.
        if (!out.empty() && !charge(1)) return {{}, XlsxExtractStatus::Ok};

        // Expand sparse col_vals into a dense vector indexed [0..max_col-1].
        std::vector<std::string> cells(static_cast<std::size_t>(max_col));
        for (auto& [cidx, cval] : col_vals)
            if (cidx >= 1 && cidx <= max_col)
                cells[static_cast<std::size_t>(cidx - 1)] = std::move(cval);

        // Skip rows where every cell is empty.
        bool has_content = false;
        for (const auto& cv : cells) if (!cv.empty()) { has_content = true; break; }
        if (!has_content) {
            // Refund the pre-charged separators for this empty row.
            remaining += sep_cost + (!out.empty() ? 1 : 0);
            continue;
        }

        if (!out.empty()) out += '\n';
        for (std::size_t i = 0; i < cells.size(); ++i) {
            if (i) out += " | ";
            out += cells[i];
        }
    }
    return {std::move(out), XlsxExtractStatus::Ok};
}

} // namespace

// ---------------------------------------------------------------------------
// XlsxParser::parse
// ---------------------------------------------------------------------------

Result<ParsedDocument> XlsxParser::parse(const std::string& content,
                                           const std::string& filename,
                                           const std::string& /*mime_type*/) const
{
    if (content.empty())
        return std::unexpected(Error::invalid_input("ingest.empty_file"));
    if (content.size() > ParserPort::kMaxInputBytes)
        return std::unexpected(Error::invalid_input("ingest.file_too_large"));
    if (content.size() < 4
        || content[0] != 'P' || content[1] != 'K'
        || content[2] != '\x03' || content[3] != '\x04')
        return std::unexpected(Error::invalid_input("ingest.mime_type_mismatch"));

    // --- Shared strings (optional; absent when no string cells) -------------
    // NotFound is normal; Error means the entry exists but is corrupt/oversized.
    auto ss_result = load_shared_strings(content);
    if (ss_result.status == XlsxExtractStatus::Error) {
        spdlog::error("[xlsx-parser] '{}' sharedStrings.xml corrupt or oversized",
                      filename);
        return std::unexpected(Error::invalid_input("ingest.xlsx.corrupt"));
    }
    const auto& shared_strings = ss_result.strings;

    // --- Sheet order from workbook + rels (paths deduplicated) --------------
    std::vector<SheetInfo> sheets;
    if (!load_sheet_order(content, sheets)) {
        spdlog::error("[xlsx-parser] '{}' workbook.xml or rels corrupt/oversized",
                      filename);
        return std::unexpected(Error::invalid_input("ingest.xlsx.corrupt"));
    }
    if (sheets.empty())
        return std::unexpected(
            Error::invalid_input("ingest.xlsx.no_worksheets"));

    if (sheets.size() > kXlsxMaxSheets) {
        spdlog::error("[xlsx-parser] '{}' has {} sheets > cap ({}); rejecting",
                      filename, sheets.size(), kXlsxMaxSheets);
        return std::unexpected(
            Error::invalid_input("ingest.xlsx.too_many_sheets"));
    }

    // --- Parse each sheet with aggregate XML cap and output budget ----------
    ParsedDocument out;
    out.filename  = filename;
    out.mime_type = "application/vnd.openxmlformats-officedocument"
                    ".spreadsheetml.sheet";

    std::size_t total_xml_bytes  = 0;
    std::size_t output_remaining = kXlsxMaxOutputBytes;
    for (const auto& sheet : sheets) {
        auto xml_res = xlsx_extract(content, sheet.path.c_str());
        if (xml_res.status == XlsxExtractStatus::Error) {
            spdlog::error("[xlsx-parser] '{}' worksheet '{}' corrupt or oversized",
                          filename, sheet.path);
            return std::unexpected(Error::invalid_input("ingest.xlsx.corrupt"));
        }
        if (xml_res.status == XlsxExtractStatus::NotFound) {
            spdlog::warn("[xlsx-parser] '{}' worksheet '{}' missing from ZIP",
                         filename, sheet.path);
            // A referenced-but-missing sheet is a structural error: the
            // workbook manifest is inconsistent with the archive contents.
            return std::unexpected(Error::invalid_input("ingest.xlsx.corrupt"));
        }

        total_xml_bytes += xml_res.content.size();
        if (total_xml_bytes > kXlsxMaxTotalXmlBytes) {
            spdlog::error(
                "[xlsx-parser] '{}' aggregate worksheet XML {} > cap {}; rejecting",
                filename, total_xml_bytes, kXlsxMaxTotalXmlBytes);
            return std::unexpected(
                Error::invalid_input("ingest.xlsx.content_limit_exceeded"));
        }

        auto ws_result = parse_worksheet(xml_res.content, shared_strings,
                                          output_remaining);
        if (ws_result.status == XlsxExtractStatus::Error) {
            spdlog::error("[xlsx-parser] '{}' worksheet '{}' XML malformed",
                          filename, sheet.path);
            return std::unexpected(Error::invalid_input("ingest.xlsx.corrupt"));
        }
        if (output_remaining == 0) {
            spdlog::error("[xlsx-parser] '{}' output budget exceeded; rejecting",
                          filename);
            return std::unexpected(
                Error::invalid_input("ingest.xlsx.content_limit_exceeded"));
        }
        if (ws_result.body.empty()) continue;

        ParsedSection sec;
        sec.heading = sheet.name;
        sec.depth   = 1;
        sec.text    = std::move(ws_result.body);
        out.sections.push_back(std::move(sec));
    }

    if (out.sections.empty())
        return std::unexpected(
            Error::invalid_input("ingest.xlsx.no_text_content"));

    for (const auto& s : out.sections) {
        if (!out.full_text.empty()) out.full_text += '\n';
        out.full_text += s.text;
    }

    return out;
}

} // namespace wikore::ingest
