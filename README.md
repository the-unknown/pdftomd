# pdftomd

Converts PDF documents to structured Markdown (C++17).

- Text extraction via the **Poppler** C++ API (including font information)
- Automatic detection of:
  - **Headings** – via a font-size-based rank model (relative sizes vs. body text)
  - **Bold / Italic / Monospace** – via font-name substrings (`Bold`, `Oblique`, `Mono`, `Courier`, …)
  - **URLs** – turned into autolinks `<https://…>`
  - **Lists** – bullets (`•`, `-`, `*`, `◦`, `‣`, …) and numbered lists, with indentation-based nesting
  - **Paragraphs & line breaks** – gap-based clustering of words into lines/blocks
  - **Dehyphenation** – line-end `-` (lowercase) and `–` (en dash) are rejoined
- Unit tests (pure logic) **and** integration tests (real, minimally generated PDFs driven through Poppler) with **GoogleTest**

## Prerequisites (Libraries)

| Package | Purpose |
|---|---|
| `poppler-cpp` / `libpoppler-cpp-dev` | PDF text extraction (C++ API) |
| `googletest` / `libgtest-dev` | Unit/integration tests |
| `cmake` (≥ 3.16) | Build system |
| `pkg-config` | Dependency resolution |
| GCC/Clang with C++17 | Compiler |

Installation (Debian/Ubuntu/Linux Mint):

```sh
sudo apt-get install -y \
  libpoppler-cpp-dev \
  libgtest-dev googletest \
  cmake pkg-config g++
```

> **Troubleshooting: `Package 'poppler-cpp' not found` although
> `libpoppler-cpp-dev` is installed**
>
> This usually means CMake picked up a `pkg-config` from another toolchain
> (e.g. Homebrew/linuxbrew), which only searches its own prefix. Verify with:
>
> ```sh
> /usr/bin/pkg-config --modversion poppler-cpp   # system copy must find it
> ```
>
> Fix for a single configure run:
>
> ```sh
> cmake -B build -DPKG_CONFIG_EXECUTABLE=/usr/bin/pkg-config
> ```
>
> …or extend the search path (persistent if exported):
>
> ```sh
> export PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig
> cmake -B build
> ```

## Build

```sh
cmake -B build
cmake --build build -j$(nproc)
```

Produces:

- `build/pdftomd` – CLI
- `build/pdftomd_tests` – test suite

## Installation

There is no dedicated installer – CMake installs the binary into the usual
prefix (default `/usr/local`):

```sh
cmake -B build
cmake --build build -j$(nproc)
sudo cmake --install build          # -> /usr/local/bin/pdftomd
```

Alternative without CMake:

```sh
sudo cp build/pdftomd /usr/local/bin/
```

The install prefix can be set: `cmake -B build -DCMAKE_INSTALL_PREFIX=/opt/pdftomd`.
After installation the command is available everywhere: `pdftomd input.pdf`.

Note: the runtime dependency `libpoppler` must be present on the target
system (see above).

## Tests

```sh
./build/pdftomd_tests                       # all tests
./build/pdftomd_tests --gtest_filter='IntegrationTest*'   # Poppler integration only
```

Note: the integration test `RealNourivaPdf` is only built if a
`nouriva_woche_1_quellen.pdf` exists in the project directory at CMake
configure time (wired up via `-DREAL_PDF=…`).

## Usage

```sh
./build/pdftomd input.pdf                      # -> input.md
./build/pdftomd input.pdf -o out.md            # explicit output file
./build/pdftomd input.pdf --stdout             # to stdout
./build/pdftomd input.pdf --pages 1-3,5        # page selection
./build/pdftomd input.pdf --no-urls            # no autolinks
./build/pdftomd input.pdf --no-dehyphenate     # disable dehyphenation
```

## Project layout

```
src/pdf2md.h      – API (data model + pure logic + Poppler wrappers)
src/pdf2md.cc     – implementation
src/main.cc       – CLI
tests/pdf_factory.h – generates minimal, valid test PDFs (Base-14 fonts)
tests/test_pdf2md.cc – 53 unit tests + 15 integration tests
CMakeLists.txt    – build configuration
```

## Design notes

- The core logic (grouping, rendering, linkify, dehyphenation) is
  poppler-free and therefore unit-testable without any PDF file.
- Integration tests generate tiny, hand-crafted PDFs (WinAnsi, Base-14
  fonts) at runtime and drive them through the real Poppler path.
- Heading ranks are heuristic (relative size signatures vs. the most
  common body font); an en dash `–` at the end of a line is treated as a
  title wrap, `-` followed by lowercase as hyphenation.
