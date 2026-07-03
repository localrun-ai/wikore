// ---------------------------------------------------------------------------
// XlsxParser — Excel Open XML (.xlsx) text extraction.
//
// An .xlsx file is a ZIP archive (ECMA-376 / ISO 29500). This parser:
//
//   1. Reads xl/workbook.xml to get the sheet names and their rId order.
//   2. Reads xl/_rels/workbook.xml.rels to resolve rId -> worksheet filename.
//   3. Loads xl/sharedStrings.xml (required by most real-world spreadsheets
//      for string cells).
//   4. For each worksheet in workbook order:
//        * Reads xl/worksheets/sheetN.xml.
//        * Walks <row> elements.  Each row becomes one line of pipe-separated
//          cell values ("val1 | val2 | val3").
//        * The first non-empty row is treated as a header row if it contains
//          string cells; subsequent rows are data rows.  Both are emitted as
//          plain text — no structural distinction is made because spreadsheet
//          "headings" are layout conventions, not semantic metadata.
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
// Cells with empty values are emitted as empty fields to preserve column
// alignment.  Completely empty rows (all cells empty) are skipped.
//
// Security: same in-memory ZIP backend as DocxParser with per-entry
// uncompressed-size cap and read-return-value check.
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
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wikore::ingest {

namespace {

static constexpr std::size_t kXlsxMaxXmlBytes = 16UL * 1024UL * 1024UL;
static constexpr std::size_t kXlsxMaxSheets   = 500;

// In-memory ZIP backend (same pattern as docx/pptx/odt parsers).
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
// Strip the namespace prefix from an XML node/attribute name.
// ---------------------------------------------------------------------------
inline std::string_view local_name(std::string_view qname)
{
    auto c = qname.rfind(':');
    return (c != std::string_view::npos) ? qname.substr(c + 1) : qname;
}

// ---------------------------------------------------------------------------
// Load the shared strings table into a vector.
// Each <si> element maps to one entry (index 0, 1, 2, ...).
// Concatenates all <t> text within an <si>, including rich-text runs.
// ---------------------------------------------------------------------------

std::vector<std::string> load_shared_strings(const std::string& zip)
{
    auto xml = xlsx_extract(zip, "xl/sharedStrings.xml");
    if (xml.empty()) return {};

    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size())) return {};

    std::vector<std::string> ss;
    // Find root sst element by local name
    pugi::xml_node sst;
    for (const auto& n : doc.children()) {
        if (local_name(n.name()) == "sst") { sst = n; break; }
    }
    if (!sst) return {};

    for (const auto& si : sst.children()) {
        if (local_name(si.name()) != "si") continue;
        std::string val;
        // Collect all <t> text, whether directly under <si> or under <r> runs
        std::function<void(const pugi::xml_node&)> collect =
            [&](const pugi::xml_node& n) {
            for (const auto& child : n.children()) {
                if (local_name(child.name()) == "t")
                    val += child.text().get();
                else
                    collect(child);
            }
        };
        collect(si);
        ss.push_back(std::move(val));
    }
    return ss;
}

// ---------------------------------------------------------------------------
// Parse workbook.xml + workbook.xml.rels to get ordered (name, path) pairs.
// ---------------------------------------------------------------------------

struct SheetInfo { std::string name; std::string path; };

std::vector<SheetInfo> load_sheet_order(const std::string& zip)
{
    auto wb_xml   = xlsx_extract(zip, "xl/workbook.xml");
    auto rels_xml = xlsx_extract(zip, "xl/_rels/workbook.xml.rels");
    if (wb_xml.empty() || rels_xml.empty()) return {};

    // Build rId -> target path from relationships.
    const std::string_view ws_type =
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet";
    std::unordered_map<std::string, std::string> rid_to_path;
    {
        pugi::xml_document rd;
        if (!rd.load_buffer(rels_xml.data(), rels_xml.size())) return {};
        pugi::xml_node root;
        for (const auto& n : rd.children())
            if (local_name(n.name()) == "Relationships") { root = n; break; }
        if (root)
            for (const auto& rel : root.children()) {
                if (local_name(rel.name()) != "Relationship") continue;
                if (std::string_view(rel.attribute("Type").value()) != ws_type)
                    continue;
                std::string target = rel.attribute("Target").value();
                // Target is relative to xl/
                std::string path = target.starts_with("/")
                    ? target.substr(1)
                    : "xl/" + target;
                rid_to_path[rel.attribute("Id").value()] = std::move(path);
            }
    }

    // Extract sheet names in workbook order.
    pugi::xml_document wb;
    if (!wb.load_buffer(wb_xml.data(), wb_xml.size())) return {};

    std::vector<SheetInfo> sheets;
    std::function<bool(const pugi::xml_node&)> find_sheets_node =
        [&](const pugi::xml_node& n) -> bool {
        for (const auto& child : n.children()) {
            if (local_name(child.name()) == "sheets") {
                for (const auto& s : child.children()) {
                    if (local_name(s.name()) != "sheet") continue;
                    std::string rid;
                    for (const auto& a : s.attributes()) {
                        if (local_name(a.name()) == "id") { rid = a.value(); break; }
                    }
                    auto it = rid_to_path.find(rid);
                    if (it != rid_to_path.end())
                        sheets.push_back({s.attribute("name").value(), it->second});
                }
                return true;
            }
            if (find_sheets_node(child)) return true;
        }
        return false;
    };
    find_sheets_node(wb);
    return sheets;
}

// ---------------------------------------------------------------------------
// Parse one worksheet XML into rows of cell values.
// Returns each non-empty row as a pipe-separated string.
// ---------------------------------------------------------------------------

std::string parse_worksheet(const std::string&              xml,
                             const std::vector<std::string>& shared_strings)
{
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size())) return {};

    // Find sheetData node
    pugi::xml_node sheet_data;
    std::function<bool(const pugi::xml_node&)> find_sd =
        [&](const pugi::xml_node& n) -> bool {
        for (const auto& child : n.children()) {
            if (local_name(child.name()) == "sheetData") {
                sheet_data = child; return true;
            }
            if (find_sd(child)) return true;
        }
        return false;
    };
    find_sd(doc);
    if (!sheet_data) return {};

    std::string out;
    for (const auto& row : sheet_data.children()) {
        if (local_name(row.name()) != "row") continue;

        std::vector<std::string> cells;
        for (const auto& c : row.children()) {
            if (local_name(c.name()) != "c") continue;
            std::string_view type = c.attribute("t").value();
            std::string val;

            if (type == "s") {
                // Shared string index
                auto v = c.child("v");
                if (v) {
                    int idx = std::atoi(v.text().get());
                    if (idx >= 0 && static_cast<std::size_t>(idx)
                                    < shared_strings.size())
                        val = shared_strings[static_cast<std::size_t>(idx)];
                }
            } else if (type == "inlineStr") {
                // <is><t>...</t></is>
                for (const auto& child : c.children())
                    if (local_name(child.name()) == "is")
                        for (const auto& t : child.children())
                            if (local_name(t.name()) == "t")
                                val += t.text().get();
            } else if (type == "b") {
                auto v = c.child("v");
                val = (v && std::string_view(v.text().get()) == "1")
                      ? "TRUE" : "FALSE";
            } else if (type == "e") {
                auto v = c.child("v");
                if (v) val = v.text().get();
            } else {
                // Numeric or inline string (t="str" or absent)
                auto v = c.child("v");
                if (!v) {
                    // Possibly t="str" with <is><t>
                    for (const auto& ch : c.children())
                        if (local_name(ch.name()) == "is")
                            for (const auto& t : ch.children())
                                if (local_name(t.name()) == "t")
                                    val += t.text().get();
                } else {
                    val = v.text().get();
                }
            }
            cells.push_back(std::move(val));
        }

        // Skip entirely empty rows
        bool has_content = false;
        for (const auto& cv : cells) if (!cv.empty()) { has_content = true; break; }
        if (!has_content) continue;

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

    // --- Shared strings (optional; many spreadsheets omit if no strings) ---
    const auto shared_strings = load_shared_strings(content);

    // --- Sheet order from workbook + rels -----------------------------------
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

    // --- Parse each sheet ---------------------------------------------------
    ParsedDocument out;
    out.filename  = filename;
    out.mime_type = "application/vnd.openxmlformats-officedocument"
                    ".spreadsheetml.sheet";

    for (const auto& sheet : sheets) {
        auto xml = xlsx_extract(content, sheet.path.c_str());
        if (xml.empty()) continue;

        auto body = parse_worksheet(xml, shared_strings);
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
