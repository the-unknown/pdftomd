#pragma once

// Minimal PDF generator for integration tests.
// Produces small, valid PDF files using the base-14 fonts:
//   1 = Helvetica, 2 = Helvetica-Bold, 3 = Helvetica-Oblique, 4 = Courier
//
// Strings are encoded in WinAnsi (Latin-1); UTF-8 input is transcoded.

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace pdfgen {

// UTF-8 -> WinAnsi/Latin-1 bytes (throws for codepoints > 0xFF except a
// few WinAnsi-specific ones: en dash, em dash, curly quotes, ellipsis).
inline std::string utf8_to_winansi(const std::string& s) {
    std::string out;
    size_t pos = 0;
    while (pos < s.size()) {
        unsigned char c = s[pos];
        uint32_t cp;
        int len;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0 && pos + 1 < s.size()) {
            cp = ((c & 0x1F) << 6) | (s[pos + 1] & 0x3F);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && pos + 2 < s.size()) {
            cp = ((c & 0x0F) << 12) | ((s[pos + 1] & 0x3F) << 6) | (s[pos + 2] & 0x3F);
            len = 3;
        } else {
            throw std::runtime_error("pdfgen: unsupported utf8 sequence");
        }
        switch (cp) {
            case 0x2013: out.push_back(0x96); break;  // – en dash
            case 0x2014: out.push_back(0x97); break;  // — em dash
            case 0x2022: out.push_back(0x95); break;  // • bullet
            case 0x2018: out.push_back(0x91); break;  // '
            case 0x2019: out.push_back(0x92); break;  // '
            case 0x201C: out.push_back(0x93); break;  // "
            case 0x201D: out.push_back(0x94); break;  // "
            case 0x2026: out.push_back(0x85); break;  // …
            default:
                if (cp > 0xFF) throw std::runtime_error("pdfgen: codepoint out of WinAnsi range");
                out.push_back(static_cast<char>(cp));
        }
        pos += len;
    }
    return out;
}

inline std::string pdf_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '(': out += "\\("; break;
            case ')': out += "\\)"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

class Doc {
public:
    Doc() : next_obj_id_(1) {}

    void add_page(double width = 612, double height = 792) {
        pages_.push_back({width, height, {}});
    }

    // Add a text chunk at position (x, y) [PDF coords: origin bottom-left]
    // with the given font (1..4) and size.
    void text(double x, double y, double size, int font, const std::string& utf8) {
        if (pages_.empty())
            throw std::runtime_error("pdfgen: no page");
        std::ostringstream os;
        os << "BT /F" << font << " " << size << " Tf " << x << " " << y
           << " Td (" << pdf_escape(utf8_to_winansi(utf8)) << ") Tj ET\n";
        pages_.back().stream += os.str();
    }

    // Convenience: a full line with per-word styling.
    struct WordSpec {
        double size;
        int font;
        std::string text;
    };
    // Lays words out left to right starting at x, using per-word widths
    // estimated from char count (good enough for base-14 metrics tests).
    void line(double x, double y, const std::vector<WordSpec>& words) {
        double cx = x;
        for (const auto& w : words) {
            text(cx, y, w.size, w.font, w.text);
            double width = 0.5 * w.size * w.text.size();
            cx += width + 0.2 * w.size;  // + space
        }
    }

    std::string bytes() const {
        if (pages_.empty())
            throw std::runtime_error("pdfgen: empty document");

        // Object layout:
        //  1            catalog
        //  2            pages
        //  3..6         fonts (Helvetica, Helvetica-Bold, Helvetica-Oblique, Courier)
        //  then per page: page obj + content obj
        std::string body;
        std::vector<std::string> objects;  // 1-based index -> object body
        objects.resize(7);
        int page_obj_start = 7;
        for (size_t i = 0; i < pages_.size(); ++i)
            objects.resize(page_obj_start + 2 * static_cast<int>(i) + 2, "");

        std::ostringstream kids;
        for (size_t i = 0; i < pages_.size(); ++i) {
            if (i) kids << " ";
            kids << (page_obj_start + 2 * static_cast<int>(i)) << " 0 R";
        }
        objects[1] = "<< /Type /Catalog /Pages 2 0 R >>";
        objects[2] = "<< /Type /Pages /Kids [" + kids.str() +
                     "] /Count " + std::to_string(pages_.size()) + " >>";
        const char* font_names[4] = {"Helvetica", "Helvetica-Bold",
                                     "Helvetica-Oblique", "Courier"};
        for (int i = 0; i < 4; ++i)
            objects[3 + i] = std::string("<< /Type /Font /Subtype /Type1 /BaseFont /") +
                             font_names[i] +
                             " /Encoding /WinAnsiEncoding >>";

        for (size_t i = 0; i < pages_.size(); ++i) {
            int page_id = page_obj_start + 2 * static_cast<int>(i);
            int content_id = page_id + 1;
            objects[page_id] =
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " +
                std::to_string(pages_[i].width) + " " +
                std::to_string(pages_[i].height) +
                "] /Resources << /Font << /F1 3 0 R /F2 4 0 R /F3 5 0 R /F4 6 0 R >> >> "
                "/Contents " +
                std::to_string(content_id) + " 0 R >>";
            objects[content_id] = "<< /Length " +
                                  std::to_string(pages_[i].stream.size()) +
                                  " >>\nstream\n" + pages_[i].stream + "endstream";
        }

        // Assemble with xref
        std::string pdf = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
        std::vector<size_t> offsets(objects.size(), 0);
        for (size_t i = 1; i < objects.size(); ++i) {
            offsets[i] = pdf.size();
            pdf += std::to_string(i) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        size_t xref_pos = pdf.size();
        pdf += "xref\n0 " + std::to_string(objects.size()) + "\n";
        pdf += "0000000000 65535 f \n";
        for (size_t i = 1; i < objects.size(); ++i) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", offsets[i]);
            pdf += buf;
        }
        pdf += "trailer\n<< /Size " + std::to_string(objects.size()) +
               " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref_pos) +
               "\n%%EOF\n";
        return pdf;
    }

    void save(const std::string& path) const {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) throw std::runtime_error("pdfgen: cannot open " + path);
        std::string b = bytes();
        std::fwrite(b.data(), 1, b.size(), f);
        std::fclose(f);
    }

private:
    struct Page {
        double width, height;
        std::string stream;
    };
    std::vector<Page> pages_;
    int next_obj_id_;
};

}  // namespace pdfgen
