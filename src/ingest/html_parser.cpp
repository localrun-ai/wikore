// ---------------------------------------------------------------------------
// HtmlParser — HTML text extraction via libxml2's HTML parser.
//
// Rationale for libxml2 over lexbor:
//   libxml2's htmlReadMemory handles the full spectrum of real-world malformed
//   HTML (unclosed tags, mismatched elements, legacy charsets, BOM).  It has
//   20+ years of production use in enterprise software.  lexbor's HTML5 spec-
//   compliance is better, but for an enterprise KB whose primary sources are
//   internal wikis and Word-exported HTML, robustness matters more.
//
// Strategy:
//   1. Parse the HTML into a DOM with htmlReadMemory.
//   2. Walk the DOM recursively.  h1-h6 elements map to section depths 1-6.
//   3. <script>, <style>, <head>, and hidden elements are stripped:
//        display:none  visibility:hidden  aria-hidden="true"
//   4. <table> cells are joined with " | "; rows with "\n".
//   5. Block-level elements (<p>, <div>, <li>, <br>, etc.) emit a newline.
//   6. <a> anchors keep their visible text; href is discarded.
//   7. Text nodes are trimmed and excess whitespace collapsed.
//   8. The charset declared in <meta http-equiv="Content-Type"> or <meta
//      charset="..."> is passed to libxml2 so multi-byte encodings are handled
//      correctly.
//
// Thread safety: HtmlParser is stateless.  Multiple threads may call parse()
// concurrently on the same instance.
// ---------------------------------------------------------------------------

#include "wikore/ingest/parser.hpp"
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::ingest {

namespace {

// ---------------------------------------------------------------------------
// RAII wrappers for libxml2 objects.
// ---------------------------------------------------------------------------

struct XmlDocDeleter {
    void operator()(xmlDocPtr p) const { if (p) xmlFreeDoc(p); }
};
using UniqueXmlDoc = std::unique_ptr<std::remove_pointer_t<xmlDocPtr>, XmlDocDeleter>;

// ---------------------------------------------------------------------------
// Return the local name of a node (without namespace prefix).
// xmlNode::name is already the local name when using the HTML parser.
// ---------------------------------------------------------------------------

inline std::string_view node_name(const xmlNode* n)
{
    return n->name ? reinterpret_cast<const char*>(n->name) : std::string_view{};
}

// ---------------------------------------------------------------------------
// Case-insensitive ASCII comparison (HTML tag names are case-insensitive,
// though htmlReadMemory normalises them to lower-case anyway).
// ---------------------------------------------------------------------------

inline bool name_eq(std::string_view n, std::string_view ref)
{
    return n.size() == ref.size()
        && std::equal(n.begin(), n.end(), ref.begin(),
                      [](unsigned char a, unsigned char b) {
                          return std::tolower(a) == std::tolower(b);
                      });
}

// ---------------------------------------------------------------------------
// Return 0 if `elem` is not a heading, or 1-6 for h1-h6.
// ---------------------------------------------------------------------------

inline int heading_level(std::string_view name)
{
    if (name.size() != 2) return 0;
    if (name[0] != 'h' && name[0] != 'H') return 0;
    if (name[1] < '1' || name[1] > '6') return 0;
    return name[1] - '0';
}

// ---------------------------------------------------------------------------
// Check whether an element should be suppressed (script, style, hidden).
// ---------------------------------------------------------------------------

bool is_suppressed_element(const xmlNode* n)
{
    if (n->type != XML_ELEMENT_NODE) return false;
    std::string_view nm = node_name(n);

    // Structural tags that carry no meaningful text for a RAG corpus
    if (name_eq(nm, "script") || name_eq(nm, "style")  ||
        name_eq(nm, "head")   || name_eq(nm, "noscript") ||
        name_eq(nm, "svg")    || name_eq(nm, "math"))
        return true;

    // HTML `hidden` boolean attribute (<div hidden> / <div hidden="">)
    auto* hidden_attr = xmlGetProp(n, reinterpret_cast<const xmlChar*>("hidden"));
    if (hidden_attr) {
        xmlFree(hidden_attr);
        return true;
    }

    // aria-hidden="true"
    auto* aria = xmlGetProp(n, reinterpret_cast<const xmlChar*>("aria-hidden"));
    if (aria) {
        bool hidden = xmlStrEqual(aria,
            reinterpret_cast<const xmlChar*>("true")) == 1;
        xmlFree(aria);
        if (hidden) return true;
    }

    // style attribute: display:none / visibility:hidden.
    // Parse CSS declarations properly: split by ';', then each declaration
    // by the first ':', trim name and value. This handles all valid CSS
    // whitespace forms including "display : none", "display  :  none",
    // "display\n:\nnone", tab-separated values, etc.
    auto* style = xmlGetProp(n, reinterpret_cast<const xmlChar*>("style"));
    if (style) {
        std::string sv = reinterpret_cast<const char*>(style);
        xmlFree(style);

        auto trim = [](std::string_view s) -> std::string_view {
            auto f = s.find_first_not_of(" \t\r\n\f\v");
            if (f == std::string_view::npos) return {};
            auto l = s.find_last_not_of(" \t\r\n\f\v");
            return s.substr(f, l - f + 1);
        };
        auto lower = [](std::string_view s) {
            std::string r(s);
            for (char& c : r) c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
            return r;
        };

        // Split by ';' and examine each declaration
        std::size_t dec_start = 0;
        while (dec_start <= sv.size()) {
            auto semi = sv.find(';', dec_start);
            std::string_view decl = std::string_view(sv).substr(
                dec_start, (semi == std::string::npos ? sv.size() : semi) - dec_start);

            auto colon = decl.find(':');
            if (colon != std::string_view::npos) {
                auto prop = lower(trim(decl.substr(0, colon)));
                auto val  = lower(trim(decl.substr(colon + 1)));

                // Strip "!important" suffix (with optional whitespace before
                // the '!') so that "display:none!important" and
                // "visibility: hidden ! important" are treated the same as
                // their non-!important counterparts.
                auto bang = val.find('!');
                if (bang != std::string::npos)
                    val = lower(trim(std::string_view(val).substr(0, bang)));

                if ((prop == "display"    && val == "none")   ||
                    (prop == "visibility" && val == "hidden"))
                {
                    return true;
                }
            }

            if (semi == std::string::npos) break;
            dec_start = semi + 1;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Returns true if `name` is a block-level element that should emit a newline
// before and after its content when rendered as plain text.
// ---------------------------------------------------------------------------

bool is_block_element(std::string_view nm)
{
    static const char* const blocks[] = {
        "p", "div", "article", "section", "main", "aside", "nav",
        "header", "footer", "blockquote", "pre", "ul", "ol", "dl",
        "li", "dt", "dd", "figure", "figcaption", "address",
        "h1", "h2", "h3", "h4", "h5", "h6",
        "tr", "th", "td",
        nullptr
    };
    for (int i = 0; blocks[i]; ++i)
        if (nm == blocks[i]) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Collapse internal whitespace in a string to single spaces and trim.
// ---------------------------------------------------------------------------

std::string collapse_ws(const char* text)
{
    if (!text) return {};
    std::string out;
    bool ws = false;
    for (const char* p = text; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (std::isspace(c)) {
            ws = true;
        } else {
            if (ws && !out.empty()) out += ' ';
            ws = false;
            out += static_cast<char>(c);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The section builder.
//
// State:
//   - sections: the top-level ParsedSection list being built.
//   - stack: open section path from the root to the current node (depth,ptr).
//   - pending_text: text accumulated before it is committed to the section.
//
// When a heading at depth D is encountered, the stack is popped to D-1 and
// a new section is pushed.  Body text is appended to the top section.
// ---------------------------------------------------------------------------

struct Builder {
    std::vector<ParsedSection>             sections;
    std::vector<std::pair<int,ParsedSection*>> stack;
    std::string                            pending_text;

    void flush_text()
    {
        // Trim
        auto f = pending_text.find_first_not_of(" \t\r\n");
        if (f == std::string::npos) { pending_text.clear(); return; }
        auto l = pending_text.find_last_not_of(" \t\r\n");
        std::string t = pending_text.substr(f, l - f + 1);
        pending_text.clear();

        if (t.empty()) return;

        ParsedSection* top = stack.empty() ? nullptr : stack.back().second;
        if (!top) {
            sections.emplace_back();
            stack.push_back({0, &sections.back()});
            top = &sections.back();
        }
        if (!top->text.empty()) top->text += ' ';
        top->text += t;
    }

    void emit_newline()
    {
        // Replace any trailing non-newline whitespace with a newline
        ParsedSection* top = stack.empty() ? nullptr : stack.back().second;
        if (!top) return;
        if (!top->text.empty() && top->text.back() != '\n')
            top->text += '\n';
    }

    void open_heading(int depth, const std::string& title)
    {
        // Pop to depth - 1
        while (!stack.empty() && stack.back().first >= depth)
            stack.pop_back();

        ParsedSection sec;
        sec.heading = title;
        sec.depth   = depth;

        if (stack.empty()) {
            sections.push_back(std::move(sec));
            stack.push_back({depth, &sections.back()});
        } else {
            ParsedSection* parent = stack.back().second;
            parent->children.push_back(std::move(sec));
            stack.push_back({depth, &parent->children.back()});
        }
    }
};

// ---------------------------------------------------------------------------
// Collect all visible text from a <table> node, flattening cells.
// Returns a formatted string ("Col A | Col B\nVal 1 | Val 2\n").
// ---------------------------------------------------------------------------

std::string flatten_table(const xmlNode* tbl);  // forward

std::string text_from_node(const xmlNode* n); // forward

std::string text_from_node(const xmlNode* n)
{
    if (!n) return {};
    std::string out;
    for (const xmlNode* c = n->children; c; c = c->next) {
        if (c->type == XML_TEXT_NODE) {
            auto t = collapse_ws(reinterpret_cast<const char*>(c->content));
            if (!t.empty()) {
                if (!out.empty()) out += ' ';
                out += t;
            }
        } else if (c->type == XML_ELEMENT_NODE && !is_suppressed_element(c)) {
            auto t = text_from_node(c);
            if (!t.empty()) {
                if (!out.empty()) out += ' ';
                out += t;
            }
        }
    }
    return out;
}

std::string flatten_table(const xmlNode* tbl)
{
    std::string out;
    // Walk tr elements (may be nested under tbody/thead/tfoot)
    std::function<void(const xmlNode*)> find_rows = [&](const xmlNode* n) {
        for (const xmlNode* c = n->children; c; c = c->next) {
            if (c->type != XML_ELEMENT_NODE) continue;
            std::string_view cn = node_name(c);
            if (name_eq(cn, "tr")) {
                bool first = true;
                for (const xmlNode* cell = c->children; cell; cell = cell->next) {
                    if (cell->type != XML_ELEMENT_NODE) continue;
                    std::string_view cln = node_name(cell);
                    if (!name_eq(cln, "td") && !name_eq(cln, "th")) continue;
                    if (!first) out += " | ";
                    first = false;
                    out += text_from_node(cell);
                }
                out += '\n';
            } else if (name_eq(cn, "tbody") || name_eq(cn, "thead") ||
                       name_eq(cn, "tfoot") || name_eq(cn, "colgroup")) {
                find_rows(c);
            }
        }
    };
    find_rows(tbl);
    return out;
}

// ---------------------------------------------------------------------------
// Recursive DOM walk that feeds the Builder.
// ---------------------------------------------------------------------------

void walk(const xmlNode* n, Builder& b)
{
    if (!n) return;

    if (n->type == XML_TEXT_NODE) {
        auto t = collapse_ws(reinterpret_cast<const char*>(n->content));
        if (!t.empty()) {
            b.pending_text += t;
            b.pending_text += ' ';
        }
        return;
    }

    if (n->type != XML_ELEMENT_NODE) return;
    if (is_suppressed_element(n)) return;

    std::string_view nm = node_name(n);
    int hlevel = heading_level(nm);

    if (hlevel > 0) {
        // Heading: collect heading text, flush pending, open section
        std::string title = text_from_node(n);
        // Trim title
        auto f = title.find_first_not_of(" \t\r\n");
        auto l = title.find_last_not_of(" \t\r\n");
        if (f != std::string::npos) title = title.substr(f, l - f + 1);

        b.flush_text();
        if (!title.empty())
            b.open_heading(hlevel, title);
        return;
    }

    if (name_eq(nm, "br")) {
        b.flush_text();
        b.emit_newline();
        return;
    }

    if (name_eq(nm, "table")) {
        b.flush_text();
        auto tbl = flatten_table(n);
        if (!tbl.empty()) {
            ParsedSection* top = b.stack.empty() ? nullptr : b.stack.back().second;
            if (!top) {
                b.sections.emplace_back();
                b.stack.push_back({0, &b.sections.back()});
                top = &b.sections.back();
            }
            if (!top->text.empty()) top->text += '\n';
            top->text += tbl;
        }
        return;
    }

    // Block element: flush before and newline after
    bool block = is_block_element(nm);
    if (block) b.flush_text();

    for (const xmlNode* c = n->children; c; c = c->next)
        walk(c, b);

    if (block) {
        b.flush_text();
        b.emit_newline();
    }
}

// ---------------------------------------------------------------------------
// Sniff a charset declaration from the HTML bytes to pass to libxml2.
// Looks for:
//   <meta charset="UTF-8">
//   <meta http-equiv="Content-Type" content="text/html; charset=UTF-8">
// Returns nullptr if nothing found (libxml2 will auto-detect).
// ---------------------------------------------------------------------------

const char* sniff_charset(const std::string& html)
{
    // This is a best-effort heuristic on the first 2KB only
    std::string_view head(html.data(), std::min(html.size(), std::size_t{2048}));
    auto pos = head.find("charset");
    if (pos == std::string_view::npos) return nullptr;
    pos += 7;
    while (pos < head.size() && (head[pos] == ' ' || head[pos] == '=')) ++pos;
    if (pos < head.size() && head[pos] == '"') ++pos;
    // Only return known-safe charsets that libxml2 handles well
    if (head.substr(pos, 5) == "UTF-8" || head.substr(pos, 5) == "utf-8")
        return "UTF-8";
    if (head.substr(pos, 10) == "ISO-8859-1") return "ISO-8859-1";
    if (head.substr(pos, 6) == "latin1" || head.substr(pos, 6) == "LATIN1")
        return "ISO-8859-1";
    return nullptr;
}

// ---------------------------------------------------------------------------
// Build full_text from section tree
// ---------------------------------------------------------------------------

void accum_text(const ParsedSection& s, std::string& out)
{
    if (!out.empty() && !s.text.empty()) out += '\n';
    out += s.text;
    for (const auto& c : s.children) accum_text(c, out);
}

} // namespace

// ---------------------------------------------------------------------------
// HtmlParser::parse
// ---------------------------------------------------------------------------

Result<ParsedDocument> HtmlParser::parse(const std::string& content,
                                           const std::string& filename,
                                           const std::string& /*mime_type*/) const
{
    // --- Input validation ---------------------------------------------------
    if (content.empty())
        return std::unexpected(Error::invalid_input("ingest.empty_file"));
    if (content.size() > ParserPort::kMaxInputBytes)
        return std::unexpected(Error::invalid_input("ingest.file_too_large"));

    // Reject obvious binary files (PDF, ZIP, etc.)
    if (content.size() >= 5 &&
        content[0] == '%' && content[1] == 'P' && content[2] == 'D' &&
        content[3] == 'F')
        return std::unexpected(Error::invalid_input("ingest.mime_type_mismatch"));
    if (content.size() >= 4 &&
        content[0] == 'P' && content[1] == 'K' &&
        content[2] == '\x03' && content[3] == '\x04')
        return std::unexpected(Error::invalid_input("ingest.mime_type_mismatch"));

    // --- Parse with libxml2 -------------------------------------------------
    // HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING suppress the stderr libxml2
    // emits for malformed HTML by default.
    // HTML_PARSE_RECOVER enables error recovery (keeps parsing on bad markup).
    // HTML_PARSE_NOBLANKS collapses insignificant whitespace-only text nodes.
    const int opts = HTML_PARSE_RECOVER | HTML_PARSE_NOERROR
                   | HTML_PARSE_NOWARNING | HTML_PARSE_NOBLANKS;

    const char* charset = sniff_charset(content);

    UniqueXmlDoc doc(htmlReadMemory(
        content.data(), static_cast<int>(content.size()),
        filename.c_str(), charset, opts));

    if (!doc)
        return std::unexpected(Error::invalid_input("ingest.html.parse_failed"));

    // --- Walk the DOM -------------------------------------------------------
    Builder b;
    walk(xmlDocGetRootElement(doc.get()), b);
    b.flush_text(); // commit any trailing text

    if (b.sections.empty())
        return std::unexpected(Error::invalid_input("ingest.html.no_text_content"));

    // --- Assemble output ----------------------------------------------------
    ParsedDocument out;
    out.filename  = filename;
    out.mime_type = "text/html";
    out.sections  = std::move(b.sections);

    for (const auto& s : out.sections)
        accum_text(s, out.full_text);

    return out;
}

} // namespace wikore::ingest
