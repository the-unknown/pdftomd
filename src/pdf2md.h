#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace poppler {
class document;
class page;
}  // namespace poppler

namespace pdf2md {

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct Options {
    // 1-based page numbers; empty = all pages
    std::vector<int> pages;
    // wrap bare URLs into <https://...> autolinks
    bool link_urls = true;
    // merge hyphenated line wraps ("foo-" / "bar" -> "foobar")
    bool dehyphenate = true;
};

// ---------------------------------------------------------------------------
// Extracted data model (plain, testable)
// ---------------------------------------------------------------------------
struct Word {
    std::string text;        // UTF-8
    bool space_after = true; // geometric gap indicates a following space
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // page coords, y grows downwards
    double size = 12.0;      // font size in points
    bool bold = false;
    bool italic = false;
    bool mono = false;
    std::string font_name;
};

struct Line {
    std::vector<Word> words;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // bbox, y grows downwards
    double size = 0;  // char-count weighted dominant font size

    // words joined with single spaces (respecting space_after)
    std::string text() const;
    void add_word(const Word& w);
    void recompute_metrics();
};

// ---------------------------------------------------------------------------
// Pure conversion pipeline (unit-testable, no poppler involved)
// ---------------------------------------------------------------------------

// Cluster words into visual lines (by vertical center), sort by x.
std::vector<Line> group_lines(const std::vector<Word>& words);

// Group lines into blocks; returns index ranges into `lines`.
std::vector<std::vector<std::size_t>> group_blocks(const std::vector<Line>& lines);

// Most common font size (char-count weighted) ~ body text size.
double estimate_body_size(const std::vector<Line>& lines);

// Map distinct (rounded) font sizes larger than body text to heading
// levels 1..6, ranked from largest.
std::map<double, int> heading_size_levels(const std::vector<Line>& lines,
                                          double body_size);
int level_for_size(double size, const std::map<double, int>& model);

// Markdown helpers
std::string escape_md(const std::string& s);
std::string linkify_urls(const std::string& s);
std::string render_inline(const std::vector<Word>& words, bool link_urls = true);

// Detect list markers at the start of plain text.
// Returns true if `s` starts with a bullet; `rest` receives the remainder
// (trimmed) and `consumed` the number of UTF-8 characters eaten.
bool starts_with_bullet(const std::string& s, std::string* rest,
                        int* consumed = nullptr);
// Ordered lists: "1. x", "2) x", "(3) x"
bool starts_with_ordered(const std::string& s, int* number, std::string* rest,
                         int* consumed = nullptr);

// Merge a dehyphenated continuation: line a ends with '-' and line b
// continues it. Mutates `a`, returns true if `b` was consumed.
bool merge_dehyphenation(Line& a, const Line& b);

// Render one block (list of lines) to a markdown string.
std::string render_block(const std::vector<Line>& block,
                         const std::map<double, int>& heading_model,
                         const Options& opts);

// Render all lines of a page.
std::string render_page(const std::vector<Line>& lines,
                        const std::map<double, int>& heading_model,
                        const Options& opts);

// ---------------------------------------------------------------------------
// Poppler-backed extraction
// ---------------------------------------------------------------------------

// Extract words (with font info) from one page. Bounding boxes are already
// in top-down page coordinates (y grows downwards).
std::vector<Word> extract_words(const poppler::page& page);

// Full conversion of a PDF file to markdown. Throws std::runtime_error on
// I/O or parse errors.
std::string convert_file(const std::string& path, const Options& opts = {});

}  // namespace pdf2md
