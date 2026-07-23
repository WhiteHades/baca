# mereader-tui

<p align="center">
  <img src="resources/brand/mereader-tui.png" alt="mereader-tui pixel art logo" width="240">
</p>

<p align="center">
  <a href="https://github.com/whitehades/mereader-tui/actions/workflows/ci.yml"><img src="https://github.com/whitehades/mereader-tui/actions/workflows/ci.yml/badge.svg" alt="ci"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-gpl--3.0-cba6f7" alt="gpl 3.0 license"></a>
</p>

a fast, local-first terminal ebook reader and calibre library manager for linux. it reads epub, pdf, mobi, fb2, comic archives, markdown, and images in a keyboard-driven tui.

```text
        .--------. .--------.
       /  >_     | |  read  \
      /__________|_|__________\
           mereader-tui
```

![mereader-tui reader preview](resources/brand/preview.svg)

## features

- reads epub, epub3, pdf, fb2, txt, markdown, cbz, cbr, cb7, png, jpeg, gif, webp, bmp, and svg
- reads extractable, drm-free mobi, prc, azw, and azw3 files through `mobitool`
- indexes calibre libraries and ordinary book folders with the bundled fff search engine
- opens on an all-books dashboard with list and card views
- groups formats for the same book and prefers epub when it is available
- finds any indexed book through an independent picker with cover and metadata details
- saves reading progress and bookmarks automatically
- opens local files and http or https urls with a private offline cache
- opens clicked ebook links only when they are credential-free http or https urls
- renders images with kitty graphics, true-color ansi, or a text fallback
- provides regular expression search, a table of contents, metadata, links, themes, and configurable keys

## install

mereader-tui supports linux with a utf-8 locale. building needs gcc 14 or later, or clang 18 or later. it also needs rust 1.97, cargo, make, and `pkg-config`. the v0.1.1 build and test flow is verified on ubuntu 24.04 lts, debian 13, fedora 43, and arch linux rolling as of july 2026.

install the system dependencies for your distribution first.

### ubuntu 24.04 lts and debian 13

```sh
sudo apt update
sudo apt install git curl ca-certificates build-essential gcc-14 pkg-config python3 \
  sqlite3 libncurses-dev libsqlite3-dev libglib2.0-dev libxml2-dev libzip-dev \
  libarchive-dev libpcre2-dev libgdk-pixbuf-2.0-dev libpoppler-glib-dev \
  libcairo2-dev libcurl4-openssl-dev librsvg2-common webp-pixbuf-loader
```

### fedora 43

```sh
sudo dnf install git curl ca-certificates gcc gcc-c++ make pkgconf-pkg-config \
  python3 diffutils ncurses-devel sqlite sqlite-devel glib2-devel libxml2-devel \
  libzip-devel libarchive-devel pcre2-devel gdk-pixbuf2-devel \
  poppler-glib-devel cairo-devel libcurl-devel librsvg2
```

### arch linux

```sh
sudo pacman -Syu
sudo pacman -S --needed git curl ca-certificates base-devel pkgconf python ncurses \
  sqlite glib2 libxml2 libzip libarchive pcre2 gdk-pixbuf2 poppler-glib \
  cairo librsvg
```

python and the `sqlite3`, `timeout`, and `cmp` commands are test-only tools included so `make test` and `make installcheck` can verify the checkout. the installed c program does not invoke them.

install the pinned rust toolchain, then clone the release with its pinned fff submodule:

```sh
curl --proto '=https' --tlsv1.2 https://sh.rustup.rs -sSf | \
  sh -s -- -y --profile minimal --default-toolchain 1.97.0
. "$HOME/.cargo/env"
git clone --branch v0.1.1 --recurse-submodules \
  https://github.com/whitehades/mereader-tui.git
cd mereader-tui
```

github's automatic source zip and tar archives omit the required fff submodule contents and are not supported build inputs. if the repository was cloned without submodules, run `git submodule update --init --recursive`.

on ubuntu 24.04, select gcc 14 explicitly while installing for the current user:

```sh
make CC=gcc-14 doctor
make CC=gcc-14 user-install
```

on the other verified distributions:

```sh
make doctor
make user-install
```

verify the installed program:

```sh
~/.local/bin/mereader-tui --doctor
```

make sure `~/.local/bin` is in `PATH`. for a system-wide install under `/usr/local`, build without root first, then install the existing build:

```sh
make CC=gcc-14                    # ubuntu 24.04
sudo make CC=gcc-14 install
```

on the other verified distributions, use `make` followed by `sudo make install`.

the install contains `mereader-tui`, `libfff_c.so`, the default config, the `mereader-tui(1)` manual, and both project licenses. rust is needed to build from source, but not to run an installed build.

uninstall with `make user-uninstall` or `sudo make uninstall`. uninstalling does not delete personal config, history, bookmarks, or cached books.

## usage

```sh
mereader-tui                              # open the library
mereader-tui book.epub                    # open a local book
mereader-tui "path with spaces/book.pdf" # quoted paths work
mereader-tui https://example.com/book.epub
mereader-tui -r                           # print reading history
mereader-tui 3                            # open history row 3
mereader-tui alice wonderland             # fuzzy-match history
mereader-tui --doctor                     # print runtime diagnostics
```

run `man mereader-tui` for the complete command reference.

## library

set `LibraryPath` in `~/.config/mereader-tui/config.ini` to a calibre library or an ordinary folder of books. `LibraryPath = auto` checks `~/Calibre Library` and `~/Documents/Calibre Library`. `MEREADER_TUI_LIBRARY_PATH` overrides the config for one process.

a folder containing `metadata.db` is treated as a calibre library. the first shelf shows all logical books. the author shelf groups those books by author. ordinary folders use their directory structure as metadata when possible. files with the same stem are grouped as formats of one book.

| key | action |
| --- | --- |
| `j`, `k`, arrows | move the selection |
| `ctrl-f`, `ctrl-b`, page keys | move by one screen |
| `gg`, `G` | select the first or last item |
| `enter`, `l` | open the selected item |
| `/` | filter the current shelf through fff |
| `space space` | find any book with a cover and details preview |
| `v` | switch between list and card views |
| `a` | switch between all books and authors |
| `f` | choose a format for the selected book |
| `backspace`, `esc`, `h` | go back or clear the filter |
| `s` | cycle recent, title, and author sorting |
| `r` | rescan the library |
| `o` | open a path or url |
| `?` | show library help |
| `q` | quit |

epub is preferred over pdf, mobi or azw, and other supported formats. press `f` to choose a different format. that choice is saved for the book.

library controls are configured separately under `[Library Keymaps]`, so changing reader scrolling never changes library navigation. the library help view and footer show the active mappings. bindings accept named keys such as `pagedown`, chords such as `ctrl+f`, and two-key sequences such as `gg` or `space space`; invalid or conflicting library bindings are rejected with the two option names.

## reader

| key | action |
| --- | --- |
| `j`, `k`, arrows | scroll |
| `ctrl-f`, `ctrl-b`, page keys | move by pages |
| `gg`, `G` | jump to the beginning or end |
| `/`, `f2` | search forward or backward |
| `n`, `N` | repeat the search |
| `ctrl-o`, `ctrl-i` | move through jump history |
| `tab` | open the table of contents |
| `b`, `B` | save or list bookmarks |
| `M` | show metadata |
| `v` | switch pdf fixed and reflow views |
| `?`, `f1` | show all configured keys |
| `f12` | save the current terminal view as svg |
| `q`, `esc` | cancel or quit |

numeric prefixes repeat movement commands, such as `12j`, `3n`, or `120G`.

## config

mereader-tui creates `~/.config/mereader-tui/config.ini` on first use. these are the main defaults:

```ini
[General]
PreferredImageViewer = auto
LibraryPath = auto
MaxTextWidth = 80
TextJustification = justify
Pretty = no
PageScrollDuration = 0.2
ImageMode = auto

[Color Dark]
Background = #1d1c2b
Foreground = #cdd6f4
Accent = #cba6f7

[Color Light]
Background = #eff1f5
Foreground = #4c4f69
Accent = #8839ef
```

the shipped palette is based on catppuccin mocha and latte. see `resources/config.ini` for every reader and library key mapping.

## native fff integration

mereader-tui is written in c23. fff remains rust and is compiled from the pinned submodule into `libfff_c.so`. mereader-tui calls the fff c abi in the same process. it does not launch an fff command or need neovim.

the c adapter owns copied paths and scores. rust owns the opaque index and temporary fff results, which are released through matching c destructors.

## development

on ubuntu 24.04, use `make CC=gcc-14 test` instead of `make test-gcc`, and pass `CC=gcc-14` to `sanitize` and `installcheck`.

```sh
make test          # native and cli tests
make test-gcc      # clean gcc build
make test-clang    # clean clang build
make sanitize      # address and undefined behavior sanitizers
make installcheck  # staged install and uninstall
make format        # format c sources and headers
```

fff is pinned as a git submodule, so builds use a reviewed source revision and a locked cargo dependency graph. see the [dependency audit](docs/dependency-audit.md) for the reviewed revision, advisory reachability, and next review date.

## limits

- kindle print replica and azw4 books are not supported
- drm-protected mobi or azw files are not supported
- pdf reflow and search need an embedded text layer
- password-protected pdfs and encrypted comic archives are not supported
- document search works on laid-out lines, so a phrase split by wrapping may need a regular expression such as `word +next`
- direct image decoding is bounded; oversized or corrupt images remain placeholders

## license

gpl 3.0. the bundled fff component uses the mit license. both licenses are installed with mereader-tui.
