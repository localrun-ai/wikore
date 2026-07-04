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
// XLSX column limit: XFD = 16384 (ECMA-376 §18.3.1.4)
static constexpr int         kXlsxMaxCol            = 16384;

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

// Returns the decompressed content of one ZIP entry, or empty on error/cap.
std::string xlsx_extract(const std::string& zip, const char* entry)
{
    XlsxMemStream ms{zip.data(), zip.size(), 0};
    zlib_filefunc_def ff{xlsx_ms_open, xlsx_ms_read, xlsx_ms_write,
                         xlsx_ms_tell, xlsx_ms_seek, xlsx_ms_close,
                         xlsx_ms_err, nullptr};
    unzFile uf = unzOpen2(reinterpret_cast<const char*>(&ms), &ff);
    if (!uf) return {};
    if (unzLocateFile(uf, entry, 1) != UNZ_OK) { unzClose(uf); return {}; }
    unz_file_info fi{};
    if (unzGetCurrentFileInfo(uf, &fi, nullptr, 0, nullptr, 0, nullptr, 0)
            != UNZ_OK) { unzClose(uf); return {}; }
    if (fi.uncompressed_size > static_cast<uLong>(kXlsxMaxXmlBytes)) {
        spdlog::warn("[xlsx-parser] entry '{}' uncompressed_size={} > cap",
                     entry, fi.uncompressed_size);
        unzClose(uf); return {};
    }
    if (unzOpenCurrentFile(uf) != UNZ_OK) { unzClose(uf); return {}; }
    const auto sz = static_cast<std::size_t>(fi.uncompressed_size);
    std::string out(sz, '\0');
    const int got = unzReadCurrentFile(uf, out.data(), static_cast<unsigned>(sz));
    unzCloseCurrentFile(uf); unzClose(uf);
    if (got < 0 || static_cast<std::size_t>(got) != sz) return {};
    return out;
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
// Each <si> maps to one vector entry. All <t> text within an <si> is
// concatenated (handles plain and rich-text runs).
// Depth limit kXlsxXmlMaxDepth prevents stack exhaustion.
// ---------------------------------------------------------------------------
std::vector<std::string> load_shared_strings(const std::string& zip)
{
    auto xml = xlsx_extract(zip, "xl/sharedStrings.xml");
    if (xml.empty()) return {};

    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size())) return {};

    pugi::xml_node sst = bfs_find(doc, "sst");
    if (!sst) return {};

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
    return ss;
}

// ---------------------------------------------------------------------------
// Parse workbook + rels to get deduplicated, ordered (name, path) pairs.
// ---------------------------------------------------------------------------
struct SheetInfo { std::string name; std::string path; };

std::vector<SheetInfo> load_sheet_order(const std::string& zip)
{
    auto wb_xml   = xlsx_extract(zip, "xl/workbook.xml");
    auto rels_xml = xlsx_extract(zip, "xl/_rels/workbook.xml.rels");
    if (wb_xml.empty() || rels_xml.empty()) return {};

    const std::string_view ws_type =
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet";
    std::unordered_map<std::string, std::string> rid_to_path;
    {
        pugi::xml_document rd;
        if (!rd.load_buffer(rels_xml.data(), rels_xml.size())) return {};
        pugi::xml_node root = bfs_find(rd, "Relationships");
        if (root)
            for (const auto& rel : root.children()) {
                if (local_name(rel.name()) != "Relationship") continue;
                if (std::string_view(rel.attribute("Type").value()) != ws_type)
                    continue;
                std::string target = rel.attribute("Target").value();
                std::string path = target.starts_with("/")
                    ? target.substr(1)
                    : "xl/" + target;
                rid_to_path[rel.attribute("Id").value()] = std::move(path);
            }
    }

    pugi::xml_document wb;
    if (!wb.load_buffer(wb_xml.data(), wb_xml.size())) return {};

    pugi::xml_node sheets_node = bfs_find(wb, "sheets");
    if (!sheets_node) return {};

    std::vector<SheetInfo> sheets;
    std::unordered_set<std::string> seen_paths; // deduplication guard
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
        sheets.push_back({s.attribute("name").value(), it->second});
    }
    return sheets;
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
// Returns empty string and sets remaining=0 if the budget is exceeded;
// the caller must treat remaining==0 as a content_limit_exceeded error.
// ---------------------------------------------------------------------------
std::string parse_worksheet(const std::string&              xml,
                             const std::vector<std::string>& shared_strings,
                             std::size_t&                    remaining)
{
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size())) return {};

    pugi::xml_node sheet_data = bfs_find(doc, "sheetData");
    if (!sheet_data) return {};

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
        std::vector<std::pair<int, std::string>> col_vals;
        int max_col = 0;
        bool budget_exceeded = false;

        for (const auto& c : row.children()) {
            if (local_name(c.name()) != "c") continue;
            const int col = col_from_ref(c.attribute("r").value());
            if (col > max_col) max_col = col;

            std::string_view type = c.attribute("t").value();
            std::string val;

            if (type == "s") {
                auto v = child_by_local(c, "v");
                if (v) {
                    int idx = std::atoi(v.text().get());
                    if (idx >= 0 && static_cast<std::size_t>(idx)
                                    < shared_strings.size()) {
                        const auto& sv = shared_strings[
                            static_cast<std::size_t>(idx)];
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
        if (budget_exceeded) return {};

        if (max_col == 0) continue; // entirely empty row

        // Pre-charge separators: (max_col - 1) * 3 bytes for " | " between
        // columns. This must happen before the dense expansion so that wide
        // sparse rows (e.g. one cell at XFD) are budgeted before allocation.
        const std::size_t sep_cost =
            (max_col > 1) ? static_cast<std::size_t>(max_col - 1) * 3 : 0;
        if (!charge(sep_cost)) return {};
        // Also charge the newline between rows.
        if (!out.empty() && !charge(1)) return {};

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
    return out;
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
    const auto shared_strings = load_shared_strings(content);

    // --- Sheet order from workbook + rels (paths deduplicated) --------------
    auto sheets = load_sheet_order(content);
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
        auto xml = xlsx_extract(content, sheet.path.c_str());
        if (xml.empty()) continue;

        total_xml_bytes += xml.size();
        if (total_xml_bytes > kXlsxMaxTotalXmlBytes) {
            spdlog::error(
                "[xlsx-parser] '{}' aggregate worksheet XML {} > cap {}; rejecting",
                filename, total_xml_bytes, kXlsxMaxTotalXmlBytes);
            return std::unexpected(
                Error::invalid_input("ingest.xlsx.content_limit_exceeded"));
        }

        auto body = parse_worksheet(xml, shared_strings, output_remaining);
        if (output_remaining == 0) {
            spdlog::error("[xlsx-parser] '{}' output budget exceeded; rejecting",
                          filename);
            return std::unexpected(
                Error::invalid_input("ingest.xlsx.content_limit_exceeded"));
        }
        if (body.empty()) continue;

        ParsedSection sec;
        sec.heading = sheet.name;
        sec.depth   = 1;
        sec.text    = std::move(body);
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
