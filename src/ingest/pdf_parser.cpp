// ---------------------------------------------------------------------------
// PdfParser — PDF text extraction via poppler-cpp.
//
// Strategy:
//   1. Load the PDF from the in-memory byte array (no temp file).
//   2. If the document has a PDF outline (ToC), walk it to build the section
//      hierarchy.  The full text of each page is extracted with page->text(),
//      which returns flowing UTF-8 text in reading order.  Section bodies are
//      assembled by consuming pages between consecutive outline destinations.
//   3. If there is no outline (or it is trivially empty), emit a single flat
//      section with all page text concatenated.
//
// Why not font-size heuristics:
//   poppler-cpp's text_box::get_font_size() returns -1 for the majority of
//   real-world PDFs because most producers do not embed per-glyph font
//   metrics in the way the API requires.  Using -1 as the discriminator would
//   make every token look identical.  The ToC is always reliable when present.
//
// Thread safety: PdfParser is stateless.  Multiple threads may call parse()
// concurrently on the same instance.
// ---------------------------------------------------------------------------

#include "wikore/ingest/parser.hpp"
#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-page.h>
#include <poppler/cpp/poppler-toc.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wikore::ingest {

namespace {

// ---------------------------------------------------------------------------
// ustring -> UTF-8 std::string.
// ---------------------------------------------------------------------------

std::string ustring_to_utf8(const poppler::ustring& u)
{
    std::string out;
    out.reserve(u.size());
    std::size_t i = 0;
    while (i < u.size()) {
        const auto cu = static_cast<unsigned>(u[i]);

        // Detect valid surrogate pair (U+D800..DBFF followed by U+DC00..DFFF)
        // and combine into a single supplementary-plane code point before
        // encoding as 4-byte UTF-8.  An unpaired surrogate is replaced with
        // U+FFFD so the output is always well-formed UTF-8.
        if (cu >= 0xD800 && cu <= 0xDBFF) {
            if (i + 1 < u.size()) {
                const auto cl = static_cast<unsigned>(u[i + 1]);
                if (cl >= 0xDC00 && cl <= 0xDFFF) {
                    // Valid pair: decode to code point in [U+10000, U+10FFFF]
                    const unsigned cp = 0x10000u
                        + ((cu - 0xD800u) << 10u)
                        + (cl - 0xDC00u);
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6)  & 0x3F));
                    out += static_cast<char>(0x80 | (cp         & 0x3F));
                    i += 2;
                    continue;
                }
            }
            // Unpaired high surrogate
            out += "\xEF\xBF\xBD"; // U+FFFD
            ++i;
            continue;
        }
        if (cu >= 0xDC00 && cu <= 0xDFFF) {
            // Unpaired low surrogate
            out += "\xEF\xBF\xBD"; // U+FFFD
            ++i;
            continue;
        }

        // BMP code point (char16_t is never > 0xFFFF)
        if (cu < 0x80) {
            out += static_cast<char>(cu);
        } else if (cu < 0x800) {
            out += static_cast<char>(0xC0 | (cu >> 6));
            out += static_cast<char>(0x80 | (cu & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cu >> 12));
            out += static_cast<char>(0x80 | ((cu >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cu        & 0x3F));
        }
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Normalise whitespace: collapse runs of blanks/newlines to single spaces,
// trim leading/trailing whitespace.  Keeps paragraph breaks (double newline)
// intact by doing a two-pass: first collapse intra-paragraph whitespace, then
// squeeze multi-blank-line runs.
// ---------------------------------------------------------------------------

std::string normalise_text(std::string text)
{
    // Replace \r with \n
    for (char& c : text)
        if (c == '\r') c = '\n';

    // Collapse runs of spaces/tabs to single space, but preserve newlines
    std::string step1;
    step1.reserve(text.size());
    bool last_space = false;
    for (char c : text) {
        if (c == ' ' || c == '\t') {
            if (!last_space) step1 += ' ';
            last_space = true;
        } else {
            step1 += c;
            last_space = (c == ' ');
        }
    }

    // Collapse 3+ consecutive newlines to 2 (preserve paragraph breaks)
    std::string step2;
    step2.reserve(step1.size());
    int nl_run = 0;
    for (char c : step1) {
        if (c == '\n') {
            ++nl_run;
            if (nl_run <= 2) step2 += c;
        } else {
            nl_run = 0;
            step2 += c;
        }
    }

    // Trim
    auto first = step2.find_first_not_of(" \n");
    auto last  = step2.find_last_not_of(" \n");
    if (first == std::string::npos) return {};
    return step2.substr(first, last - first + 1);
}

// ---------------------------------------------------------------------------
// Extract the concatenated text of all pages as a single UTF-8 string.
// Pages are separated by a newline.
// ---------------------------------------------------------------------------

std::string extract_all_text(poppler::document& doc)
{
    std::string out;
    int npages = doc.pages();
    for (int p = 0; p < npages; ++p) {
        auto page = doc.create_page(p);
        if (!page) continue;
        std::string page_text = ustring_to_utf8(page->text());
        if (!page_text.empty()) {
            if (!out.empty()) out += '\n';
            out += std::move(page_text);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Walk the poppler ToC and record heading titles with their depths.
// Depth 1 = top-level (root's direct children).
// We cap the *recorded* depth at 6 (matches HTML h1-h6 semantics) to avoid
// deeply nested technical PDFs producing 20-level section trees.
//
// Two adversarial caps:
//   kMaxTocDepth   — caps recursion to prevent stack overflow on a crafted
//                    PDF with deeply nested outline entries.
//   kMaxTocEntries — caps the total number of outline entries collected.
//                    Without this, a flat outline with many unmatched entries
//                    causes O(entries × text_size) work in split_by_toc and
//                    unbounded TocEntry allocation. Legitimate enterprise
//                    documents rarely have more than a few hundred outline
//                    entries; >2000 is a red flag (generated, adversarial, or
//                    not useful for chunking anyway).
// ---------------------------------------------------------------------------

static constexpr int         kMaxTocDepth   = 32;
static constexpr std::size_t kMaxTocEntries = 2000;

struct TocEntry {
    int         depth;   // 1..6
    std::string title;
};

// Returns true if all entries were collected (normal case).
// Returns false if kMaxTocEntries was hit — the caller must fall back to
// flat parsing rather than splitting on an incomplete outline, which would
// assign all remaining document body text to the last retained section.
bool walk_toc(poppler::toc_item* item, int depth, std::vector<TocEntry>& out)
{
    if (depth > kMaxTocDepth) return true;
    for (auto* child : item->children()) {
        if (out.size() >= kMaxTocEntries) return false;  // truncated
        std::string title = ustring_to_utf8(child->title());
        auto f = title.find_first_not_of(" \t\r\n");
        auto l = title.find_last_not_of(" \t\r\n");
        if (f != std::string::npos)
            title = title.substr(f, l - f + 1);
        if (!title.empty())
            out.push_back({std::min(depth, 6), std::move(title)});
        if (!walk_toc(child, depth + 1, out))
            return false;  // propagate truncation upward
    }
    return true;
}

// ---------------------------------------------------------------------------
// Given the full document text and a flat list of ToC entries, try to split
// the text into sections by progressively searching for each heading title
// (each searched only after the previous match end). Returns false if fewer
// than half the entries match, in which case the caller falls back to a flat
// section.
//
// Note on printed-ToC pages: PDFs commonly list all headings on a printed
// contents page before the body. Detecting that reliably requires PDF outline
// destinations (page numbers), which the poppler C++ `toc_item` API does not
// expose (only `title()`, `is_open()`, `children()` are available). Content-
// length and position-cluster heuristics both produce false positives on
// legitimate documents (glossaries, slide-style PDFs, documents with short
// opening sections followed by a long final section). We therefore do not
// attempt printed-ToC detection. The result for such PDFs is that the printed-
// ToC rows appear as sections with short bodies; the actual body text is
// mostly in the final section. That is still hierarchically richer than a
// single flat section and accurately reflects the outline structure that
// poppler extracted. A future implementation using the poppler C API or a
// library that exposes destinations can do better.
//
// Hierarchy: the depth field from TocEntry builds a proper parent-child tree.
// ---------------------------------------------------------------------------

bool split_by_toc(const std::string&          full_text,
                  const std::vector<TocEntry>& entries,
                  std::vector<ParsedSection>&  sections)
{
    if (entries.empty()) return false;

    // --- Helpers -----------------------------------------------------------
    auto normalise_for_cmp = [](std::string_view s) {
        std::string out;
        bool ws = false;
        for (unsigned char c : s) {
            if (std::isspace(c)) { ws = true; continue; }
            if (ws) { out += ' '; ws = false; }
            out += (char)std::tolower(c);
        }
        return out;
    };

    const std::string norm_text = normalise_for_cmp(full_text);

    // Build normalised -> original position map (walk both strings in sync).
    std::vector<std::size_t> norm_to_orig;
    norm_to_orig.reserve(norm_text.size());
    {
        bool ws_seen = false;
        std::size_t oi = 0;
        for (; oi < full_text.size(); ) {
            unsigned char c = (unsigned char)full_text[oi];
            if (std::isspace(c)) { ws_seen = true; ++oi; }
            else {
                if (ws_seen) { norm_to_orig.push_back(oi); ws_seen = false; }
                norm_to_orig.push_back(oi);
                ++oi;
            }
        }
        if (ws_seen && norm_to_orig.size() < norm_text.size())
            norm_to_orig.push_back(oi);
    }

    auto orig_pos_of = [&](std::size_t ni) -> std::size_t {
        return (ni < norm_to_orig.size()) ? norm_to_orig[ni] : full_text.size();
    };

    // Progressive search: find each title after the previous title's end.
    struct Match { std::size_t pos; int depth; std::string title; };
    std::vector<Match> matches;
    matches.reserve(entries.size());
    {
        std::size_t cursor = 0;
        for (const auto& e : entries) {
            auto nt = normalise_for_cmp(e.title);
            if (nt.empty()) continue;
            auto p = norm_text.find(nt, cursor);
            if (p == std::string::npos) continue;
            matches.push_back({p, e.depth, e.title});
            cursor = p + nt.size();
        }
    }

    // Require at least half the entries to match.
    if (matches.size() < entries.size() / 2 + 1)
        return false;

    // --- Build section tree using depth -----------------------------------
    // Optional preamble (text before the first heading)
    {
        std::size_t orig_end = orig_pos_of(matches[0].pos);
        std::string body = normalise_text(full_text.substr(0, orig_end));
        if (!body.empty()) {
            ParsedSection pre;
            pre.depth = 0;
            pre.text  = std::move(body);
            sections.push_back(std::move(pre));
        }
    }

    // Stack: open ancestors by depth (same approach as DocxParser / HtmlParser).
    struct Frame { int depth; ParsedSection* sec; };
    std::vector<Frame> stack;

    for (std::size_t i = 0; i < matches.size(); ++i) {
        const auto& m  = matches[i];
        auto nt        = normalise_for_cmp(m.title);
        std::size_t body_orig_start = orig_pos_of(m.pos + nt.size());
        std::size_t body_orig_end   = (i + 1 < matches.size())
                                        ? orig_pos_of(matches[i + 1].pos)
                                        : full_text.size();
        if (body_orig_start > body_orig_end) body_orig_start = body_orig_end;

        ParsedSection sec;
        sec.heading = m.title;
        sec.depth   = m.depth;
        sec.text    = normalise_text(full_text.substr(
                          body_orig_start, body_orig_end - body_orig_start));

        // Pop stack to the parent level
        while (!stack.empty() && stack.back().depth >= m.depth)
            stack.pop_back();

        if (stack.empty()) {
            sections.push_back(std::move(sec));
            stack.push_back({m.depth, &sections.back()});
        } else {
            ParsedSection* parent = stack.back().sec;
            parent->children.push_back(std::move(sec));
            stack.push_back({m.depth, &parent->children.back()});
        }
    }

    return !sections.empty();
}

} // namespace

// ---------------------------------------------------------------------------
// PdfParser::parse
// ---------------------------------------------------------------------------

Result<ParsedDocument> PdfParser::parse(const std::string& content,
                                         const std::string& filename,
                                         const std::string& /*mime_type*/) const
{
    // --- Input validation ---------------------------------------------------
    if (content.empty())
        return std::unexpected(Error::invalid_input("ingest.empty_file"));
    if (content.size() > ParserPort::kMaxInputBytes)
        return std::unexpected(Error::invalid_input("ingest.file_too_large"));

    // Magic byte check: PDF starts with %PDF-
    if (content.size() < 5
        || content[0] != '%' || content[1] != 'P' || content[2] != 'D'
        || content[3] != 'F' || content[4] != '-')
    {
        return std::unexpected(Error::invalid_input("ingest.mime_type_mismatch"));
    }

    // --- Load via poppler ---------------------------------------------------
    poppler::byte_array ba(content.begin(), content.end());
    std::unique_ptr<poppler::document> doc(
        poppler::document::load_from_data(&ba));

    if (!doc)
        return std::unexpected(Error::invalid_input("ingest.pdf.corrupt_or_empty"));
    if (doc->is_locked())
        return std::unexpected(Error::invalid_input("ingest.pdf.password_protected"));
    if (doc->pages() == 0)
        return std::unexpected(Error::invalid_input("ingest.pdf.no_pages"));

    // --- Extract text -------------------------------------------------------
    std::string full_text = extract_all_text(*doc);
    full_text = normalise_text(full_text);
    if (full_text.empty())
        return std::unexpected(Error::invalid_input("ingest.pdf.no_extractable_text"));

    // --- Build section structure via ToC ------------------------------------
    ParsedDocument out;
    out.filename  = filename;
    out.mime_type = "application/pdf";

    std::unique_ptr<poppler::toc> toc(doc->create_toc());
    bool structured = false;

    if (toc) {
        // toc->root() can return nullptr for a degenerate empty outline even
        // when create_toc() returns non-null. Guard before dereferencing.
        if (auto* root = toc->root()) {
            std::vector<TocEntry> entries;
            const bool complete = walk_toc(root, 1, entries);
            if (!complete) {
                // Outline was truncated at kMaxTocEntries. Parsing on a
                // partial outline would assign all remaining body text to
                // the last retained section. Fall through to flat section.
                spdlog::warn("[pdf-parser] outline for '{}' exceeded "
                             "kMaxTocEntries ({}); falling back to flat "
                             "section", filename, kMaxTocEntries);
            } else if (!entries.empty()) {
                structured = split_by_toc(full_text, entries, out.sections);
                if (!structured)
                    spdlog::debug("[pdf-parser] ToC present but heading match "
                                  "failed for '{}'; falling back to flat section",
                                  filename);
            }
        }
    }

    if (!structured) {
        // No ToC or matching failed: single flat section
        ParsedSection flat;
        flat.depth = 0;
        flat.text  = full_text;
        out.sections.push_back(std::move(flat));
    }

    // Build full_text as concatenation of all section bodies (depth-first)
    std::function<void(const ParsedSection&)> accum =
        [&](const ParsedSection& s) {
            if (!out.full_text.empty() && !s.text.empty())
                out.full_text += '\n';
            out.full_text += s.text;
            for (const auto& child : s.children)
                accum(child);
        };
    for (const auto& s : out.sections)
        accum(s);

    return out;
}

} // namespace wikore::ingest
