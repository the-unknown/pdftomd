#include "pdf2md.h"

#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-page.h>
#include <poppler/cpp/poppler-rectangle.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace pdf2md {

namespace {

// ---- UTF-8 helpers --------------------------------------------------------

// Decode one codepoint at position `pos` in s; returns the number of bytes
// consumed (0 on empty/out-of-range).
int utf8_decode(const std::string& s, size_t pos, uint32_t* out) {
    if (pos >= s.size()) return 0;
    unsigned char c = static_cast<unsigned char>(s[pos]);
    auto bytes = [&](int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (pos + i >= s.size()) return false;
            v = (v << 6) | (static_cast<unsigned char>(s[pos + i]) & 0x3F);
        }
        *out = v;
        return true;
    };
    if (c < 0x80) { *out = c; return 1; }
    if ((c & 0xE0) == 0xC0) { uint32_t v; if (!bytes(2)) return 0; *out = v; return 2; }
    if ((c & 0xF0) == 0xE0) { uint32_t v; if (!bytes(3)) return 0; *out = v; return 3; }
    if ((c & 0xF8) == 0xF0) { uint32_t v; if (!bytes(4)) return 0; *out = v; return 4; }
    *out = c;
    return 1;
}

std::string utf8_encode(uint32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return s;
}

int utf8_length(const std::string& s) {
    int n = 0;
    uint32_t cp;
    size_t pos = 0;
    while (pos < s.size()) {
        int len = utf8_decode(s, pos, &cp);
        if (len <= 0) break;
        ++n;
        pos += len;
    }
    return n;
}

std::string to_lower(const std::string& s) {
    std::string r;
    for (unsigned char c : s)
        r.push_back(static_cast<char>(std::tolower(static_cast<int>(c))));
    return r;
}

double round_half(double v) { return std::round(v * 2.0) / 2.0; }

bool same_style(const Word& a, const Word& b) {
    return a.bold == b.bold && a.italic == b.italic && a.mono == b.mono;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Remove one trailing UTF-8 codepoint from s.
std::string chop_last_char(const std::string& s) {
    size_t pos = 0;
    uint32_t cp;
    size_t last_start = 0, last_len = 0;
    while (pos < s.size()) {
        int len = utf8_decode(s, pos, &cp);
        if (len <= 0) break;
        last_start = pos;
        last_len = len;
        pos += len;
    }
    return s.substr(0, last_start);
}

// Remove exactly `n` leading UTF-8 codepoints from s.
std::string drop_leading_chars(const std::string& s, int n) {
    size_t pos = 0;
    uint32_t cp;
    while (n > 0 && pos < s.size()) {
        int len = utf8_decode(s, pos, &cp);
        if (len <= 0) break;
        pos += len;
        --n;
    }
    return s.substr(pos);
}

}  // namespace

// ---------------------------------------------------------------------------
// Line
// ---------------------------------------------------------------------------

std::string Line::text() const {
    std::string out;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if (i > 0 && words[i - 1].space_after) out += " ";
        out += words[i].text;
    }
    return out;
}

void Line::add_word(const Word& w) {
    words.push_back(w);
    recompute_metrics();
}

void Line::recompute_metrics() {
    x0 = y0 = 1e18;
    x1 = y1 = -1e18;
    double chars = 0, weighted = 0;
    for (const auto& w : words) {
        x0 = std::min(x0, w.x0);
        y0 = std::min(y0, w.y0);
        x1 = std::max(x1, w.x1);
        y1 = std::max(y1, w.y1);
        double c = utf8_length(w.text);
        chars += c;
        weighted += c * w.size;
    }
    size = chars > 0 ? weighted / chars : 0;
}

// ---------------------------------------------------------------------------
// group_lines
// ---------------------------------------------------------------------------

std::vector<Line> group_lines(const std::vector<Word>& in) {
    std::vector<Word> words = in;
    auto yc = [](const Word& w) { return (w.y0 + w.y1) / 2.0; };
    std::stable_sort(words.begin(), words.end(),
                     [&](const Word& a, const Word& b) {
                         if (std::fabs(yc(a) - yc(b)) > 1e-6) return yc(a) < yc(b);
                         return a.x0 < b.x0;
                     });

    std::vector<Line> lines;
    for (const auto& w : words) {
        double wy = yc(w);
        bool merged = false;
        if (!lines.empty()) {
            Line& L = lines.back();
            double lcy = (L.y0 + L.y1) / 2.0;
            double tol = std::max(1.5, 0.35 * std::max(L.size, w.size));
            if (std::fabs(wy - lcy) <= tol) {
                L.add_word(w);
                merged = true;
            }
        }
        if (!merged) {
            Line L;
            L.add_word(w);
            lines.push_back(std::move(L));
        }
    }
    for (auto& L : lines)
        std::stable_sort(L.words.begin(), L.words.end(),
                         [](const Word& a, const Word& b) { return a.x0 < b.x0; });
    return lines;
}

// ---------------------------------------------------------------------------
// group_blocks
// ---------------------------------------------------------------------------

std::vector<std::vector<std::size_t>> group_blocks(const std::vector<Line>& lines) {
    std::vector<std::vector<std::size_t>> blocks;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        bool new_block = blocks.empty();
        if (!new_block) {
            const Line& prev = lines[i - 1];
            const Line& cur = lines[i];
            double gap = cur.y0 - prev.y1;
            double threshold = 0.55 * std::max(prev.size, cur.size);
            bool size_jump = std::fabs(cur.size - prev.size) > 1.5;
            if (gap > threshold || size_jump) new_block = true;
        }
        if (new_block) blocks.emplace_back();
        blocks.back().push_back(i);
    }
    return blocks;
}

// ---------------------------------------------------------------------------
// body size + heading model
// ---------------------------------------------------------------------------

double estimate_body_size(const std::vector<Line>& lines) {
    std::map<double, double> chars;
    double total = 0;
    for (const auto& L : lines)
        for (const auto& w : L.words) {
            double c = utf8_length(w.text);
            chars[round_half(w.size)] += c;
            total += c;
        }
    if (total <= 0) return 0;
    double best = 0, best_c = -1;
    for (const auto& [s, c] : chars)
        if (c > best_c || (c == best_c && s < best)) {
            best = s;
            best_c = c;
        }
    return best;
}

std::map<double, int> heading_size_levels(const std::vector<Line>& lines,
                                          double body_size) {
    std::map<double, double> chars;
    for (const auto& L : lines)
        for (const auto& w : L.words)
            chars[round_half(w.size)] += utf8_length(w.text);
    std::vector<std::pair<double, double>> candidates;
    for (const auto& [s, c] : chars)
        if (body_size > 0 && s >= body_size * 1.1 && c >= 5)
            candidates.emplace_back(s, c);
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second > b.second;
              });
    std::map<double, int> model;
    for (std::size_t i = 0; i < candidates.size(); ++i)
        model[candidates[i].first] = static_cast<int>(std::min<std::size_t>(i + 1, 6));
    return model;
}

int level_for_size(double size, const std::map<double, int>& model) {
    double r = round_half(size);
    auto it = model.lower_bound(r);
    if (it == model.end()) return 0;
    if (std::fabs(it->first - r) < 0.26) return it->second;
    return 0;
}

// ---------------------------------------------------------------------------
// markdown helpers
// ---------------------------------------------------------------------------

std::string escape_md(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '`' || c == '*' || c == '_' || c == '[' || c == ']')
            out += '\\';
        out += c;
    }
    return out;
}

std::string linkify_urls(const std::string& s) {
    static const std::regex url_re(R"(https?://[A-Za-z0-9./?_%#&=+:@~-]+)");
    std::string out;
    std::size_t last = 0;
    for (;;) {
        std::string rest = s.substr(last);
        std::smatch m;
        if (!std::regex_search(rest, m, url_re)) break;
        std::string url = m[0].str();
        while (!url.empty() && std::string(".!?;,:'\"")
                   .find(url.back()) != std::string::npos)
            url.pop_back();
        out += rest.substr(0, m.position(0));
        out += "<" + url + ">";
        out += m[0].str().substr(url.size());  // re-emit trimmed punctuation
        last += m.position(0) + m[0].length();
    }
    out += s.substr(last);
    return out;
}

std::string render_inline(const std::vector<Word>& words, bool link_urls) {
    if (words.empty()) return "";
    std::string out;
    std::size_t i = 0;
    while (i < words.size()) {
        if (words[i].text.empty()) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j + 1 < words.size() && !words[j + 1].text.empty() &&
               same_style(words[j], words[j + 1]))
            ++j;
        std::string run;
        for (std::size_t k = i; k <= j; ++k) {
            if (k > i && words[k - 1].space_after) run += " ";
            run += escape_md(words[k].text);
        }
        if (!out.empty() && words[i - 1].space_after) out += " ";
        if (words[i].mono) {
            out += "`" + run + "`";
        } else if (words[i].bold && words[i].italic) {
            out += "***" + run + "***";
        } else if (words[i].bold) {
            out += "**" + run + "**";
        } else if (words[i].italic) {
            out += "*" + run + "*";
        } else {
            out += run;
        }
        i = j + 1;
    }
    return link_urls ? linkify_urls(out) : out;
}

// ---------------------------------------------------------------------------
// list detection
// ---------------------------------------------------------------------------

bool starts_with_bullet(const std::string& s, std::string* rest,
                        int* marker_chars) {
    static const char* bullets[] = {"\xE2\x80\xA2",  // • (U+2022)
                                    "\xE2\x97\x8F",  // ● (U+25CF)
                                    "\xE2\x97\xA6",  // ◦ (U+25E6)
                                    "\xE2\x97\x8A",  // ▪ (U+25AA)
                                    "\xE2\x80\x93",  // – (U+2013)
                                    "\xC2\xB7",      // · (U+00B7)
                                    "-"};
    size_t pos = 0;
    while (pos < s.size() && s[pos] == ' ') ++pos;
    for (const char* b : bullets) {
        size_t blen = std::char_traits<char>::length(b);
        if (s.compare(pos, blen, b) == 0) {
            size_t after = pos + blen;
            if (after >= s.size() || s[after] == ' ' || s[after] == '\t') {
                if (rest) *rest = trim(s.substr(after));
                if (marker_chars) *marker_chars = 1;  // bullet is one codepoint
                return true;
            }
        }
    }
    return false;
}

bool starts_with_ordered(const std::string& s, int* number, std::string* rest,
                         int* marker_chars) {
    static const std::regex re(R"(^\s*(\d{1,3})[.)](\s+)|^\s*\((\d{1,3})\)(\s+))");
    std::cmatch m;
    if (!std::regex_search(s.c_str(), m, re)) return false;
    bool paren = m[1].length() == 0;
    if (number) *number = std::stoi(paren ? m[3].str() : m[1].str());
    std::size_t after = m.position(0) + m.length(0);
    if (rest) *rest = trim(s.substr(after));
    if (marker_chars) {
        *marker_chars = paren ? static_cast<int>(m[3].length()) + 2  // "(N)"
                              : static_cast<int>(m[1].length()) + 1;  // "N."
    }
    return true;
}

// ---------------------------------------------------------------------------
// dehyphenation
// ---------------------------------------------------------------------------

bool merge_dehyphenation(Line& a, const Line& b) {
    if (a.words.empty() || b.words.empty()) return false;
    if (std::fabs(a.size - b.size) > 1.5) return false;
    std::string at = a.text();
    if (ends_with(at, "-")) {
        uint32_t first_cp = 0;
        utf8_decode(b.text(), 0, &first_cp);
        bool lower = first_cp >= 'a' && first_cp <= 'z';
        if (lower) {
            // true hyphenation: merge into a single word without a space
            a.words.back().text =
                chop_last_char(a.words.back().text) + b.words.front().text;
            a.words.insert(a.words.end(), b.words.begin() + 1, b.words.end());
            a.recompute_metrics();
            return true;
        }
    } else if (ends_with(at, "\xE2\x80\x93")) {  // en dash "–" (title wrap)
        // keep the dash, continue with a space
        a.words.back().text =
            a.words.back().text + " " + b.words.front().text;
        a.words.insert(a.words.end(), b.words.begin() + 1, b.words.end());
        a.recompute_metrics();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// block / page rendering
// ---------------------------------------------------------------------------

namespace {
struct ListItem {
    double indent = 0;
    std::string text;
};
}  // namespace

std::string render_block(const std::vector<Line>& in_block,
                         const std::map<double, int>& heading_model,
                         const Options& opts) {
    std::vector<Line> block = in_block;
    if (opts.dehyphenate) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t i = 0; i + 1 < block.size(); ++i) {
                if (merge_dehyphenation(block[i], block[i + 1])) {
                    block.erase(block.begin() + static_cast<std::ptrdiff_t>(i + 1));
                    changed = true;
                    break;
                }
            }
        }
    }

    std::vector<std::string> parts;
    std::vector<std::string> para;
    std::vector<ListItem> list_items;
    double list_min_indent = 0;
    bool have_list = false;

    auto flush_para = [&]() {
        if (!para.empty()) {
            std::string p;
            for (std::size_t i = 0; i < para.size(); ++i) {
                if (i) p += " ";
                p += para[i];
            }
            parts.push_back(p);
            para.clear();
        }
    };
    auto flush_list = [&]() {
        if (!have_list) return;
        double step = 0;
        for (const auto& it : list_items) {
            double d = it.indent - list_min_indent;
            if (d > 4 && (step == 0 || d < step)) step = d;
        }
        if (step <= 0) step = 8;
        std::string l;
        for (std::size_t i = 0; i < list_items.size(); ++i) {
            if (i) l += "\n";
            int level = static_cast<int>(
                std::round((list_items[i].indent - list_min_indent) / step));
            level = std::max(0, std::min(level, 8));
            for (int k = 0; k < level; ++k) l += "  ";
            l += list_items[i].text;
        }
        parts.push_back(l);
        list_items.clear();
        have_list = false;
    };

    for (const auto& L : block) {
        int h = level_for_size(L.size, heading_model);
        std::string plain = L.text();

        if (h > 0) {
            flush_para();
            flush_list();
            std::string t;
            for (int k = 0; k < h; ++k) t += '#';
            parts.push_back(t + " " + render_inline(L.words, opts.link_urls));
            continue;
        }

        std::string rest;
        int marker_chars = 0;
        bool bullet = starts_with_bullet(plain, &rest, &marker_chars);
        bool ordered = !bullet && starts_with_ordered(plain, nullptr, &rest, &marker_chars);
        if (bullet || ordered) {
            flush_para();
            std::vector<Word> rest_words = L.words;
            if (!rest_words.empty() && marker_chars > 0) {
                std::string& t = rest_words.front().text;
                size_t p = 0;
                while (p < t.size() && t[p] == ' ') ++p;
                t.erase(0, p);
                t = drop_leading_chars(t, marker_chars);
                if (t.empty()) rest_words.erase(rest_words.begin());
            }
            std::string marker = "- ";
            if (ordered) {
                int num = 0;
                starts_with_ordered(plain, &num, nullptr, nullptr);
                marker = std::to_string(num) + ". ";
            }
            if (!have_list) {
                have_list = true;
                list_min_indent = L.x0;
            }
            list_items.push_back({L.x0, marker + render_inline(rest_words, opts.link_urls)});
            continue;
        }

        flush_list();
        para.push_back(render_inline(L.words, opts.link_urls));
    }
    flush_para();
    flush_list();

    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "\n\n";
        out += parts[i];
    }
    return out;
}

std::string render_page(const std::vector<Line>& lines,
                        const std::map<double, int>& heading_model,
                        const Options& opts) {
    std::string out;
    for (const auto& block : group_blocks(lines)) {
        std::vector<Line> bl;
        bl.reserve(block.size());
        for (auto idx : block) bl.push_back(lines[idx]);
        std::string b = render_block(bl, heading_model, opts);
        if (!b.empty()) {
            out += b;
            out += "\n\n";
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// poppler extraction
// ---------------------------------------------------------------------------

std::vector<Word> extract_words(const poppler::page& page) {
    std::vector<Word> out;
    auto boxes = page.text_list(poppler::page::text_list_include_font);
    for (const auto& box : boxes) {
        poppler::ustring text = box.text();
        if (text.length() == 0) continue;
        Word w;
        poppler::byte_array utf8 = text.to_utf8();
        w.text = std::string(utf8.data(), utf8.size());
        poppler::rectf b = box.bbox();
        // text_list bboxes are already in top-down page coordinates
        w.x0 = std::min(b.left(), b.right());
        w.x1 = std::max(b.left(), b.right());
        w.y0 = std::min(b.top(), b.bottom());
        w.y1 = std::max(b.top(), b.bottom());
        w.size = box.get_font_size();
        w.font_name = box.get_font_name(0);
        w.space_after = box.has_space_after();
        std::string fname = to_lower(w.font_name);
        w.bold = fname.find("bold") != std::string::npos ||
                 fname.find("black") != std::string::npos ||
                 fname.find("heavy") != std::string::npos;
        w.italic = fname.find("italic") != std::string::npos ||
                   fname.find("oblique") != std::string::npos;
        w.mono = fname.find("courier") != std::string::npos ||
                 fname.find("mono") != std::string::npos ||
                 fname.find("consol") != std::string::npos;
        out.push_back(std::move(w));
    }
    return out;
}

std::string convert_file(const std::string& path, const Options& opts) {
    poppler::document* doc = poppler::document::load_from_file(path);
    if (!doc) throw std::runtime_error("cannot open PDF: " + path);
    std::unique_ptr<poppler::document> doc_guard(doc);

    int total = doc->pages();
    std::vector<int> selected;
    if (opts.pages.empty()) {
        for (int i = 1; i <= total; ++i) selected.push_back(i);
    } else {
        for (int p : opts.pages)
            if (p >= 1 && p <= total) selected.push_back(p);
        std::sort(selected.begin(), selected.end());
    }

    std::vector<std::vector<Line>> page_lines(selected.size());
    std::vector<Line> all_lines;
    for (std::size_t si = 0; si < selected.size(); ++si) {
        std::unique_ptr<poppler::page> pg(doc->create_page(selected[si] - 1));
        if (!pg) continue;
        auto words = extract_words(*pg);
        auto lines = group_lines(words);
        page_lines[si] = std::move(lines);
        for (auto& L : page_lines[si]) all_lines.push_back(L);
    }

    double body = estimate_body_size(all_lines);
    auto model = heading_size_levels(all_lines, body);

    std::string out;
    for (const auto& pl : page_lines) out += render_page(pl, model, opts);
    return out;
}

}  // namespace pdf2md
