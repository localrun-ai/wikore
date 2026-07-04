#pragma once
#include "wikore/domain/types.hpp"
#include "wikore/ingest/types.hpp"
#include <cstddef>
#include <memory>
#include <string>

namespace wikore::ingest {

// ---------------------------------------------------------------------------
// ParserPort: abstract document parser.
//
// Implementations:
//   PlainTextParser  - plain text and Markdown (detects # headings)
//   PdfParser        - PDF via poppler-cpp (outline -> heading hierarchy;
//                      font-size heuristic fallback when no outline)
//   DocxParser       - Office Open XML (.docx) via minizip + pugixml
//                      (w:pStyle HeadingN detection, table flattening)
//   HtmlParser       - HTML via libxml2 (h1-h6 hierarchy, script/style strip)
// ---------------------------------------------------------------------------

class ParserPort {
public:
    virtual ~ParserPort() = default;

    static constexpr std::size_t kMaxInputBytes = 32U * 1024U * 1024U;

    // Parse raw content bytes into a structured ParsedDocument.
    // mime_type is advisory; the parser may inspect content bytes to override.
    // filename is used for ParsedDocument::filename and may guide parsing hints.
    virtual Result<ParsedDocument> parse(const std::string& content,
                                         const std::string& filename,
                                         const std::string& mime_type) const = 0;
};

// ---------------------------------------------------------------------------
// PlainTextParser: handles text/plain and text/markdown.
//
// Heading detection: lines starting with one or more '#' followed by a space
// (Markdown ATX headings). For plain text without any '#' headings the whole
// document is a single section with depth=0 and an empty heading.
//
// Also used for structured text formats routed as text/plain:
//   .json / .jsonl / .ndjson  — JSON data
//   .csv / .tsv               — tabular text data
//   .yaml / .yml              — YAML configuration / data
//   .rst / .adoc / .asciidoc  — lightweight markup (headings not detected)
//   .log / .conf / .ini / .toml / .env — operational text files
// These all produce a single flat section; section structure is rarely
// meaningful in these formats and the chunker handles length splitting.
// ---------------------------------------------------------------------------

class PlainTextParser : public ParserPort {
public:
    Result<ParsedDocument> parse(const std::string& content,
                                 const std::string& filename,
                                 const std::string& mime_type) const override;
};

// ---------------------------------------------------------------------------
// PdfParser: handles application/pdf.
//
// Heading strategy:
//   1. PDF outline (ToC). If the document has an outline with up to
//      kMaxTocEntries entries, the outline titles are used as section headings.
//      Section bodies are assigned by progressively searching for each title
//      in the extracted text: each title is sought only after the previous
//      match ends. Title search is case-insensitive and collapses internal
//      whitespace to handle poppler line-break artefacts.
//
//      Known limitation: if the PDF has a printed contents page, the first
//      occurrence of each heading title appears there rather than in the body.
//      The progressive search finds those ToC-page occurrences, so short
//      sections cluster on the contents page and most body text lands under
//      the final heading. Reliable boundary placement requires PDF outline
//      destinations (page numbers), which the poppler C++ toc_item API does
//      not expose; the poppler C API would be needed to implement that. For
//      now the outline-based split is accepted as a best-effort approach that
//      is richer than flat output for documents without a printed ToC.
//
//   2. Flat fallback. Used when: no outline exists, the outline exceeds
//      kMaxTocEntries (adversarial/generated PDFs), or fewer than half the
//      outline titles match anywhere in the extracted text.
//
// Password-protected, corrupt, or empty documents return Error::invalid_input.
// ---------------------------------------------------------------------------

class PdfParser : public ParserPort {
public:
    Result<ParsedDocument> parse(const std::string& content,
                                 const std::string& filename,
                                 const std::string& mime_type) const override;
};

// ---------------------------------------------------------------------------
// DocxParser: handles
//   application/vnd.openxmlformats-officedocument.wordprocessingml.document
//
// Extracts text from word/document.xml inside the ZIP container.
// Heading detection: w:pStyle with val matching "Heading1".."Heading9"
//   (locale-invariant; also matches "heading1" lower-case variants).
// Tables are flattened: cells separated by " | ", rows by newline.
// Tracked-change deletions (w:del) are suppressed; insertions (w:ins)
//   are included.
// Footnotes and endnotes (word/footnotes.xml, word/endnotes.xml) are
//   appended as a flat "Notes" section if present.
// ---------------------------------------------------------------------------

class DocxParser : public ParserPort {
public:
    Result<ParsedDocument> parse(const std::string& content,
                                 const std::string& filename,
                                 const std::string& mime_type) const override;
};

// ---------------------------------------------------------------------------
// PptxParser: handles
//   application/vnd.openxmlformats-officedocument.presentationml.presentation
//
// Slide order is read from ppt/_rels/presentation.xml.rels (falls back to
// numeric sort of ppt/slides/slide*.xml). Each slide becomes one
// ParsedSection at depth 1. The title shape (<p:ph type="title"> or
// <p:ph type="ctrTitle">) becomes the section heading; all other shape text
// is the section body. Speaker notes are not extracted.
// ---------------------------------------------------------------------------

class PptxParser : public ParserPort {
public:
    Result<ParsedDocument> parse(const std::string& content,
                                 const std::string& filename,
                                 const std::string& mime_type) const override;
};

// ---------------------------------------------------------------------------
// OdtParser: handles application/vnd.oasis.opendocument.text (.odt)
//
// Extracts text from content.xml inside the ZIP. Heading detection:
//   <text:h text:outline-level="N"> maps to ParsedSection depth N (1..6).
//   <text:p> becomes body text.
//   <table:table> is flattened: cells " | ", rows newline.
//   <text:list> items are prefixed with "- ".
//   <text:span> is transparent (text extracted, markup discarded).
// ---------------------------------------------------------------------------

class OdtParser : public ParserPort {
public:
    Result<ParsedDocument> parse(const std::string& content,
                                 const std::string& filename,
                                 const std::string& mime_type) const override;
};

// ---------------------------------------------------------------------------
// XlsxParser: handles
//   application/vnd.openxmlformats-officedocument.spreadsheetml.sheet
//
// Reads xl/workbook.xml for sheet order and names, xl/sharedStrings.xml for
// string cells, and each xl/worksheets/sheetN.xml for row data.
// Each worksheet becomes a ParsedSection at depth 1 (sheet name as heading).
// Rows are rendered as pipe-separated cell values; empty rows are skipped.
// Sparse rows (missing cells between A1 and C1) are padded with empty fields
// to preserve column alignment.
// Cell types handled: shared string (t="s"), numeric/formula (absent/t="str"),
// inline string (t="inlineStr"), boolean (t="b"), error (t="e").
// Security: 16 MiB per-entry cap, 128 MiB aggregate worksheet XML cap,
// 16 MiB global output text budget (cells + separators, shared across all
// sheets — prevents separator amplification from wide sparse rows),
// 500-sheet cap with path deduplication (all return explicit errors).
// All XML walks are iterative BFS with kXlsxXmlMaxDepth=64 depth limit.
// ---------------------------------------------------------------------------

class XlsxParser : public ParserPort {
public:
    Result<ParsedDocument> parse(const std::string& content,
                                 const std::string& filename,
                                 const std::string& mime_type) const override;
};

// ---------------------------------------------------------------------------
// HtmlParser: handles text/html.
//
// Uses libxml2's HTML parser (robust against real-world malformed markup).
// h1-h6 elements define the section hierarchy at depths 1-6.
// <script>, <style>, <head>, hidden elements (display:none, visibility:hidden,
//   aria-hidden="true") and HTML comments are stripped before text extraction.
// <table> cells are joined with " | "; rows separated by newline.
// <a> anchors keep their text; href is discarded.
// Output is UTF-8; the parser handles charset meta tags and BOM.
// ---------------------------------------------------------------------------

class HtmlParser : public ParserPort {
public:
    Result<ParsedDocument> parse(const std::string& content,
                                 const std::string& filename,
                                 const std::string& mime_type) const override;
};

// ---------------------------------------------------------------------------
// DispatchingParser — production routing layer.
//
// Calls resolve_text_mime() to determine the MIME type from the file extension
// and content magic bytes, then dispatches to the appropriate concrete parser.
// This is the parser to inject into IngestDocumentVersionUseCase in production.
// ---------------------------------------------------------------------------

class DispatchingParser : public ParserPort {
public:
    Result<ParsedDocument> parse(const std::string& content,
                                 const std::string& filename,
                                 const std::string& mime_type) const override;
};

} // namespace wikore::ingest
