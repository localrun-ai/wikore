// ---------------------------------------------------------------------------
// DocxParser — Office Open XML (.docx) text extraction.
//
// A .docx file is a ZIP archive (ECMA-376 / ISO 29500).  The main document
// body lives in word/document.xml; footnotes and endnotes live in
// word/footnotes.xml and word/endnotes.xml.  This parser:
//
//   1. Unzips the three XML files from the archive in memory (no temp file).
//   2. Parses each with pugixml (SAX-like fast parse, zero-copy DOM).
//   3. Walks <w:p> elements to extract paragraphs:
//        * Heading level from <w:pPr><w:pStyle w:val="HeadingN"/> (1-9).
//          Also accepts lower-case variants ("heading1") and style names
//          with spaces ("Heading 1") because real producers vary.
//        * Text from all <w:r>/<w:t> runs within the paragraph.
//        * Tracked-change deletions (<w:del>) are suppressed.
//        * Tracked-change insertions (<w:ins>) are included.
//   4. <w:tbl> tables are flattened: cells separated by " | ", rows by "\n".
//   5. Footnotes/endnotes, if present, are appended as a flat "Notes" section
//      at depth 1 so they are available to the chunker without polluting the
//      main section tree.
//   6. Builds the section hierarchy: consecutive non-heading paragraphs are
//      appended as body text to the innermost open section.
//
// Thread safety: DocxParser is stateless.  Multiple threads may call parse()
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

// ---------------------------------------------------------------------------
// In-memory minizip helpers.
//
// minizip's unzOpen2 accepts a custom I/O backend.  We implement a minimal
// read-only backend backed by a std::string so no temp file is needed.
//
// Security properties maintained by this backend:
//   * ms_seek clamps pos to [0, size] — a crafted ZIP whose local-file
//     offsets point beyond EOF cannot cause ms_read to compute a negative
//     avail (which would underflow to ~0 and trigger an OOB memcpy).
//   * ms_read guards pos >= size before computing avail, so a seek that
//     lands exactly at size returns 0 bytes rather than underflowing.
//   * zip_extract caps uncompressed_size at kMaxXmlBytes before allocating,
//     so a zip-bomb whose local header advertises several GB of expanded data
//     cannot exhaust process memory.  The actual decompressed data is read in
//     one bounded chunk and the return value is checked for short/failed reads.
// ---------------------------------------------------------------------------

// Maximum size for any single extracted XML entry (16 MiB).  This is well
// above the largest realistic word/document.xml (typical max ~1 MiB) and
// well below the process memory limit in any normal deployment.
static constexpr std::size_t kMaxXmlBytes = 16UL * 1024UL * 1024UL;

struct MemStream {
    const char* data;
    std::size_t size;
    std::size_t pos;
};

static voidpf ms_open(voidpf /*opaque*/, const char* filename, int /*mode*/)
{
    // filename is actually a pointer to our MemStream cast to const char*.
    return (voidpf)(std::uintptr_t)filename;
}

static uLong ms_read(voidpf /*opaque*/, voidpf stream, void* buf, uLong n)
{
    auto* ms = static_cast<MemStream*>(stream);
    if (ms->pos >= ms->size) return 0;                    // at or past EOF
    auto  avail = static_cast<uLong>(ms->size - ms->pos); // safe: pos < size
    uLong r = (n < avail) ? n : avail;
    std::memcpy(buf, ms->data + ms->pos, r);
    ms->pos += r;
    return r;
}

static uLong ms_write(voidpf, voidpf, const void*, uLong)
{
    return 0; // read-only; write is never called by unzip
}

static long ms_tell(voidpf /*opaque*/, voidpf stream)
{
    return static_cast<long>(static_cast<MemStream*>(stream)->pos);
}

static long ms_seek(voidpf /*opaque*/, voidpf stream, uLong offset, int origin)
{
    auto* ms = static_cast<MemStream*>(stream);
    std::size_t new_pos;
    if (origin == ZLIB_FILEFUNC_SEEK_SET) {
        new_pos = static_cast<std::size_t>(offset);
    } else if (origin == ZLIB_FILEFUNC_SEEK_CUR) {
        // Guard against wrapping: if adding offset would exceed SIZE_MAX,
        // clamp to size (which ms_read treats as EOF).
        new_pos = (static_cast<std::size_t>(offset) > ms->size - ms->pos)
                    ? ms->size
                    : ms->pos + static_cast<std::size_t>(offset);
    } else { // ZLIB_FILEFUNC_SEEK_END
        // offset is treated as non-negative by minizip's unzip.c for this
        // backend; clamp the result to [0, size].
        new_pos = (static_cast<std::size_t>(offset) > ms->size)
                    ? 0
                    : ms->size - static_cast<std::size_t>(offset);
    }
    // Clamp: minizip may seek to exactly size (one past last byte); that is
    // valid (it reads 0 bytes from there).  Seeking past size is an error in
    // a valid ZIP, so clamping is the safe choice.
    ms->pos = (new_pos > ms->size) ? ms->size : new_pos;
    return 0;
}

static int ms_close(voidpf, voidpf) { return 0; }
static int ms_err  (voidpf, voidpf) { return 0; }

// Extract the contents of `entry` from the ZIP `zip`.
// Returns an empty string if the entry does not exist (optional files like
// footnotes.xml) or if extraction fails for any reason, including:
//   * uncompressed size > kMaxXmlBytes (zip-bomb defence)
//   * short / failed read from minizip
//   * minizip API errors
std::string zip_extract(const std::string& zip, const char* entry)
{
    MemStream ms{zip.data(), zip.size(), 0};
    zlib_filefunc_def ff{
        ms_open, ms_read, ms_write, ms_tell, ms_seek, ms_close, ms_err,
        nullptr};
    unzFile uf = unzOpen2(reinterpret_cast<const char*>(&ms), &ff);
    if (!uf) return {};

    if (unzLocateFile(uf, entry, 1) != UNZ_OK) {
        unzClose(uf);
        return {};
    }

    unz_file_info fi{};
    if (unzGetCurrentFileInfo(uf, &fi, nullptr, 0, nullptr, 0, nullptr, 0)
            != UNZ_OK) {
        unzClose(uf);
        return {};
    }

    // Zip-bomb defence: reject entries whose stated uncompressed size exceeds
    // our XML cap. The compressed input is already bounded by kMaxInputBytes;
    // this caps how far minizip can inflate it.
    if (fi.uncompressed_size > static_cast<uLong>(kMaxXmlBytes)) {
        spdlog::warn("[docx-parser] entry '{}' claims uncompressed_size={} "
                     "> kMaxXmlBytes ({}); rejecting as potential zip-bomb",
                     entry, fi.uncompressed_size, kMaxXmlBytes);
        unzClose(uf);
        return {};
    }

    if (unzOpenCurrentFile(uf) != UNZ_OK) {
        unzClose(uf);
        return {};
    }

    const auto alloc_size = static_cast<std::size_t>(fi.uncompressed_size);
    std::string out(alloc_size, '\0');

    // Read and verify: unzReadCurrentFile returns the number of bytes read
    // (>= 0) or a negative error code.  A short read means decompression
    // failed or the entry is truncated — treat as unrecoverable.
    const int bytes_read = unzReadCurrentFile(
        uf, out.data(), static_cast<unsigned>(alloc_size));
    unzCloseCurrentFile(uf);
    unzClose(uf);

    if (bytes_read < 0
        || static_cast<std::size_t>(bytes_read) != alloc_size) {
        spdlog::warn("[docx-parser] short/failed read for '{}': "
                     "got {} bytes, expected {}",
                     entry, bytes_read, alloc_size);
        return {};
    }

    return out;
}

// ---------------------------------------------------------------------------
// Heading-style detection.
//
// Returns 0 if not a heading, 1-9 otherwise.
// Matches:
//   "Heading1", "heading1", "Heading 1", "heading 1",
//   "Heading2" .. "Heading9" and their spaced / lower-case variants.
// ---------------------------------------------------------------------------

int parse_heading_level(const char* style_val)
{
    if (!style_val || !*style_val) return 0;

    std::string s = style_val;
    // Lower-case compare
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Remove a single optional space between "heading" and the digit
    const std::string_view prefix = "heading";
    if (s.size() < prefix.size() + 1) return 0;
    if (s.substr(0, prefix.size()) != prefix) return 0;
    std::string_view rest = std::string_view(s).substr(prefix.size());
    if (rest.size() == 2 && rest[0] == ' ') rest = rest.substr(1);
    if (rest.size() != 1 || rest[0] < '1' || rest[0] > '9') return 0;
    return rest[0] - '0';
}

// ---------------------------------------------------------------------------
// Normalise a UTF-8 text string from pugixml.
// Replaces complete multi-byte sequences for specific Unicode characters:
//   U+00A0 NBSP        (UTF-8: C2 A0) -> ASCII space
//   U+00AD Soft Hyphen (UTF-8: C2 AD) -> dropped
// Processing individual bytes would corrupt UTF-8: discarding the second byte
// of C2 A0 / C2 AD leaves a dangling C2 lead byte and invalid UTF-8 output.
// ---------------------------------------------------------------------------

std::string normalise_xml_text(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        // Check for the specific 2-byte sequences we normalise.
        if (b0 == 0xC2 && i + 1 < s.size()) {
            const auto b1 = static_cast<unsigned char>(s[i + 1]);
            if (b1 == 0xA0) {          // U+00A0 NBSP -> space
                out += ' ';
                i += 2;
                continue;
            }
            if (b1 == 0xAD) {          // U+00AD Soft Hyphen -> drop
                i += 2;
                continue;
            }
        }
        out += static_cast<char>(b0);
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Text extraction from a single <w:p> node.
// Suppresses w:del children; includes w:ins and plain w:r.
// Text from pugixml is UTF-8; normalise_xml_text handles NBSP and soft hyphen.
// ---------------------------------------------------------------------------

std::string extract_para_text(const pugi::xml_node& para)
{
    std::string out;
    for (const auto& child : para.children()) {
        std::string_view cn = child.name();
        if (cn == "w:del") continue;
        if (cn == "w:r") {
            for (const auto& t : child.children("w:t"))
                out += normalise_xml_text(t.text().get());
        } else if (cn == "w:ins") {
            for (const auto& r : child.children("w:r"))
                for (const auto& t : r.children("w:t"))
                    out += normalise_xml_text(t.text().get());
        } else if (cn == "w:hyperlink" || cn == "w:fldSimple"
                   || cn == "w:smartTag") {
            // Recurse into inline containers
            for (const auto& r : child.children("w:r"))
                for (const auto& t : r.children("w:t"))
                    out += normalise_xml_text(t.text().get());
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Flatten a <w:tbl> into text.
// Cells are separated by " | "; rows end with "\n".
// An empty cell emits an empty field so column alignment is preserved.
// ---------------------------------------------------------------------------

std::string extract_table_text(const pugi::xml_node& tbl)
{
    std::string out;
    for (const auto& row : tbl.children("w:tr")) {
        bool first = true;
        for (const auto& cell : row.children("w:tc")) {
            if (!first) out += " | ";
            first = false;
            // A cell contains one or more <w:p>
            bool cell_first_para = true;
            for (const auto& p : cell.children("w:p")) {
                if (!cell_first_para) out += ' ';
                out += extract_para_text(p);
                cell_first_para = false;
            }
        }
        out += '\n';
    }
    return out;
}

// ---------------------------------------------------------------------------
// Walk the <w:body> and build the ParsedSection list.
//
// Algorithm (single-pass, O(n)):
//   Maintain a "section stack" that tracks the currently open section at each
//   depth.  When a heading paragraph is encountered at depth D:
//     * Pop the stack back to depth D-1 (the new section's parent).
//     * Push a new section at depth D.
//   Body text is appended to the top-of-stack section.
//
//   Top-level sections (those at depth 1) are placed directly into `out`.
//   Deeper sections are placed in the `children` vector of the section
//   immediately above them.
// ---------------------------------------------------------------------------

void parse_body(const pugi::xml_node& body, std::vector<ParsedSection>& out)
{
    // Stack of (depth, pointer-to-section).  The pointer is stable because
    // we only append — never insert — to `out` and its `children` vectors.
    struct Frame { int depth; ParsedSection* sec; };
    std::vector<Frame> stack;

    auto top_section = [&]() -> ParsedSection* {
        return stack.empty() ? nullptr : stack.back().sec;
    };

    auto append_text = [&](const std::string& text) {
        if (text.empty()) return;
        ParsedSection* s = top_section();
        if (!s) {
            // Text before the first heading: create an implicit depth-0 section
            out.emplace_back();
            stack.push_back({0, &out.back()});
            s = &out.back();
        }
        if (!s->text.empty()) s->text += '\n';
        s->text += text;
    };

    for (const auto& child : body.children()) {
        std::string_view cn = child.name();

        if (cn == "w:p") {
            int level = parse_heading_level(
                child.child("w:pPr").child("w:pStyle").attribute("w:val").value());

            if (level > 0) {
                std::string heading = extract_para_text(child);
                if (heading.empty()) continue; // blank heading — skip

                // Pop stack to the level above this one
                while (!stack.empty() && stack.back().depth >= level)
                    stack.pop_back();

                ParsedSection sec;
                sec.heading = std::move(heading);
                sec.depth   = level;

                if (stack.empty()) {
                    // Top-level section
                    out.push_back(std::move(sec));
                    stack.push_back({level, &out.back()});
                } else {
                    // Child of the current top
                    ParsedSection* parent = stack.back().sec;
                    parent->children.push_back(std::move(sec));
                    stack.push_back({level, &parent->children.back()});
                }
            } else {
                std::string text = extract_para_text(child);
                append_text(text);
            }
        } else if (cn == "w:tbl") {
            append_text(extract_table_text(child));
        }
        // w:sdt (structured document tags), w:bookmarkStart, etc. are ignored.
    }
}

// ---------------------------------------------------------------------------
// Parse one XML string (document, footnotes, or endnotes) and append sections.
// ---------------------------------------------------------------------------

bool parse_xml_document(const std::string&         xml,
                         std::vector<ParsedSection>& sections,
                         std::string_view            context)
{
    pugi::xml_document doc;
    auto result = doc.load_buffer(xml.data(), xml.size(),
                                   pugi::parse_default | pugi::parse_ws_pcdata_single);
    if (!result) {
        spdlog::warn("[docx-parser] XML parse error in {}: {}", context,
                     result.description());
        return false;
    }

    // word/document.xml: body is w:document/w:body
    auto body = doc.child("w:document").child("w:body");
    if (body) {
        parse_body(body, sections);
        return true;
    }

    // word/footnotes.xml: root is <w:footnotes>, children are <w:footnote>
    // word/endnotes.xml:  root is <w:endnotes>,  children are <w:endnote>
    // parse_body processes direct w:p / w:tbl children; it does NOT descend
    // into w:footnote / w:endnote wrappers.  Iterate the wrappers explicitly.
    auto notes_root = doc.child("w:footnotes");
    if (!notes_root)
        notes_root = doc.child("w:endnotes");
    if (notes_root) {
        for (const auto& note : notes_root.children()) {
            std::string_view nn = note.name();
            if (nn != "w:footnote" && nn != "w:endnote") continue;
            // Each wrapper contains w:p / w:tbl directly
            parse_body(note, sections);
        }
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Build full_text from a section tree (depth-first).
// ---------------------------------------------------------------------------

void accum_text(const ParsedSection& s, std::string& out)
{
    if (!out.empty() && !s.text.empty()) out += '\n';
    out += s.text;
    for (const auto& c : s.children) accum_text(c, out);
}

} // namespace

// ---------------------------------------------------------------------------
// DocxParser::parse
// ---------------------------------------------------------------------------

Result<ParsedDocument> DocxParser::parse(const std::string& content,
                                           const std::string& filename,
                                           const std::string& /*mime_type*/) const
{
    // --- Input validation ---------------------------------------------------
    if (content.empty())
        return std::unexpected(Error::invalid_input("ingest.empty_file"));
    if (content.size() > ParserPort::kMaxInputBytes)
        return std::unexpected(Error::invalid_input("ingest.file_too_large"));

    // Magic bytes for ZIP: PK\x03\x04
    if (content.size() < 4
        || content[0] != 'P' || content[1] != 'K'
        || content[2] != '\x03' || content[3] != '\x04')
    {
        return std::unexpected(Error::invalid_input("ingest.mime_type_mismatch"));
    }

    // --- Extract XML entries ------------------------------------------------
    auto doc_xml = zip_extract(content, "word/document.xml");
    if (doc_xml.empty())
        return std::unexpected(
            Error::invalid_input("ingest.docx.missing_document_xml"));

    // --- Parse --------------------------------------------------------------
    ParsedDocument out;
    out.filename  = filename;
    out.mime_type = "application/vnd.openxmlformats-officedocument"
                    ".wordprocessingml.document";

    if (!parse_xml_document(doc_xml, out.sections, "word/document.xml"))
        return std::unexpected(Error::invalid_input("ingest.docx.parse_failed"));

    // Footnotes / endnotes (optional)
    auto footnotes_xml = zip_extract(content, "word/footnotes.xml");
    auto endnotes_xml  = zip_extract(content, "word/endnotes.xml");

    std::vector<ParsedSection> notes;
    if (!footnotes_xml.empty())
        parse_xml_document(footnotes_xml, notes, "word/footnotes.xml");
    if (!endnotes_xml.empty())
        parse_xml_document(endnotes_xml, notes, "word/endnotes.xml");

    if (!notes.empty()) {
        // Merge all note text into a single "Notes" section at depth 1.
        ParsedSection notes_sec;
        notes_sec.heading = "Notes";
        notes_sec.depth   = 1;
        for (const auto& ns : notes) {
            if (!ns.text.empty()) {
                if (!notes_sec.text.empty()) notes_sec.text += '\n';
                notes_sec.text += ns.text;
            }
            for (const auto& nc : ns.children) accum_text(nc, notes_sec.text);
        }
        if (!notes_sec.text.empty())
            out.sections.push_back(std::move(notes_sec));
    }

    if (out.sections.empty())
        return std::unexpected(Error::invalid_input("ingest.docx.no_text_content"));

    // Build full_text
    for (const auto& s : out.sections)
        accum_text(s, out.full_text);

    return out;
}

} // namespace wikore::ingest
