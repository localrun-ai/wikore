// ---------------------------------------------------------------------------
// PptxParser — PowerPoint Open XML (.pptx) text extraction.
//
// A .pptx file is a ZIP archive (ECMA-376 / ISO 29500). Slides live in
// ppt/slides/slideN.xml. This parser:
//
//   1. Reads the slide order from ppt/_rels/presentation.xml.rels, which
//      maps rId -> slide filename. The order in ppt/presentation.xml's
//      <p:sldIdLst> is the canonical slide order; we use the relationship
//      file to resolve filenames.  Falls back to numeric sort of
//      ppt/slides/slide*.xml if the relationship entries cannot be resolved.
//
//   2. For each slide, extracts:
//        * Title text from shapes with <p:ph type="title"> or
//          <p:ph type="ctrTitle">. Each slide title becomes the heading of
//          a ParsedSection at depth 1.
//        * Body text from all other shapes (type "body", idx >= 1, or no
//          placeholder at all). Text runs (<a:t>) are concatenated; paragraphs
//          are separated by newlines.
//
//   3. Slides without a title produce a section with an empty heading at
//      depth 1 (treated as untitled slide content).
//
//   4. Speaker notes (ppt/notesSlides/) are not extracted — they are rarely
//      authoritative content for RAG purposes.
//
// Thread safety: PptxParser is stateless. Multiple threads may call parse()
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
#include <unordered_set>
#include <vector>

namespace wikore::ingest {

namespace {

// Maximum number of slides to process. Prevents an attacker-controlled
// relationship file from referencing the same (or many) slides repeatedly
// and causing unbounded decompression and CPU work.
static constexpr std::size_t kMaxPptxSlides = 2000;

// Maximum total bytes of decompressed slide XML across all slides. Each
// slide may expand to kPptxMaxXmlBytes (16 MiB) individually; without an
// aggregate cap, 2000 slides × 16 MiB = 32 GB of aggregate work.
// 128 MiB comfortably covers the largest legitimate deck we expect.
static constexpr std::size_t kPptxMaxTotalXmlBytes = 128UL * 1024UL * 1024UL;

// Shared in-memory ZIP backend — identical to the one in docx_parser.cpp.
// Defined here independently to keep parsers self-contained.

static constexpr std::size_t kPptxMaxXmlBytes = 16UL * 1024UL * 1024UL;

struct PptxMemStream { const char* data; std::size_t size; std::size_t pos; };

static voidpf pptx_ms_open(voidpf, const char* fn, int)
    { return (voidpf)(std::uintptr_t)fn; }
static uLong pptx_ms_read(voidpf, voidpf s, void* buf, uLong n)
{
    auto* ms = static_cast<PptxMemStream*>(s);
    if (ms->pos >= ms->size) return 0;
    auto avail = static_cast<uLong>(ms->size - ms->pos);
    uLong r = (n < avail) ? n : avail;
    std::memcpy(buf, ms->data + ms->pos, r);
    ms->pos += r;
    return r;
}
static uLong pptx_ms_write(voidpf, voidpf, const void*, uLong) { return 0; }
static long  pptx_ms_tell (voidpf, voidpf s)
    { return static_cast<long>(static_cast<PptxMemStream*>(s)->pos); }
static long  pptx_ms_seek (voidpf, voidpf s, uLong off, int orig)
{
    auto* ms = static_cast<PptxMemStream*>(s);
    std::size_t np;
    if (orig == ZLIB_FILEFUNC_SEEK_SET)
        np = off;
    else if (orig == ZLIB_FILEFUNC_SEEK_CUR)
        np = (off > ms->size - ms->pos) ? ms->size : ms->pos + off;
    else
        np = (off > ms->size) ? 0 : ms->size - off;
    ms->pos = (np > ms->size) ? ms->size : np;
    return 0;
}
static int pptx_ms_close(voidpf, voidpf) { return 0; }
static int pptx_ms_err  (voidpf, voidpf) { return 0; }

std::string pptx_extract(const std::string& zip, const char* entry)
{
    PptxMemStream ms{zip.data(), zip.size(), 0};
    zlib_filefunc_def ff{pptx_ms_open, pptx_ms_read, pptx_ms_write,
                         pptx_ms_tell, pptx_ms_seek, pptx_ms_close,
                         pptx_ms_err, nullptr};
    unzFile uf = unzOpen2(reinterpret_cast<const char*>(&ms), &ff);
    if (!uf) return {};
    if (unzLocateFile(uf, entry, 1) != UNZ_OK) { unzClose(uf); return {}; }
    unz_file_info fi{};
    if (unzGetCurrentFileInfo(uf, &fi, nullptr, 0, nullptr, 0, nullptr, 0)
            != UNZ_OK) { unzClose(uf); return {}; }
    if (fi.uncompressed_size > static_cast<uLong>(kPptxMaxXmlBytes))
    {
        spdlog::warn("[pptx-parser] entry '{}' uncompressed_size={} > cap",
                     entry, fi.uncompressed_size);
        unzClose(uf); return {};
    }
    if (unzOpenCurrentFile(uf) != UNZ_OK) { unzClose(uf); return {}; }
    const auto sz = static_cast<std::size_t>(fi.uncompressed_size);
    std::string out(sz, '\0');
    const int got = unzReadCurrentFile(uf, out.data(), static_cast<unsigned>(sz));
    unzCloseCurrentFile(uf);
    unzClose(uf);
    if (got < 0 || static_cast<std::size_t>(got) != sz) return {};
    return out;
}

// Enumerate all entries in the ZIP that start with `prefix` and end with `.xml`.
// Returns the matching filenames (full paths, e.g. "ppt/slides/slide3.xml").
std::vector<std::string> pptx_list_entries(const std::string& zip,
                                            std::string_view   prefix)
{
    PptxMemStream ms{zip.data(), zip.size(), 0};
    zlib_filefunc_def ff{pptx_ms_open, pptx_ms_read, pptx_ms_write,
                         pptx_ms_tell, pptx_ms_seek, pptx_ms_close,
                         pptx_ms_err, nullptr};
    unzFile uf = unzOpen2(reinterpret_cast<const char*>(&ms), &ff);
    if (!uf) return {};

    std::vector<std::string> found;
    unz_global_info gi{};
    unzGetGlobalInfo(uf, &gi);
    unzGoToFirstFile(uf);
    for (uLong i = 0; i < gi.number_entry; ++i) {
        char fname[512]{};
        unzGetCurrentFileInfo(uf, nullptr, fname, sizeof(fname),
                              nullptr, 0, nullptr, 0);
        std::string_view sv(fname);
        if (sv.starts_with(prefix) && sv.ends_with(".xml"))
            found.push_back(std::string(sv));
        if (i + 1 < gi.number_entry)
            unzGoToNextFile(uf);
    }
    unzClose(uf);
    return found;
}

// Parse the slide relationship file to get the ordered list of slide filenames.
// Returns paths relative to the ZIP root (e.g. "ppt/slides/slide2.xml").
// If the relationship file cannot be parsed, returns an empty vector.
std::vector<std::string> slide_order_from_rels(const std::string& zip)
{
    auto prs_xml = pptx_extract(zip, "ppt/presentation.xml");
    auto rels_xml = pptx_extract(zip, "ppt/_rels/presentation.xml.rels");
    if (prs_xml.empty() || rels_xml.empty()) return {};

    // Parse relationships: map rId -> slide filename.
    // The OOXML relationship namespace is the default namespace in most
    // documents, so elements appear as "Relationships"/"Relationship" without
    // a prefix. However, a document is also valid if it uses an explicit
    // prefix (e.g. "r:Relationships"). child("Relationships") in pugixml
    // performs an exact name match including prefix; "r:Relationships" would
    // not match. Walk by local name to handle both forms.
    pugi::xml_document rels_doc;
    if (!rels_doc.load_buffer(rels_xml.data(), rels_xml.size())) return {};

    const std::string_view slide_type =
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide";
    std::unordered_map<std::string, std::string> rid_to_file;

    // Find the root Relationships element by local name.
    pugi::xml_node rels_root;
    for (const auto& n : rels_doc.children()) {
        std::string_view nn = n.name();
        auto nc = nn.rfind(':');
        if ((nc != std::string_view::npos ? nn.substr(nc+1) : nn) == "Relationships") {
            rels_root = n; break;
        }
    }
    if (rels_root) {
        for (const auto& rel : rels_root.children()) {
            std::string_view rn = rel.name();
            auto rc = rn.rfind(':');
            if ((rc != std::string_view::npos ? rn.substr(rc+1) : rn)
                    != "Relationship") continue;
            if (std::string_view(rel.attribute("Type").value()) == slide_type) {
                std::string target = rel.attribute("Target").value();
                std::string path = target.starts_with("/")
                    ? target.substr(1)
                    : "ppt/" + target;
                rid_to_file[rel.attribute("Id").value()] = std::move(path);
            }
        }
    }
    if (rid_to_file.empty()) return {};

    // Parse presentation.xml to get the sldIdLst order.
    pugi::xml_document prs_doc;
    if (!prs_doc.load_buffer(prs_xml.data(), prs_xml.size())) return {};

    std::vector<std::string> order;
    // Walk the sldIdLst; the presentation namespace prefix varies, so search
    // for any element whose local name contains "sldIdLst"/"sldId".
    std::function<bool(const pugi::xml_node&)> find_list =
        [&](const pugi::xml_node& n) -> bool {
        for (const auto& child : n.children()) {
            std::string_view cn = child.name();
            // Strip namespace prefix
            auto colon = cn.rfind(':');
            auto local = (colon != std::string_view::npos)
                         ? cn.substr(colon + 1) : cn;
            if (local == "sldIdLst") {
                for (const auto& sid : child.children()) {
                    for (const auto& attr : sid.attributes()) {
                        if (std::string_view(attr.name()).find("id")
                                != std::string_view::npos
                            && std::string_view(attr.name()).rfind(':')
                                != std::string_view::npos) {
                            auto it = rid_to_file.find(attr.value());
                            if (it != rid_to_file.end())
                                order.push_back(it->second);
                        }
                    }
                }
                return true;
            }
            if (find_list(child)) return true;
        }
        return false;
    };
    find_list(prs_doc);
    return order;
}

// Extract all text from an <a:txBody> node (paragraph by paragraph).
// Respects <a:br> (line break) and separates paragraphs with newlines.
std::string extract_txbody_text(const pugi::xml_node& txbody)
{
    std::string out;
    for (const auto& para : txbody.children()) {
        std::string_view pn = para.name();
        auto colon_p = pn.rfind(':');
        if ((colon_p == std::string_view::npos ? pn : pn.substr(colon_p + 1))
                != "p") continue;

        std::string para_text;
        for (const auto& child : para.children()) {
            std::string_view cn = child.name();
            auto colon = cn.rfind(':');
            auto local = (colon != std::string_view::npos)
                         ? cn.substr(colon + 1) : cn;
            if (local == "r") {
                // Text run: collect <a:t> children
                for (const auto& t : child.children()) {
                    std::string_view tn = t.name();
                    auto tc = tn.rfind(':');
                    if ((tc != std::string_view::npos
                             ? tn.substr(tc + 1) : tn) == "t")
                        para_text += t.text().get();
                }
            } else if (local == "br") {
                para_text += '\n';
            }
        }
        if (!para_text.empty()) {
            if (!out.empty()) out += '\n';
            out += para_text;
        }
    }
    return out;
}

// Parse one slide XML and return (title_text, body_text).
std::pair<std::string, std::string> parse_slide(const std::string& xml)
{
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size()))
        return {};

    std::string title, body;

    // Walk all <p:sp> shapes in the slide's shape tree.
    // The shape tree is under <p:sld><p:cSld><p:spTree>.
    // Namespace prefix varies (usually "p:"), so we strip it.
    std::function<void(const pugi::xml_node&, int)> walk =
        [&](const pugi::xml_node& n, int depth) {
        if (depth > 8) return;
        for (const auto& child : n.children()) {
            std::string_view cn = child.name();
            auto colon = cn.rfind(':');
            auto local = (colon != std::string_view::npos)
                         ? cn.substr(colon + 1) : cn;

            if (local == "sp") {
                // Find the placeholder type via nvSpPr/nvPr/ph[@type].
                // child("p:nvSpPr") matches ONLY when the prefix is literally
                // "p:" — a file using any other prefix (e.g. "x:nvSpPr") would
                // fail silently and treat every shape as body, losing section
                // headings. Use local-name matching throughout instead.
                pugi::xml_node nvSpPr;
                for (const auto& sc : child.children()) {
                    std::string_view sn = sc.name();
                    auto sc2 = sn.rfind(':');
                    if ((sc2 != std::string_view::npos
                             ? sn.substr(sc2+1) : sn) == "nvSpPr") {
                        nvSpPr = sc; break;
                    }
                }
                // pugixml: traverse to find ph
                pugi::xml_node ph;
                if (nvSpPr) {
                    for (const auto& nv : nvSpPr.children()) {
                        std::string_view nln = nv.name();
                        auto nc = nln.rfind(':');
                        if ((nc != std::string_view::npos
                                 ? nln.substr(nc+1) : nln) == "nvPr") {
                            for (const auto& p : nv.children()) {
                                std::string_view pln = p.name();
                                auto pc = pln.rfind(':');
                                if ((pc != std::string_view::npos
                                         ? pln.substr(pc+1) : pln) == "ph") {
                                    ph = p;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }

                // Find txBody
                pugi::xml_node txbody;
                for (const auto& sc : child.children()) {
                    std::string_view sln = sc.name();
                    auto slc = sln.rfind(':');
                    if ((slc != std::string_view::npos
                             ? sln.substr(slc+1) : sln) == "txBody") {
                        txbody = sc;
                        break;
                    }
                }
                if (!txbody) { walk(child, depth+1); continue; }

                auto text = extract_txbody_text(txbody);
                if (text.empty()) continue;

                // Is this a title shape?
                bool is_title = false;
                if (ph) {
                    std::string_view type_attr = ph.attribute("type").value();
                    is_title = (type_attr == "title" || type_attr == "ctrTitle");
                }
                if (is_title) {
                    // Keep only the first title per slide
                    if (title.empty()) title = text;
                } else {
                    if (!body.empty()) body += '\n';
                    body += text;
                }
            } else {
                walk(child, depth + 1);
            }
        }
    };
    walk(doc, 0);

    return {title, body};
}

} // namespace

// ---------------------------------------------------------------------------
// PptxParser::parse
// ---------------------------------------------------------------------------

Result<ParsedDocument> PptxParser::parse(const std::string& content,
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

    // --- Determine slide order -------------------------------------------
    // Try the relationship-file approach first; fall back to numeric sort.
    auto slide_files = slide_order_from_rels(content);

    if (slide_files.empty()) {
        // Enumerate ppt/slides/slideN.xml numerically
        auto all = pptx_list_entries(content, "ppt/slides/slide");
        // Keep only direct children: "ppt/slides/slide*.xml" not rels
        all.erase(std::remove_if(all.begin(), all.end(), [](const std::string& p) {
            return p.find("_rels") != std::string::npos;
        }), all.end());
        // Sort by the trailing integer
        auto slide_num = [](const std::string& path) -> int {
            auto slash = path.rfind('/');
            auto dot   = path.rfind('.');
            if (slash == std::string::npos || dot == std::string::npos
                || dot < slash + 6) return 0;
            try { return std::stoi(path.substr(slash + 6, dot - slash - 6)); }
            catch (...) { return 0; }
        };
        std::sort(all.begin(), all.end(), [&](const std::string& a,
                                               const std::string& b) {
            return slide_num(a) < slide_num(b);
        });
        slide_files = std::move(all);
    }

    if (slide_files.empty())
        return std::unexpected(
            Error::invalid_input("ingest.pptx.no_slides_found"));

    // Deduplicate: a crafted relationship file can reference the same slide
    // multiple times; process each unique path at most once.
    {
        std::unordered_set<std::string> seen;
        std::vector<std::string> unique;
        for (auto& p : slide_files)
            if (seen.insert(p).second)
                unique.push_back(std::move(p));
        slide_files = std::move(unique);
    }

    // Cap total slide count. Exceeding the cap is treated as a complexity
    // error rather than silent truncation: marking an enterprise deck as
    // "successfully ingested" when only 2000/50000 slides were processed
    // would mislead both users and the polling recovery worker.
    if (slide_files.size() > kMaxPptxSlides) {
        spdlog::error("[pptx-parser] '{}' has {} unique slides > cap ({}); "
                      "rejecting to avoid silent partial ingestion",
                      filename, slide_files.size(), kMaxPptxSlides);
        return std::unexpected(
            Error::invalid_input("ingest.pptx.too_many_slides"));
    }

    // --- Parse slides (with aggregate byte cap) ---------------------------
    ParsedDocument out;
    out.filename  = filename;
    out.mime_type = "application/vnd.openxmlformats-officedocument"
                    ".presentationml.presentation";

    std::size_t total_xml_bytes = 0;
    for (const auto& slide_path : slide_files) {
        auto xml = pptx_extract(content, slide_path.c_str());
        if (xml.empty()) continue;

        total_xml_bytes += xml.size();
        if (total_xml_bytes > kPptxMaxTotalXmlBytes) {
            // Same rationale: partial ingestion is worse than an explicit
            // error; the caller can fail the job and surface it to ops.
            spdlog::error("[pptx-parser] '{}' aggregate decompressed XML "
                          "exceeded cap ({} bytes); rejecting partial result",
                          filename, kPptxMaxTotalXmlBytes);
            return std::unexpected(
                Error::invalid_input("ingest.pptx.aggregate_xml_too_large"));
        }

        auto [title, body] = parse_slide(xml);

        ParsedSection sec;
        sec.heading = std::move(title);
        sec.depth   = 1;
        sec.text    = std::move(body);

        // Title-only slides have no body text. The chunker (chunker.cpp)
        // skips sections with empty text, so the slide would produce zero
        // chunks and its content would be silently lost from the corpus.
        // Copy the heading into the body so the title text is indexed.
        // This matches how document titles are treated across the pipeline:
        // the heading is metadata, but its text must also appear in the
        // retrievable body so a search for the title finds this slide.
        if (sec.text.empty() && !sec.heading.empty())
            sec.text = sec.heading;

        out.sections.push_back(std::move(sec));
    }

    if (out.sections.empty())
        return std::unexpected(
            Error::invalid_input("ingest.pptx.no_text_content"));

    // Build full_text
    for (const auto& s : out.sections) {
        if (!out.full_text.empty() && !s.text.empty())
            out.full_text += '\n';
        out.full_text += s.text;
    }

    return out;
}

} // namespace wikore::ingest
