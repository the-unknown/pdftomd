#include "pdf2md.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void print_usage(std::ostream& os) {
    os << "Usage: pdftomd [options] <input.pdf> [output.md]\n"
       << "\n"
       << "Converts the text content of a PDF document to Markdown.\n"
       << "\n"
       << "Options:\n"
       << "  -o, --output <file>    write to <file> (default: <input-stem>.md)\n"
       << "  -p, --pages <spec>     1-based page selection, e.g. '1,3-5' (default: all)\n"
       << "      --stdout           write markdown to stdout\n"
       << "      --no-urls          do not wrap bare URLs into autolinks\n"
       << "      --no-dehyphenate   do not merge hyphenated line wraps\n"
       << "  -h, --help             show this help\n";
}

// Parses "1,3-5" into {1,3,4,5}; returns false on malformed input.
bool parse_page_spec(const std::string& spec, std::vector<int>* out) {
    std::stringstream ss(spec);
    std::string part;
    bool any = false;
    while (std::getline(ss, part, ',')) {
        if (part.empty()) continue;
        auto dash = part.find('-');
        if (dash == std::string::npos) {
            try {
                out->push_back(std::stoi(part));
                any = true;
            } catch (...) {
                return false;
            }
        } else {
            try {
                int a = std::stoi(part.substr(0, dash));
                int b = std::stoi(part.substr(dash + 1));
                if (a > b) std::swap(a, b);
                for (int i = a; i <= b; ++i) out->push_back(i);
                any = true;
            } catch (...) {
                return false;
            }
        }
    }
    return any;
}

}  // namespace

int main(int argc, char** argv) {
    pdf2md::Options opts;
    std::string input;
    std::string output;
    bool to_stdout = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "error: missing value for " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            print_usage(std::cout);
            return 0;
        } else if (a == "-o" || a == "--output") {
            output = next("--output");
        } else if (a == "-p" || a == "--pages") {
            if (!parse_page_spec(next("--pages"), &opts.pages)) {
                std::cerr << "error: invalid --pages spec\n";
                return 2;
            }
        } else if (a == "--stdout") {
            to_stdout = true;
        } else if (a == "--no-urls") {
            opts.link_urls = false;
        } else if (a == "--no-dehyphenate") {
            opts.dehyphenate = false;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "error: unknown option: " << a << "\n";
            print_usage(std::cerr);
            return 2;
        } else if (input.empty()) {
            input = a;
        } else if (output.empty()) {
            output = a;
        } else {
            std::cerr << "error: too many arguments\n";
            return 2;
        }
    }

    if (input.empty()) {
        print_usage(std::cerr);
        return 2;
    }

    try {
        std::string md = pdf2md::convert_file(input, opts);
        if (to_stdout) {
            std::cout << md;
        } else {
            if (output.empty()) {
                // derive from input: strip .pdf, append .md
                std::string stem = input;
                auto dot = stem.rfind('.');
                auto slash = stem.rfind('/');
                if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
                    stem = stem.substr(0, dot);
                output = stem + ".md";
            }
            std::ofstream f(output, std::ios::binary);
            if (!f) throw std::runtime_error("cannot open output file: " + output);
            f << md;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
