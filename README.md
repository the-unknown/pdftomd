# pdftomd

Konvertiert PDF-Dokumente zu strukturiertem Markdown (C++17).

- Texte Extraktion über die **Poppler** C++ API (Font-Informationen inkludiert)
- Automatische Erkennung von:
  - **Headings** – über ein font-size-basiertes Rangkmodell (Relativgrößen gegen Body-Text)
  - **Bold / Italic / Monospace** – über Font-Name-Substrings (`Bold`, `Oblique`, `Mono`, `Courier`, …)
  - **URLs** – werden zu autolinked `<https://…>`
  - **Aufzählungen** – Bullets (`•`, `-`, `*`, `◦`, `‣`, …) und nummerierte Listen, mit Einrückungs-Nesting
  - **Absätze & Zeilenumbrüche** – Gap-basiertes Clustering von Wörtern zu Zeilen/Blöcken
  - **Dehyphenierung** – Zeilenumbrüche mit `-` (klein) oder `–` werden rückgebaut
- Unit-Tests (reine Logik) **und** Integrationstests (echte, minimal generierte PDFs durch Poppler) mit **GoogleTest**

## Voraussetzungen (Libraries)

| Paket | Zweck |
|---|---|
| `poppler-cpp` / `libpoppler-cpp-dev` | PDF-Textextraktion (C++ API) |
| `googletest` / `libgtest-dev` | Unit-/Integrationstests |
| `cmake` (≥ 3.16) | Build-System |
| `pkg-config` | Abhängigkeitsauflösung |
| GCC/Clang mit C++17 | Compiler |

Installation (Debian/Ubuntu/Linux Mint):

```sh
sudo apt-get install -y \
  libpoppler-cpp-dev \
  libgtest-dev googletest \
  cmake pkg-config g++
```

## Build

```sh
cmake -B build
cmake --build build -j$(nproc)
```

Erzeugt:

- `build/pdftomd` – CLI
- `build/pdftomd_tests` – Test-Suite

## Tests

```sh
./build/pdftomd_tests                       # alle Tests
./build/pdftomd_tests --gtest_filter='IntegrationTest*'   # nur Poppler-Integration
```

Hinweis: Der Integrationstest `RealNourivaPdf` wird nur mitgebaut, wenn beim
CMake-Konfigurieren eine `nouriva_woche_1_quellen.pdf` im Projektverzeichnis
liegt (dynamisch via `-DREAL_PDF=…`).

## Nutzung

```sh
./build/pdftomd input.pdf                      # -> input.md
./build/pdftomd input.pdf -o out.md            # explizites Zielfile
./build/pdftomd input.pdf --stdout             # auf stdout
./build/pdftomd input.pdf --pages 1-3,5        # Seitenauswahl
./build/pdftomd input.pdf --no-urls            # keine Autolinks
./build/pdftomd input.pdf --no-dehyphenate     # Dehyphenierung abschalten
```

## Projektstruktur

```
src/pdf2md.h      – API (Datenmodell + reine Logik + Poppler-Wrapper)
src/pdf2md.cc     – Implementierung
src/main.cc       – CLI
tests/pdf_factory.h – minimale, gültige Test-PDFs generiert (Base-14-Fonts)
tests/test_pdf2md.cc – 53 Unit-Tests + 15 Integrationstests
CMakeLists.txt    – Build-Konfiguration
```

## Design-Hinweise

- Die Kernlogik (Gruppierung, Rendering, Linkify, Dehyphenation) ist poppler-frei
  und daher ohne PDF-Datei unit-testbar.
- Die Integrationstests generieren zur Laufzeit winzige, handbaute PDFs
  (WinAnsi, Base-14-Fonts) und treiben sie durch den echten Poppler-Pfad.
- Heading-Ränge sind heuristisch (Relative Größensignaturen gegen den
  häufigsten Body-Font); `–`-Bindestriche in Zeilenenden werden als
  Titelumbruch behandelt, `-` + Kleinschreibung als Dehyphenierung.
