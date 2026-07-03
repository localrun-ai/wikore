#include "wikore/ingest/parser.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <sstream>
#include <string_view>

namespace wikore::ingest {

namespace {

constexpr std::string_view kEicarPrefix = R"(X5O!P%@AP[4\PZX54(P^)7CC)7})";
constexpr std::string_view kEicarMarker = "EICAR-STANDARD-ANTIVIRUS-TEST-FILE";

std::string lower_ascii(std::string value)
{
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string extension_of(const std::string& filename)
{
    return lower_ascii(std::filesystem::path(filename).extension().string());
}

bool starts_with_bytes(const std::string& content, std::string_view magic)
{
    return content.size() >= magic.size()
        && std::equal(magic.begin(), magic.end(), content.begin());
}

Result<std::string> resolve_text_mime(const std::string& content,
                                      const std::string& filename,
                                      const std::string& mime_type)
{
    if (content.empty())
        return std::unexpected(Error::invalid_input("ingest.empty_file"));
    if (content.size() > ParserPort::kMaxInputBytes)
        return std::unexpected(Error::invalid_input("ingest.file_too_large"));
    const auto eicar_prefix = content.find(kEicarPrefix);
    if (eicar_prefix != std::string::npos
        && content.find(kEicarMarker, eicar_prefix + kEicarPrefix.size())
            != std::string::npos)
    {
        return std::unexpected(
            Error::invalid_input("ingest.av.eicar_signature_detected"));
    }

    const auto ext    = extension_of(filename);
    const bool is_pdf = starts_with_bytes(content, "%PDF-");
    const bool is_zip = starts_with_bytes(content, std::string_view{"PK\x03\x04", 4});

    if (ext == ".pdf" && !is_pdf)
        return std::unexpected(Error::invalid_input("ingest.mime_type_mismatch"));
    if ((ext == ".docx" || ext == ".xlsx" || ext == ".pptx"
         || ext == ".odp" || ext == ".ods") && !is_zip)
        return std::unexpected(Error::invalid_input("ingest.mime_type_mismatch"));
    if (is_pdf)
        return std::string{"application/pdf"};
    if (is_zip) {
        if (ext == ".docx")
            return std::string{
                "application/vnd.openxmlformats-officedocument"
                ".wordprocessingml.document"};
        if (ext == ".pptx")
            return std::string{
                "application/vnd.openxmlformats-officedocument"
                ".presentationml.presentation"};
        // xlsx/xls not yet supported
        return std::unexpected(Error::invalid_input("ingest.unsupported_format.office"));
    }
    if (content.find('\0') != std::string::npos)
        return std::unexpected(Error::invalid_input("ingest.binary_content"));

    if (ext == ".md" || ext == ".markdown")
        return std::string{"text/markdown"};
    if (ext == ".txt" || ext.empty())
        return std::string{"text/plain"};
    if (ext == ".html" || ext == ".htm")
        return std::string{"text/html"};
    if (ext == ".odt")
        return std::string{"application/vnd.oasis.opendocument.text"};

    if (mime_type == "text/markdown" || mime_type == "text/plain")
        return mime_type;
    if (mime_type == "text/html")
        return mime_type;
    return std::unexpected(Error::invalid_input("ingest.unsupported_format"));
}

bool decode_utf8(std::string_view input, std::size_t& offset,
                 char32_t& codepoint, std::size_t& width)
{
    const auto first = static_cast<unsigned char>(input[offset]);
    if (first < 0x80) {
        codepoint = first;
        width = 1;
        return true;
    }

    if ((first & 0xE0) == 0xC0) {
        codepoint = first & 0x1F;
        width = 2;
    } else if ((first & 0xF0) == 0xE0) {
        codepoint = first & 0x0F;
        width = 3;
    } else if ((first & 0xF8) == 0xF0) {
        codepoint = first & 0x07;
        width = 4;
    } else {
        return false;
    }

    if (offset + width > input.size())
        return false;
    for (std::size_t i = 1; i < width; ++i) {
        const auto next = static_cast<unsigned char>(input[offset + i]);
        if ((next & 0xC0) != 0x80)
            return false;
        codepoint = (codepoint << 6) | (next & 0x3F);
    }

    if ((width == 2 && codepoint < 0x80)
        || (width == 3 && codepoint < 0x800)
        || (width == 4 && codepoint < 0x10000)
        || codepoint > 0x10FFFF
        || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
    {
        return false;
    }
    return true;
}

Result<std::string> strip_unicode_tags(std::string_view input)
{
    std::string output;
    output.reserve(input.size());
    for (std::size_t offset = 0; offset < input.size();) {
        char32_t codepoint = 0;
        std::size_t width = 0;
        if (!decode_utf8(input, offset, codepoint, width))
            return std::unexpected(Error::invalid_input("ingest.invalid_utf8"));

        if (codepoint < 0xE0000 || codepoint > 0xE007F)
            output.append(input.substr(offset, width));
        offset += width;
    }
    return output;
}

bool has_boolean_attribute(std::string_view tag, std::string_view attribute,
                           std::size_t attributes_start)
{
    auto pos = tag.find(attribute, attributes_start);
    while (pos != std::string_view::npos) {
        const auto after = pos + attribute.size();
        const bool starts_token = pos > 0
            && std::isspace(static_cast<unsigned char>(tag[pos - 1]));
        const bool ends_token = after == tag.size()
            || std::isspace(static_cast<unsigned char>(tag[after]))
            || tag[after] == '=' || tag[after] == '/';
        if (starts_token && ends_token)
            return true;
        pos = tag.find(attribute, after);
    }
    return false;
}

bool is_void_html_element(std::string_view name)
{
    return name == "area" || name == "base" || name == "br"
        || name == "col" || name == "embed" || name == "hr"
        || name == "img" || name == "input" || name == "link"
        || name == "meta" || name == "source" || name == "track"
        || name == "wbr";
}

bool is_self_closing_tag(std::string_view tag)
{
    const auto end = tag.find_last_not_of(" \t\r\n");
    return end != std::string_view::npos && tag[end] == '/';
}

Result<std::string> strip_markdown_html(std::string input)
{
    const auto lower = lower_ascii(input);
    std::string output;
    output.reserve(input.size());

    for (std::size_t pos = 0; pos < input.size();) {
        if (input[pos] != '<') {
            output.push_back(input[pos++]);
            continue;
        }

        const auto tag_end = input.find('>', pos + 1);
        if (tag_end == std::string::npos) {
            output.append(input.substr(pos));
            break;
        }

        const auto tag = lower.substr(pos + 1, tag_end - pos - 1);
        std::size_t name_start = 0;
        if (name_start < tag.size() && tag[name_start] == '/')
            ++name_start;
        if (name_start >= tag.size()
            || !std::isalpha(static_cast<unsigned char>(tag[name_start])))
        {
            output.push_back(input[pos++]);
            continue;
        }
        std::size_t name_end = name_start;
        while (name_end < tag.size()
               && (std::isalnum(static_cast<unsigned char>(tag[name_end]))
                   || tag[name_end] == '-'))
            ++name_end;
        const auto name = tag.substr(name_start, name_end - name_start);

        const bool hidden = name == "script" || name == "style"
            || tag.find("display:none") != std::string::npos
            || tag.find("display: none") != std::string::npos
            || tag.find("visibility:hidden") != std::string::npos
            || tag.find("visibility: hidden") != std::string::npos
            || has_boolean_attribute(tag, "hidden", name_end);
        if (hidden && !name.empty() && tag.front() != '/') {
            if (is_self_closing_tag(tag) || is_void_html_element(name)) {
                pos = tag_end + 1;
                continue;
            }

            const auto close = "</" + name;
            const auto close_start = lower.find(close, tag_end + 1);
            if (close_start == std::string::npos)
                return std::unexpected(
                    Error::invalid_input("ingest.unclosed_hidden_tag"));
            const auto close_end = input.find('>', close_start + close.size());
            if (close_end == std::string::npos)
                return std::unexpected(
                    Error::invalid_input("ingest.unclosed_hidden_tag"));
            pos = close_end + 1;
            continue;
        }

        pos = tag_end + 1;
    }
    return output;
}

} // namespace

// ---------------------------------------------------------------------------
// PlainTextParser
//
// Detects ATX-style Markdown headings (lines beginning with 1-6 '#' chars
// followed by a space). Every heading starts a new section; text lines between
// headings are concatenated (newline-separated) into that section's body.
//
// Documents with no headings are treated as a single flat section (depth=0,
// empty heading) whose text is the full document.
// ---------------------------------------------------------------------------

Result<ParsedDocument>
PlainTextParser::parse(const std::string& content,
                        const std::string& filename,
                        const std::string& mime_type) const
{
    auto resolved_mime = resolve_text_mime(content, filename, mime_type);
    if (!resolved_mime)
        return std::unexpected(resolved_mime.error());

    // PlainTextParser only handles text/plain and text/markdown. Other MIME
    // types (application/pdf, application/vnd.openxmlformats-*) are returned
    // by resolve_text_mime when the magic bytes match; the caller should have
    // used the appropriate parser.
    if (*resolved_mime != "text/plain" && *resolved_mime != "text/markdown")
        return std::unexpected(Error::invalid_input("ingest.unsupported_format"));

    auto normalized = strip_unicode_tags(content);
    if (!normalized)
        return std::unexpected(normalized.error());
    if (*resolved_mime == "text/markdown") {
        auto sanitized = strip_markdown_html(std::move(*normalized));
        if (!sanitized)
            return std::unexpected(sanitized.error());
        *normalized = std::move(*sanitized);
    }

    ParsedDocument doc;
    doc.filename  = filename;
    doc.mime_type = *resolved_mime;

    std::istringstream stream(*normalized);
    std::string        line;

    // Working state: current heading depth and the section being assembled
    struct PendingSection {
        std::string heading;
        int         depth = 0;
        std::string text;
    };

    std::vector<PendingSection> pending; // one per top-level + nested heading in order

    auto flush_to_doc = [&]() {
        // Convert flat pending list into a section tree.
        // We stack sections by depth, nesting shallower ones into deeper ones.
        std::vector<ParsedSection*> stack; // stack[i] = last section at depth i+1

        for (auto& p : pending) {
            ParsedSection sec;
            sec.heading = p.heading;
            sec.depth   = p.depth;
            sec.text    = p.text;

            if (p.depth == 0 || stack.empty()) {
                doc.sections.push_back(std::move(sec));
                stack.clear();
                stack.push_back(&doc.sections.back());
            } else {
                // Pop until we find an ancestor with strictly smaller depth.
                while (!stack.empty() && stack.back()->depth >= p.depth)
                    stack.pop_back();

                if (stack.empty()) {
                    // Same or higher level than every ancestor: top-level section.
                    doc.sections.push_back(std::move(sec));
                    stack.push_back(&doc.sections.back());
                } else {
                    stack.back()->children.push_back(std::move(sec));
                    stack.push_back(&stack.back()->children.back());
                }
            }
        }
    };

    PendingSection current{};
    bool           has_headings = false;

    while (std::getline(stream, line)) {
        // std::getline strips the line-terminating '\n' but leaves the '\r'
        // from CRLF terminators. Without this, every heading and body line
        // from a Windows-style file would carry a trailing '\r', polluting
        // heading_path (used for the section path string), chunk text, and
        // any downstream consumer that does substring comparison on
        // headings (e.g. authoritative-quote matching in reranking).
        //
        // Use `while` not `if`: some double-conversion toolchains (a
        // Windows tool re-emitting a CRLF file in text mode) produce
        // \r\r\n terminators; getline leaves \r\r, and a single pop_back
        // would still surface a stray \r downstream.
        while (!line.empty() && line.back() == '\r')
            line.pop_back();

        // Detect ATX heading: 1-6 '#' then a space
        int hashes = 0;
        while (hashes < static_cast<int>(line.size()) && line[hashes] == '#')
            ++hashes;

        if (hashes >= 1 && hashes <= 6
            && hashes < static_cast<int>(line.size())
            && line[hashes] == ' ')
        {
            // Save previous section
            if (has_headings || !current.text.empty())
                pending.push_back(current);

            has_headings    = true;
            current.heading = line.substr(hashes + 1);
            current.depth   = hashes;
            current.text    = {};
        } else {
            if (!current.text.empty())
                current.text += '\n';
            current.text += line;
        }
    }

    // Flush final section
    pending.push_back(current);

    if (!has_headings) {
        // No headings: single flat section
        ParsedSection flat;
        flat.heading = {};
        flat.depth   = 0;
        flat.text    = current.text;
        doc.sections.push_back(std::move(flat));
    } else {
        flush_to_doc();
    }

    // Build full_text as concatenation of all section texts (depth-first)
    std::function<void(const ParsedSection&)> append_text =
        [&](const ParsedSection& s) {
            if (!doc.full_text.empty() && !s.text.empty())
                doc.full_text += '\n';
            doc.full_text += s.text;
            for (const auto& c : s.children)
                append_text(c);
        };
    for (const auto& s : doc.sections)
        append_text(s);

    return doc;
}

} // namespace wikore::ingest
