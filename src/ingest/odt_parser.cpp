// ---------------------------------------------------------------------------
// OdtParser — OpenDocument Text (.odt) text extraction.
//
// An .odt file is a ZIP archive (ODF 1.2/1.3).  The document body lives in
// content.xml under office:body/office:text.  This parser:
//
//   1. Extracts content.xml from the ZIP in memory (no temp file).
//   2. Walks office:text children:
//        * <text:h text:outline-level="N"> — heading at depth N (1..6).
//        * <text:p>                         — body paragraph.
//        * <text:list>                      — list items (extracted as
//          paragraphs with "- " prefix for leaf items).
//        * <table:table>                    — flattened: cells separated by
//          " | ", rows by newline.
//   3. Builds the ParsedSection hierarchy using a depth stack, mirroring
//      DocxParser and HtmlParser.
//   4. Styled characters (<text:span>) are transparent: their text content
//      is extracted and the span is discarded.
//   5. Footnotes and annotations are not extracted.
//
// Thread safety: OdtParser is stateless.  Multiple threads may call parse()
// concurrently on the same instance.
// ---------------------------------------------------------------------------

#include "wikore/ingest/parser.hpp"
#include <minizip/unzip.h>
#include <pugixml.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::ingest {

namespace {

static constexpr std::size_t kOdtMaxXmlBytes = 16UL * 1024UL * 1024UL;

// In-memory ZIP backend (same pattern as docx/pptx parsers).
struct OdtMemStream { const char* data; std::size_t size; std::size_t pos; };
static voidpf odt_ms_open(voidpf, const char* fn, int)
    { return (voidpf)(std::uintptr_t)fn; }
static uLong odt_ms_read(voidpf, voidpf s, void* buf, uLong n)
{
    auto* ms = static_cast<OdtMemStream*>(s);
    if (ms->pos >= ms->size) return 0;
    auto avail = static_cast<uLong>(ms->size - ms->pos);
    uLong r = (n < avail) ? n : avail;
    std::memcpy(buf, ms->data + ms->pos, r);
    ms->pos += r; return r;
}
static uLong odt_ms_write(voidpf, voidpf, const void*, uLong) { return 0; }
static long  odt_ms_tell (voidpf, voidpf s)
    { return static_cast<long>(static_cast<OdtMemStream*>(s)->pos); }
static long  odt_ms_seek (voidpf, voidpf s, uLong off, int orig)
{
    auto* ms = static_cast<OdtMemStream*>(s);
    std::size_t np;
    if (orig == ZLIB_FILEFUNC_SEEK_SET) np = off;
    else if (orig == ZLIB_FILEFUNC_SEEK_CUR)
        np = (off > ms->size - ms->pos) ? ms->size : ms->pos + off;
    else np = (off > ms->size) ? 0 : ms->size - off;
    ms->pos = (np > ms->size) ? ms->size : np;
    return 0;
}
static int odt_ms_close(voidpf, voidpf) { return 0; }
static int odt_ms_err  (voidpf, voidpf) { return 0; }

std::string odt_extract(const std::string& zip, const char* entry)
{
    OdtMemStream ms{zip.data(), zip.size(), 0};
    zlib_filefunc_def ff{odt_ms_open, odt_ms_read, odt_ms_write,
                         odt_ms_tell, odt_ms_seek, odt_ms_close,
                         odt_ms_err, nullptr};
    unzFile uf = unzOpen2(reinterpret_cast<const char*>(&ms), &ff);
    if (!uf) return {};
    if (unzLocateFile(uf, entry, 1) != UNZ_OK) { unzClose(uf); return {}; }
    unz_file_info fi{};
    if (unzGetCurrentFileInfo(uf, &fi, nullptr, 0, nullptr, 0, nullptr, 0)
            != UNZ_OK) { unzClose(uf); return {}; }
    if (fi.uncompressed_size > static_cast<uLong>(kOdtMaxXmlBytes)) {
        spdlog::warn("[odt-parser] content.xml uncompressed_size={} > cap",
                     fi.uncompressed_size);
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
// ODT safety constants
// ---------------------------------------------------------------------------

// Maximum number of spaces a single <text:s text:c="N"/> is allowed to emit.
static constexpr int kMaxOdtSpaceExpansion = 256;

// Global per-document text budget. A single call to OdtParser::parse charges
// all text bytes against this budget regardless of how many paragraphs,
// table cells, or nested elements produced them.
static constexpr std::size_t kOdtMaxTextBytes = 8UL * 1024UL * 1024UL; // 8 MiB

// ---------------------------------------------------------------------------
// OdtLimits: shared budget struct threaded through all ODT parsing functions.
//
// Design:
//   * remaining_bytes: a single global text budget shared across all paragraphs,
//     table cells, headings, and inline elements in the document. Every byte
//     written to any output buffer is charged here. Once exhausted, all further
//     text production stops and truncated is set.
//   * truncated: set true (irreversibly) by any limit. OdtParser::parse reads
//     it after the walk and returns ingest.odt.content_limit_exceeded.
//   * walk_depth: incremented on every entry into the walk lambda (sections,
//     nested lists). Guards against unbounded recursion from nested text:section
//     and nested list elements.
// ---------------------------------------------------------------------------

struct OdtLimits {
    std::size_t remaining_bytes;
    bool        truncated   = false;
    int         walk_depth  = 0;

    static constexpr int kMaxWalkDepth = 64;

    // Try to consume `n` bytes. Returns false (and sets truncated) if the
    // budget is already exhausted; returns true otherwise and charges `n`
    // (clamped to remaining).
    bool consume(std::size_t n) {
        if (truncated || remaining_bytes == 0) {
            truncated = true;
            return false;
        }
        if (n > remaining_bytes) {
            remaining_bytes = 0;
            truncated = true;
            return false;
        }
        remaining_bytes -= n;
        return true;
    }
};

// Maximum recursion depth for collect_text (inline span nesting).
static constexpr int kOdtMaxSpanDepth = 64;

// ---------------------------------------------------------------------------
// Attribute lookup by local name. ODF documents may use any prefix for the
// ODF text namespace (e.g. "t:" instead of "text:"). Looking up attributes
// by their full prefixed name (e.g. child.attribute("text:c")) silently
// returns an empty attribute for valid documents that use a different prefix.
// This function iterates all attributes of `n` and returns the first one
// whose local name (after stripping the prefix) matches `local_name`.
// ---------------------------------------------------------------------------

pugi::xml_attribute attr_by_local(const pugi::xml_node& n,
                                   std::string_view      local_name)
{
    for (const auto& a : n.attributes()) {
        std::string_view an = a.name();
        auto colon = an.rfind(':');
        auto local = (colon != std::string_view::npos)
                     ? an.substr(colon + 1) : an;
        if (local == local_name) return a;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Collect all plain text from a node recursively, handling:
//   text:span  — transparent container
//   text:s     — one or more spaces (<text:s text:c="N"/>)
//   text:tab   — emit a tab
//   text:line-break — emit newline
//
// All output is charged to lim.remaining_bytes. lim.truncated is set true
// when any cap fires; callers should stop processing and propagate the flag.
// ---------------------------------------------------------------------------

std::string collect_text(const pugi::xml_node& n, OdtLimits& lim, int depth = 0)
{
    if (lim.truncated) return {};
    if (depth > kOdtMaxSpanDepth) {
        lim.truncated = true;
        return {};
    }
    std::string out;
    for (const auto& child : n.children()) {
        if (lim.truncated) break;
        if (child.type() == pugi::node_pcdata
            || child.type() == pugi::node_cdata)
        {
            std::string_view v = child.value();
            if (!lim.consume(v.size())) break;
            out += v;
            continue;
        }
        if (child.type() != pugi::node_element) continue;

        std::string_view cn = child.name();
        auto colon = cn.rfind(':');
        auto local = (colon != std::string_view::npos)
                     ? cn.substr(colon + 1) : cn;

        if (local == "s") {
            // <text:s text:c="N"/> — N spaces (default 1).
            // Cap to kMaxOdtSpaceExpansion: an attacker-controlled text:c
            // can be 2^31-1, causing a multi-gigabyte std::string::append.
            auto c_attr   = attr_by_local(child, "c");
            int  n_raw    = c_attr ? c_attr.as_int(1) : 1;
            int  n_spaces = std::max(0, std::min(n_raw, kMaxOdtSpaceExpansion));
            if (!lim.consume(static_cast<std::size_t>(n_spaces))) break;
            out.append(static_cast<std::size_t>(n_spaces), ' ');
        } else if (local == "tab") {
            if (!lim.consume(1)) break;
            out += '\t';
        } else if (local == "line-break") {
            if (!lim.consume(1)) break;
            out += '\n';
        } else if (local == "span"  || local == "a"
                || local == "bookmark" || local == "bookmark-start"
                || local == "bookmark-end"  || local == "reference-mark"
                || local == "reference-mark-start") {
            auto sub = collect_text(child, lim, depth + 1);
            out += sub;
        }
        // Footnote, annotation etc: ignored
    }
    return out;
}

// Flatten a <table:table> into text (cells " | ", rows "\n").
std::string flatten_odt_table(const pugi::xml_node& tbl, OdtLimits& lim)
{
    std::string out;
    for (const auto& row : tbl.children()) {
        if (lim.truncated) break;
        std::string_view rn = row.name();
        auto rc = rn.rfind(':');
        if ((rc != std::string_view::npos ? rn.substr(rc+1) : rn)
                != "table-row") continue;
        bool first = true;
        for (const auto& cell : row.children()) {
            if (lim.truncated) break;
            std::string_view cn = cell.name();
            auto cc = cn.rfind(':');
            if ((cc != std::string_view::npos ? cn.substr(cc+1) : cn)
                    != "table-cell") continue;
            if (!first) {
                if (!lim.consume(3)) break; // " | "
                out += " | ";
            }
            first = false;
            // Concatenate all paragraphs in the cell
            bool cp = false;
            for (const auto& p : cell.children()) {
                if (lim.truncated) break;
                std::string_view pn = p.name();
                auto pc = pn.rfind(':');
                auto pl = (pc != std::string_view::npos) ? pn.substr(pc+1) : pn;
                if (pl != "p" && pl != "h") continue;
                auto t = collect_text(p, lim);
                if (!t.empty()) {
                    if (cp) { if (!lim.consume(1)) break; out += ' '; }
                    out += t;
                    cp = true;
                }
            }
        }
        if (!lim.truncated) { lim.consume(1); out += '\n'; }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Parse office:text and build the section hierarchy.
// ---------------------------------------------------------------------------

void parse_odt_body(const pugi::xml_node&      body,
                     std::vector<ParsedSection>& sections,
                     OdtLimits&                  lim)
{
    struct Frame { int depth; ParsedSection* sec; };
    std::vector<Frame> stack;

    auto append_text = [&](const std::string& text) {
        if (text.empty()) return;
        ParsedSection* top = stack.empty() ? nullptr : stack.back().sec;
        if (!top) {
            sections.emplace_back();
            stack.push_back({0, &sections.back()});
            top = &sections.back();
        }
        if (!top->text.empty()) top->text += '\n';
        top->text += text;
    };

    // walk carries a depth counter for section/list nesting.
    // Defined as a non-capturing std::function so it can pass depth explicitly.
    std::function<void(const pugi::xml_node&, int)> walk =
        [&](const pugi::xml_node& n, int depth) {
        if (lim.truncated) return;
        if (depth > OdtLimits::kMaxWalkDepth) {
            lim.truncated = true;
            return;
        }
        for (const auto& child : n.children()) {
            if (lim.truncated) break;
            if (child.type() != pugi::node_element) continue;
            std::string_view cn = child.name();
            auto colon = cn.rfind(':');
            auto local = (colon != std::string_view::npos)
                         ? cn.substr(colon + 1) : cn;

            if (local == "h") {
                // Heading: get outline-level (1..6).
                // Use attr_by_local so documents that prefix the ODF text
                // namespace differently (e.g. "t:outline-level") still work.
                auto lvl_attr = attr_by_local(child, "outline-level");
                int hdepth = lvl_attr ? lvl_attr.as_int(1) : 1;
                hdepth = std::max(1, std::min(hdepth, 6));
                std::string title = collect_text(child, lim);
                // Trim
                auto f = title.find_first_not_of(" \t\r\n");
                auto l = title.find_last_not_of(" \t\r\n");
                if (f != std::string::npos) title = title.substr(f, l-f+1);
                if (title.empty()) continue;

                // Pop stack to parent level
                while (!stack.empty() && stack.back().depth >= hdepth)
                    stack.pop_back();

                ParsedSection sec;
                sec.heading = std::move(title);
                sec.depth   = hdepth;
                if (stack.empty()) {
                    sections.push_back(std::move(sec));
                    stack.push_back({hdepth, &sections.back()});
                } else {
                    ParsedSection* parent = stack.back().sec;
                    parent->children.push_back(std::move(sec));
                    stack.push_back({hdepth, &parent->children.back()});
                }

            } else if (local == "p") {
                append_text(collect_text(child, lim));
            } else if (local == "list") {
                // Walk list-item children; nested lists recurse with depth+1.
                for (const auto& item : child.children()) {
                    if (lim.truncated) break;
                    std::string_view iln = item.name();
                    auto ic = iln.rfind(':');
                    if ((ic != std::string_view::npos
                             ? iln.substr(ic+1) : iln) != "list-item") continue;
                    for (const auto& lc : item.children()) {
                        if (lim.truncated) break;
                        std::string_view lcn = lc.name();
                        auto lcc = lcn.rfind(':');
                        auto lcl = (lcc != std::string_view::npos)
                                   ? lcn.substr(lcc+1) : lcn;
                        if (lcl == "p") {
                            auto t = collect_text(lc, lim);
                            if (!t.empty()) append_text("- " + t);
                        } else if (lcl == "list") {
                            // Nested list: recurse directly with depth+1.
                            // The old approach (fake wrapper + walk) hid the
                            // depth increment; passing depth+1 makes it visible.
                            walk(lc, depth + 1);
                        }
                    }
                }
            } else if (local == "table") {
                append_text(flatten_odt_table(child, lim));
            } else if (local == "section" || local == "text-section") {
                // ODF text sections: recurse with depth+1.
                walk(child, depth + 1);
            }
        }
    };

    walk(body, 0);
}

} // namespace

// ---------------------------------------------------------------------------
// OdtParser::parse
// ---------------------------------------------------------------------------

Result<ParsedDocument> OdtParser::parse(const std::string& content,
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

    auto xml = odt_extract(content, "content.xml");
    if (xml.empty())
        return std::unexpected(Error::invalid_input("ingest.odt.missing_content_xml"));

    pugi::xml_document doc;
    auto result = doc.load_buffer(xml.data(), xml.size(),
                                   pugi::parse_default | pugi::parse_ws_pcdata_single);
    if (!result) {
        spdlog::warn("[odt-parser] XML parse error in content.xml: {}",
                     result.description());
        return std::unexpected(Error::invalid_input("ingest.odt.parse_failed"));
    }

    // Navigate to office:body/office:text.
    // Namespace prefix is typically "office:", but walk generically.
    // Use iterative BFS with a depth cap rather than recursion: a crafted
    // content.xml with thousands of deeply nested wrapper elements before
    // office:text would exhaust the stack. The 16 MiB XML cap does not
    // constrain nesting depth; only an explicit depth limit does.
    static constexpr int kOdtFindTextMaxDepth = 32;
    pugi::xml_node text_node;
    {
        // BFS queue: (node, depth)
        std::vector<std::pair<pugi::xml_node, int>> queue;
        queue.emplace_back(doc, 0);
        for (std::size_t qi = 0; qi < queue.size() && !text_node; ++qi) {
            auto [n, depth] = queue[qi];
            if (depth > kOdtFindTextMaxDepth) continue;
            for (const auto& child : n.children()) {
                if (child.type() != pugi::node_element) continue;
                std::string_view cn = child.name();
                auto colon = cn.rfind(':');
                auto local = (colon != std::string_view::npos)
                             ? cn.substr(colon + 1) : cn;
                if (local == "text") { text_node = child; break; }
                if (depth + 1 <= kOdtFindTextMaxDepth)
                    queue.emplace_back(child, depth + 1);
            }
        }
    }

    if (!text_node)
        return std::unexpected(
            Error::invalid_input("ingest.odt.no_text_body"));

    ParsedDocument out;
    out.filename  = filename;
    out.mime_type = "application/vnd.oasis.opendocument.text";

    OdtLimits lim{kOdtMaxTextBytes};
    parse_odt_body(text_node, out.sections, lim);

    if (lim.truncated) {
        // A safety limit fired (span depth, walk depth, or global byte budget).
        // Return an explicit error rather than partial content as success.
        return std::unexpected(
            Error::invalid_input("ingest.odt.content_limit_exceeded"));
    }

    if (out.sections.empty())
        return std::unexpected(
            Error::invalid_input("ingest.odt.no_text_content"));

    // Build full_text
    std::function<void(const ParsedSection&)> accum =
        [&](const ParsedSection& s) {
        if (!out.full_text.empty() && !s.text.empty())
            out.full_text += '\n';
        out.full_text += s.text;
        for (const auto& c : s.children) accum(c);
    };
    for (const auto& s : out.sections) accum(s);

    return out;
}

} // namespace wikore::ingest
