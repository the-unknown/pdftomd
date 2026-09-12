#include "pdf2md.h"
#include "pdf_factory.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace pdf2md;

namespace {

// ---- test data builders ----------------------------------------------------

enum Style { PLAIN = 0, BOLD = 1, ITALIC = 2, BOLD_ITALIC = 3, MONO = 4 };

Word make_word(const std::string& text, double x, double y, double size = 12,
               Style style = PLAIN) {
    Word w;
    w.text = text;
    w.x0 = x;
    w.x1 = x + size * text.size() * 0.5;
    w.y0 = y;
    w.y1 = y + size;
    w.size = size;
    w.bold = style == BOLD || style == BOLD_ITALIC;
    w.italic = style == ITALIC || style == BOLD_ITALIC;
    w.mono = style == MONO;
    w.space_after = true;
    return w;
}

Line make_line(double x, double y, double size,
               const std::vector<std::pair<std::string, Style>>& words) {
    Line L;
    double cx = x;
    for (const auto& [text, style] : words) {
        Word w = make_word(text, cx, y, size, style);
        L.add_word(w);
        cx += size * text.size() * 0.5 + 4;
    }
    return L;
}

// A line with a single plain word.
Line plain_line(double x, double y, double size, const std::string& text) {
    return make_line(x, y, size, {{text, PLAIN}});
}

// Full pipeline over synthetic lines (same as convert_file, minus poppler).
std::string render_lines(const std::vector<Line>& lines, const Options& opts = {}) {
    double body = estimate_body_size(lines);
    auto model = heading_size_levels(lines, body);
    return render_page(lines, model, opts);
}

std::string temp_path(const std::string& name) {
    const char* dir = "/tmp/opencode/pdftomd_test";
    ::mkdir(dir, 0755);
    return std::string(dir) + "/" + name;
}

}  // namespace

// ===========================================================================
// escape_md
// ===========================================================================

TEST(EscapeMd, EscapesSpecialChars) {
    EXPECT_EQ(escape_md("a*b_c"), "a\\*b\\_c");
}
TEST(EscapeMd, EscapesBacktickAndBrackets) {
    EXPECT_EQ(escape_md("`x` [y]"), "\\`x\\` \\[y\\]");
}
TEST(EscapeMd, EscapesBackslash) {
    EXPECT_EQ(escape_md("a\\b"), "a\\\\b");
}
TEST(EscapeMd, PlainTextUntouched) {
    EXPECT_EQ(escape_md("hallo welt"), "hallo welt");
}

// ===========================================================================
// linkify_urls
// ===========================================================================

TEST(LinkifyUrls, WrapsUrlInAngleBrackets) {
    EXPECT_EQ(linkify_urls("see https://example.com/a now"),
              "see <https://example.com/a> now");
}
TEST(LinkifyUrls, TrimsTrailingPunctuation) {
    EXPECT_EQ(linkify_urls("Visit https://example.com/a?x=1."),
              "Visit <https://example.com/a?x=1>.");
}
TEST(LinkifyUrls, NoUrl) {
    EXPECT_EQ(linkify_urls("just text"), "just text");
}
TEST(LinkifyUrls, TwoUrls) {
    EXPECT_EQ(linkify_urls("a http://x.de b https://y.org/c."),
              "a <http://x.de> b <https://y.org/c>.");
}

// ===========================================================================
// group_lines
// ===========================================================================

TEST(GroupLines, SameY_MergesIntoOneLine) {
    std::vector<Word> w = {
        make_word("hello", 100, 100),
        make_word("world", 150, 100),
        make_word("!", 200, 100),
    };
    auto lines = group_lines(w);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].words.size(), 3u);
    EXPECT_EQ(lines[0].text(), "hello world !");
}

TEST(GroupLines, DifferentY_SplitsLines) {
    std::vector<Word> w = {
        make_word("a", 100, 100),  // center 106
        make_word("b", 100, 130),  // center 136
    };
    auto lines = group_lines(w);
    EXPECT_EQ(lines.size(), 2u);
}

TEST(GroupLines, SlightYJitter_StaysOneLine) {
    // 1pt jitter is within tolerance for size 12 (tol = 4.2)
    std::vector<Word> w = {
        make_word("a", 100, 100),
        make_word("b", 150, 101),
    };
    auto lines = group_lines(w);
    EXPECT_EQ(lines.size(), 1u);
}

TEST(GroupLines, WordsSortedByX) {
    std::vector<Word> w = {
        make_word("right", 300, 100),
        make_word("left", 100, 100),
    };
    auto lines = group_lines(w);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].words.front().text, "left");
}

TEST(GroupLines, EmptyInput) {
    auto lines = group_lines({});
    EXPECT_TRUE(lines.empty());
}

// ===========================================================================
// group_blocks
// ===========================================================================

TEST(GroupBlocks, SmallGap_SameBlock) {
    std::vector<Line> lines = {plain_line(72, 100, 12, "first line"),
                               plain_line(72, 118, 12, "second line")};
    auto blocks = group_blocks(lines);
    EXPECT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].size(), 2u);
}

TEST(GroupBlocks, LargeGap_SplitsBlocks) {
    std::vector<Line> lines = {plain_line(72, 100, 12, "first line"),
                               plain_line(72, 130, 12, "second line")};
    auto blocks = group_blocks(lines);
    EXPECT_EQ(blocks.size(), 2u);
}

TEST(GroupBlocks, SizeJump_SplitsBlocks) {
    // heading (24pt) right after body (12pt) with small gap
    std::vector<Line> lines = {plain_line(72, 100, 12, "body text"),
                               plain_line(72, 114, 24, "big heading")};
    auto blocks = group_blocks(lines);
    EXPECT_EQ(blocks.size(), 2u);
}

// ===========================================================================
// estimate_body_size
// ===========================================================================

TEST(BodySize, DominantSizeWins) {
    std::vector<Line> lines;
    for (int i = 0; i < 5; ++i)
        lines.push_back(plain_line(72, 100 + i * 20, 12, "some body text here"));
    lines.push_back(plain_line(72, 500, 24, "One heading"));
    EXPECT_DOUBLE_EQ(estimate_body_size(lines), 12.0);
}

TEST(BodySize, Empty) {
    EXPECT_DOUBLE_EQ(estimate_body_size({}), 0.0);
}

// ===========================================================================
// heading model
// ===========================================================================

TEST(HeadingModel, RanksSizesDescending) {
    std::vector<Line> lines;
    for (int i = 0; i < 5; ++i)
        lines.push_back(plain_line(72, 200 + i * 20, 12, "body text body text"));
    lines.push_back(plain_line(72, 100, 24, "The big title"));
    lines.push_back(plain_line(72, 300, 16, "A section title"));

    double body = estimate_body_size(lines);
    auto model = heading_size_levels(lines, body);
    EXPECT_EQ(level_for_size(24, model), 1);
    EXPECT_EQ(level_for_size(16, model), 2);
    EXPECT_EQ(level_for_size(12, model), 0);
}

TEST(HeadingModel, NoLargerSize_EmptyModel) {
    std::vector<Line> lines = {plain_line(72, 100, 12, "just body text")};
    auto model = heading_size_levels(lines, 12);
    EXPECT_TRUE(model.empty());
}

TEST(HeadingModel, VerySmallText_NotHeading) {
    // 13pt with body 12: ratio 1.08 < 1.1 -> not a heading candidate
    std::vector<Line> lines;
    for (int i = 0; i < 5; ++i)
        lines.push_back(plain_line(72, 200 + i * 20, 12, "body text body text"));
    lines.push_back(plain_line(72, 100, 13, "slightly bigger text"));
    auto model = heading_size_levels(lines, 12);
    EXPECT_EQ(level_for_size(13, model), 0);
}

// ===========================================================================
// render_inline
// ===========================================================================

TEST(RenderInline, PlainWords) {
    auto w = std::vector<Word>{make_word("hello", 0, 0), make_word("world", 60, 0)};
    EXPECT_EQ(render_inline(w), "hello world");
}

TEST(RenderInline, BoldRun) {
    auto w = std::vector<Word>{
        make_word("hello", 0, 0, 12, BOLD),
        make_word("world", 60, 0, 12, BOLD),
    };
    EXPECT_EQ(render_inline(w), "**hello world**");
}

TEST(RenderInline, ItalicRun) {
    auto w = std::vector<Word>{
        make_word("hello", 0, 0, 12, ITALIC),
        make_word("world", 60, 0, 12, ITALIC),
    };
    EXPECT_EQ(render_inline(w), "*hello world*");
}

TEST(RenderInline, BoldItalicRun) {
    auto w = std::vector<Word>{make_word("hello", 0, 0, 12, BOLD_ITALIC)};
    EXPECT_EQ(render_inline(w), "***hello***");
}

TEST(RenderInline, MonoRun) {
    auto w = std::vector<Word>{
        make_word("int", 0, 0, 12, MONO),
        make_word("x", 40, 0, 12, MONO),
    };
    EXPECT_EQ(render_inline(w), "`int x`");
}

TEST(RenderInline, MixedStyles) {
    auto w = std::vector<Word>{
        make_word("plain", 0, 0, 12, PLAIN),
        make_word("bold", 60, 0, 12, BOLD),
        make_word("tail", 110, 0, 12, PLAIN),
    };
    EXPECT_EQ(render_inline(w), "plain **bold** tail");
}

TEST(RenderInline, NoSpaceAfter_JoinsTight) {
    auto a = make_word("foo", 0, 0);
    a.space_after = false;
    auto w = std::vector<Word>{a, make_word("bar", 30, 0)};
    EXPECT_EQ(render_inline(w), "foobar");
}

TEST(RenderInline, LinksUrls) {
    auto w = std::vector<Word>{make_word("see", 0, 0),
                               make_word("https://example.com", 30, 0)};
    EXPECT_EQ(render_inline(w, true), "see <https://example.com>");
    EXPECT_EQ(render_inline(w, false), "see https://example.com");
}

// ===========================================================================
// list detection
// ===========================================================================

TEST(Bullet, DetectsRoundBullet) {
    std::string rest;
    int mc = 0;
    ASSERT_TRUE(starts_with_bullet("\xE2\x97\x8F first item", &rest, &mc));
    EXPECT_EQ(rest, "first item");
    EXPECT_EQ(mc, 1);
}

TEST(Bullet, DetectsDashBullet) {
    std::string rest;
    ASSERT_TRUE(starts_with_bullet("- dash item", &rest));
    EXPECT_EQ(rest, "dash item");
}

TEST(Bullet, RejectsHyphenInMiddleOfWord) {
    // "well-known" must not be a bullet
    std::string rest;
    EXPECT_FALSE(starts_with_bullet("well-known thing", &rest));
}

TEST(Bullet, RejectsPlainText) {
    std::string rest;
    EXPECT_FALSE(starts_with_bullet("no bullet here", &rest));
}

TEST(Ordered, DetectsNumbered) {
    int num = 0, mc = 0;
    std::string rest;
    ASSERT_TRUE(starts_with_ordered("1. first item", &num, &rest, &mc));
    EXPECT_EQ(num, 1);
    EXPECT_EQ(rest, "first item");
    EXPECT_EQ(mc, 2);
}

TEST(Ordered, DetectsParenthesized) {
    int num = 0;
    std::string rest;
    ASSERT_TRUE(starts_with_ordered("(2) second item", &num, &rest));
    EXPECT_EQ(num, 2);
    EXPECT_EQ(rest, "second item");
}

TEST(Ordered, DetectsClosingParen) {
    int num = 0;
    std::string rest;
    ASSERT_TRUE(starts_with_ordered("3) third item", &num, &rest));
    EXPECT_EQ(num, 3);
    EXPECT_EQ(rest, "third item");
}

TEST(Ordered, RejectsPlainNumberAtLineEnd) {
    int num = 0;
    std::string rest;
    EXPECT_FALSE(starts_with_ordered("42", &num, &rest));
}

TEST(Ordered, RejectsYearLikeText) {
    int num = 0;
    std::string rest;
    // "2020 was great" - "2020" is 4 digits, regex only allows 1-3
    EXPECT_FALSE(starts_with_ordered("2020. was great", &num, &rest));
}

// ===========================================================================
// dehyphenation
// ===========================================================================

TEST(Dehyphenation, HyphenLowercase_MergesTight) {
    Line a = make_line(72, 100, 12, {{"Insulin-", PLAIN}});
    Line b = make_line(72, 118, 12, {{"resistenz", PLAIN}});
    ASSERT_TRUE(merge_dehyphenation(a, b));
    EXPECT_EQ(a.text(), "Insulinresistenz");
}

TEST(Dehyphenation, EnDash_MergesWithSpaceSemantics) {
    Line a = make_line(72, 100, 12, {{"verstehen", PLAIN}, {"\xE2\x80\x93", PLAIN}});
    Line b = make_line(72, 118, 12, {{"Grundlagen", PLAIN}});
    ASSERT_TRUE(merge_dehyphenation(a, b));
    EXPECT_EQ(a.text(), "verstehen \xE2\x80\x93 Grundlagen");
}

TEST(Dehyphenation, DifferentSize_NoMerge) {
    Line a = make_line(72, 100, 12, {{"foo-", PLAIN}});
    Line b = make_line(72, 118, 24, {{"bar", PLAIN}});
    EXPECT_FALSE(merge_dehyphenation(a, b));
}

TEST(Dehyphenation, NoHyphen_NoMerge) {
    Line a = make_line(72, 100, 12, {{"foo", PLAIN}});
    Line b = make_line(72, 118, 12, {{"bar", PLAIN}});
    EXPECT_FALSE(merge_dehyphenation(a, b));
}

// ===========================================================================
// render_block / render_page (end-to-end on synthetic data)
// ===========================================================================

TEST(RenderPage, HeadingAndParagraph) {
    std::vector<Line> lines = {
        plain_line(72, 100, 24, "The Document Title"),
        plain_line(72, 200, 12, "This is the first paragraph of text."),
        plain_line(72, 218, 12, "It continues on the next line."),
    };
    std::string md = render_lines(lines);
    EXPECT_EQ(md, "# The Document Title\n\nThis is the first paragraph of text. "
                  "It continues on the next line.\n\n");
}

TEST(RenderPage, MultipleHeadingLevels) {
    std::vector<Line> lines = {
        plain_line(72, 100, 30, "Level One Heading"),
        plain_line(72, 200, 20, "Level Two Heading"),
        plain_line(72, 300, 12, "Body text goes here for the document."),
    };
    std::string md = render_lines(lines);
    EXPECT_EQ(md, "# Level One Heading\n\n## Level Two Heading\n\n"
                  "Body text goes here for the document.\n\n");
}

TEST(RenderPage, BoldAndItalicInParagraph) {
    std::vector<Line> lines = {
        make_line(72, 100, 12, {{"This is", PLAIN}, {"bold", BOLD},
                                 {"and", PLAIN}, {"italic", ITALIC},
                                 {"text.", PLAIN}}),
    };
    std::string md = render_lines(lines);
    EXPECT_EQ(md, "This is **bold** and *italic* text.\n\n");
}

TEST(RenderPage, BulletList) {
    std::vector<Line> lines = {
        make_line(72, 100, 12, {{"\xE2\x97\x8F", PLAIN}, {"first", PLAIN}}),
        make_line(72, 118, 12, {{"\xE2\x97\x8F", PLAIN}, {"second", PLAIN}}),
        make_line(72, 136, 12, {{"\xE2\x97\x8F", PLAIN}, {"third", PLAIN}}),
    };
    std::string md = render_lines(lines);
    EXPECT_EQ(md, "- first\n- second\n- third\n\n");
}

TEST(RenderPage, NumberedList) {
    std::vector<Line> lines = {
        make_line(72, 100, 12, {{"1.", PLAIN}, {"first", PLAIN}}),
        make_line(72, 118, 12, {{"2.", PLAIN}, {"second", PLAIN}}),
    };
    std::string md = render_lines(lines);
    EXPECT_EQ(md, "1. first\n2. second\n\n");
}

TEST(RenderPage, ListAfterParagraph) {
    std::vector<Line> lines = {
        plain_line(72, 100, 12, "Intro text before the list."),
        plain_line(72, 130, 12, ""),  // spacer line (large gap -> new block)
        make_line(72, 150, 12, {{"-", PLAIN}, {"item", PLAIN}}),
    };
    (void)lines;
    // simpler: paragraph + list in one page with gap
    std::vector<Line> l2 = {
        plain_line(72, 100, 12, "Intro text before the list."),
        make_line(72, 160, 12, {{"-", PLAIN}, {"item", PLAIN}}),
    };
    std::string md = render_lines(l2);
    EXPECT_EQ(md, "Intro text before the list.\n\n- item\n\n");
}

TEST(RenderPage, NestedListByIndent) {
    std::vector<Line> lines = {
        make_line(72, 100, 12, {{"-", PLAIN}, {"outer", PLAIN}}),
        make_line(92, 118, 12, {{"-", PLAIN}, {"inner", PLAIN}}),
        make_line(72, 136, 12, {{"-", PLAIN}, {"outer2", PLAIN}}),
    };
    std::string md = render_lines(lines);
    EXPECT_EQ(md, "- outer\n  - inner\n- outer2\n\n");
}

TEST(RenderPage, DehyphenatedWrap) {
    std::vector<Line> lines = {
        make_line(72, 100, 12, {{"This long", PLAIN}, {"Insulin-", PLAIN}}),
        make_line(72, 118, 12, {{"resistenz.", PLAIN}}),
    };
    std::string md = render_lines(lines);
    EXPECT_EQ(md, "This long Insulinresistenz.\n\n");
}

TEST(RenderPage, UrlInText) {
    std::vector<Line> lines = {
        make_line(72, 100, 12, {{"More info at", PLAIN},
                                 {"https://pubmed.ncbi.nlm.nih.gov/123/", PLAIN}}),
    };
    std::string md = render_lines(lines);
    EXPECT_EQ(md, "More info at <https://pubmed.ncbi.nlm.nih.gov/123/>\n\n");
}

TEST(RenderPage, NoUrlsOption) {
    Options opts;
    opts.link_urls = false;
    std::vector<Line> lines = {
        make_line(72, 100, 12, {{"https://example.com", PLAIN}}),
    };
    std::string md = render_lines(lines, opts);
    EXPECT_EQ(md, "https://example.com\n\n");
}

TEST(RenderPage, EmptyLines) {
    EXPECT_EQ(render_lines({}), "");
}

// ===========================================================================
// parse_page_spec (via CLI behavior is covered separately; test helper logic)
// ===========================================================================

// ===========================================================================
// Integration tests: real poppler over generated PDFs
// ===========================================================================

class IntegrationTest : public ::testing::Test {
protected:
    void write_pdf(const pdfgen::Doc& doc, const std::string& name) {
        path_ = temp_path(name);
        doc.save(path_);
    }
    std::string convert(const pdfgen::Doc& doc, const std::string& name,
                        const Options& opts = {}) {
        write_pdf(doc, name);
        return convert_file(path_, opts);
    }
    std::string path_;
};

TEST_F(IntegrationTest, SimpleText) {
    pdfgen::Doc doc;
    doc.add_page();
    doc.text(72, 700, 12, 1, "Hello world");
    std::string md = convert(doc, "simple.pdf");
    EXPECT_NE(md.find("Hello world"), std::string::npos);
}

TEST_F(IntegrationTest, TwoLinesTwoParagraphs) {
    pdfgen::Doc doc;
    doc.add_page();
    doc.text(72, 700, 12, 1, "First paragraph line.");
    doc.text(72, 650, 12, 1, "Second paragraph far away.");
    std::string md = convert(doc, "two_paras.pdf");
    EXPECT_NE(md.find("First paragraph line."), std::string::npos);
    EXPECT_NE(md.find("Second paragraph far away."), std::string::npos);
    // two separate paragraphs -> two blocks separated by blank line
    EXPECT_NE(md.find("First paragraph line.\n\nSecond paragraph far away."),
              std::string::npos);
}

TEST_F(IntegrationTest, WrappedLine_StaysOneParagraph) {
    pdfgen::Doc doc;
    doc.add_page();
    doc.text(72, 700, 12, 1, "This is a long line that wraps onto");
    doc.text(72, 685, 12, 1, "the next line in the same paragraph.");
    std::string md = convert(doc, "wrapped.pdf");
    EXPECT_NE(md.find("This is a long line that wraps onto the next line in the "
                      "same paragraph."),
              std::string::npos);
}

TEST_F(IntegrationTest, HeadingFromFontSize) {
    pdfgen::Doc doc;
    doc.add_page();
    doc.text(72, 720, 24, 1, "The Main Title Here");
    doc.text(72, 650, 12, 1, "Body text of the document goes here.");
    std::string md = convert(doc, "heading.pdf");
    EXPECT_NE(md.find("# The Main Title Here"), std::string::npos);
    EXPECT_NE(md.find("Body text of the document goes here."), std::string::npos);
}

TEST_F(IntegrationTest, BoldFont) {
    pdfgen::Doc doc;
    doc.add_page();
    doc.text(72, 700, 12, 2, "This text is bold");
    std::string md = convert(doc, "bold.pdf");
    EXPECT_NE(md.find("**This text is bold**"), std::string::npos);
}

TEST_F(IntegrationTest, ItalicFont) {
    pdfgen::Doc doc;
    doc.add_page();
    doc.text(72, 700, 12, 3, "This text is italic");
    std::string md = convert(doc, "italic.pdf");
    EXPECT_NE(md.find("*This text is italic*"), std::string::npos);
}

TEST_F(IntegrationTest, MonospaceFont) {
    pdfgen::Doc doc;
    doc.add_page();
    doc.text(72, 700, 12, 4, "code sample");
    std::string md = convert(doc, "mono.pdf");
    EXPECT_NE(md.find("`code sample`"), std::string::npos);
}

TEST_F(IntegrationTest, Umlauts) {
    pdfgen::Doc doc;
    doc.add_page();
    doc.text(72, 700, 12, 1, "Grüße und Grüße öäü ß");
    std::string md = convert(doc, "umlauts.pdf");
    EXPECT_NE(md.find("Grüße"), std::string::npos);
    EXPECT_NE(md.find("öäü ß"), std::string::npos);
}

TEST_F(IntegrationTest, UrlInPdf) {
    pdfgen::Doc doc;
    doc.add_page();
    doc.text(72, 700, 12, 1, "Source: https://pubmed.ncbi.nlm.nih.gov/40247011/");
    std::string md = convert(doc, "url.pdf");
    EXPECT_NE(md.find("<https://pubmed.ncbi.nlm.nih.gov/40247011/>"),
              std::string::npos);
}

TEST_F(IntegrationTest, MultiPage) {
    pdfgen::Doc doc;
    doc.add_page();
    doc.text(72, 700, 12, 1, "Content of page one");
    doc.add_page();
    doc.text(72, 700, 12, 1, "Content of page two");
    std::string md = convert(doc, "multipage.pdf");
    EXPECT_NE(md.find("Content of page one"), std::string::npos);
    EXPECT_NE(md.find("Content of page two"), std::string::npos);
    size_t p1 = md.find("Content of page one");
    size_t p2 = md.find("Content of page two");
    ASSERT_NE(p1, std::string::npos);
    ASSERT_NE(p2, std::string::npos);
    EXPECT_LT(p1, p2);
}

TEST_F(IntegrationTest, PageSelection) {
    pdfgen::Doc doc;
    doc.add_page();
    doc.text(72, 700, 12, 1, "ONLY FIRST PAGE TEXT");
    doc.add_page();
    doc.text(72, 700, 12, 1, "ONLY SECOND PAGE TEXT");

    Options opts;
    opts.pages = {2};
    write_pdf(doc, "pagesel.pdf");
    std::string md = convert_file(path_, opts);
    EXPECT_EQ(md.find("ONLY FIRST PAGE TEXT"), std::string::npos);
    EXPECT_NE(md.find("ONLY SECOND PAGE TEXT"), std::string::npos);
}

TEST_F(IntegrationTest, BulletListFromPdf) {
    pdfgen::Doc doc;
    doc.add_page();
    doc.text(72, 700, 12, 1, "\xE2\x80\xA2 alpha");
    doc.text(72, 685, 12, 1, "\xE2\x80\xA2 beta");
    std::string md = convert(doc, "bullet.pdf");
    EXPECT_NE(md.find("- alpha"), std::string::npos);
    EXPECT_NE(md.find("- beta"), std::string::npos);
}

TEST_F(IntegrationTest, NumberedListFromPdf) {
    pdfgen::Doc doc;
    doc.add_page();
    doc.text(72, 700, 12, 1, "1. first");
    doc.text(72, 685, 12, 1, "2. second");
    std::string md = convert(doc, "ordered.pdf");
    EXPECT_NE(md.find("1. first"), std::string::npos);
    EXPECT_NE(md.find("2. second"), std::string::npos);
}

TEST_F(IntegrationTest, MixedBoldPlainInOneLine) {
    pdfgen::Doc doc;
    doc.add_page();
    // two text runs on the same line: plain then bold
    doc.text(72, 700, 12, 1, "plain ");
    doc.text(72 + 6 * 12 * 0.5, 700, 12, 2, "bold part");
    std::string md = convert(doc, "mixedline.pdf");
    EXPECT_NE(md.find("**bold part**"), std::string::npos);
    EXPECT_NE(md.find("plain"), std::string::npos);
}

#ifdef REAL_PDF
TEST_F(IntegrationTest, RealNourivaPdf) {
    Options opts;
    std::string md = convert_file(REAL_PDF, opts);
    ASSERT_FALSE(md.empty());
    EXPECT_NE(md.find("WOCHE 1"), std::string::npos);
    EXPECT_NE(md.find("Insulinresistenz"), std::string::npos);
    EXPECT_NE(md.find("pubmed.ncbi.nlm.nih.gov"), std::string::npos);
    // URLs should be wrapped as autolinks
    EXPECT_NE(md.find("<https://pubmed.ncbi.nlm.nih.gov/40247011/>"),
              std::string::npos);
    // some heading should have been detected
    EXPECT_NE(md.find("#"), std::string::npos);
}
#endif

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
