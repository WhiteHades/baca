.POSIX:

CC = cc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
MANDIR ?= $(DATADIR)/man
INSTALL ?= install
CARGO ?= cargo
RUST_TOOLCHAIN ?= 1.97.0
FFF_TARGET_DIR ?= $(CURDIR)/vendor/fff/target
FFF_CFLAGS ?=
FFF_CXXFLAGS ?=
LIBDIR ?= $(PREFIX)/lib/mereader-tui
LICENSEDIR ?= $(DATADIR)/licenses/mereader-tui
PKGS = ncursesw sqlite3 glib-2.0 libxml-2.0 libzip libarchive libpcre2-8 gdk-pixbuf-2.0 poppler-glib cairo libcurl
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude -Ivendor/fff/crates/fff-c/include $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O2 -g
CFLAGS += -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 \
	-Wstrict-prototypes -Wmissing-prototypes -Werror=implicit-function-declaration
LDLIBS += -Lbuild -lfff_c $(shell pkg-config --libs $(PKGS)) -lm -pthread
LDFLAGS += -Wl,-rpath,'$$ORIGIN:$$ORIGIN/..:$$ORIGIN/../lib/mereader-tui'
TEST_LDLIBS = -lutil -lfontconfig

SOURCES = \
	src/app.c \
	src/catalog.c \
	src/common.c \
	src/comic.c \
	src/config.c \
	src/database.c \
	src/document.c \
	src/epub.c \
	src/fb2.c \
	src/graphics.c \
	src/html.c \
	src/image.c \
	src/keymap.c \
	src/layout.c \
	src/library.c \
	src/library_shelf.c \
	src/library_tui.c \
	src/mobi.c \
	src/pdf.c \
	src/platform.c \
	src/remote.c \
	src/search.c \
	src/terminal_runtime.c \
	src/text.c \
	src/text_input.c \
	src/reader_tui.c \
	src/main.c
OBJECTS = $(SOURCES:src/%.c=build/%.o)
TEST_SOURCES = tests/test_main.c tests/test_catalog.c tests/test_common.c tests/test_comic.c tests/test_config.c tests/test_database.c \
	tests/test_document.c tests/test_fb2.c tests/test_graphics.c tests/test_layout.c tests/test_library.c \
	tests/test_library_shelf.c \
	tests/test_remote.c tests/test_search.c tests/test_support.c tests/test_text.c

TEST_SOURCES += tests/test_pdf.c
TEST_OBJECTS = $(TEST_SOURCES:tests/%.c=build/tests/%.o)
LIB_OBJECTS = $(filter-out build/main.o,$(OBJECTS))

.PHONY: all check-deps clean doctor fff format install installcheck sanitize test test-clang test-gcc tests uninstall user-install user-uninstall

all: check-deps build/mereader-tui

check-deps:
	@command -v pkg-config >/dev/null 2>&1 || { printf '%s\n' 'error: pkg-config is required' >&2; exit 1; }
	@missing=''; for package in $(PKGS); do \
		pkg-config --exists "$$package" || missing="$$missing $$package"; \
	done; \
	if [ -n "$$missing" ]; then \
		printf 'error: missing development packages:%s\n' "$$missing" >&2; \
		exit 1; \
	fi
	@command -v "$(CARGO)" >/dev/null 2>&1 || { printf '%s\n' 'error: cargo is required to build bundled fff' >&2; exit 1; }
	@"$(CARGO)" +"$(RUST_TOOLCHAIN)" --version >/dev/null 2>&1 || { \
		printf '%s\n' 'error: Rust toolchain $(RUST_TOOLCHAIN) is required to build bundled fff' >&2; exit 1; \
	}

doctor: check-deps
	@printf 'build dependencies: ready\n'
	@if command -v mobitool >/dev/null 2>&1; then \
		printf 'MOBI/AZW runtime: %s\n' "$$(command -v mobitool)"; \
	else \
		printf 'MOBI/AZW runtime: unavailable (install mobitool from libmobi)\n'; \
	fi

fff: build/libfff_c.so

build/libfff_c.so: vendor/fff/Cargo.lock
	@mkdir -p build
	CFLAGS="$(FFF_CFLAGS)" CXXFLAGS="$(FFF_CXXFLAGS)" CARGO_TARGET_DIR="$(FFF_TARGET_DIR)" \
		"$(CARGO)" +"$(RUST_TOOLCHAIN)" build \
		--manifest-path vendor/fff/Cargo.toml --locked --release -p fff-c
	cp "$(FFF_TARGET_DIR)/release/libfff_c.so" $@

build/mereader-tui: build/libfff_c.so $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

build/tests/test_mereader_tui: build/mereader-tui $(LIB_OBJECTS) $(TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(LIB_OBJECTS) $(TEST_OBJECTS) $(LDLIBS) $(TEST_LDLIBS)

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

build/tests/%.o: tests/%.c
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

test tests: build/tests/test_mereader_tui build/mereader-tui
	./build/tests/test_mereader_tui
	sh ./tests/cli_test.sh ./build/mereader-tui

sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	LD_LIBRARY_PATH="$(CURDIR)/build:$${LD_LIBRARY_PATH:-}" \
	$(MAKE) CC="$(CC)" \
		CFLAGS="-O1 -g -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
		-Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Werror=implicit-function-declaration \
		-fsanitize=address,undefined -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=address,undefined" test

test-gcc:
	$(MAKE) clean
	$(MAKE) CC=gcc test

test-clang:
	$(MAKE) clean
	$(MAKE) CC=clang test

format:
	clang-format -i include/mereader-tui/*.h src/*.c tests/*.c tests/*.h

install: build/mereader-tui
	$(INSTALL) -Dm755 build/mereader-tui "$(DESTDIR)$(BINDIR)/mereader-tui"
	$(INSTALL) -Dm755 build/libfff_c.so "$(DESTDIR)$(LIBDIR)/libfff_c.so"
	$(INSTALL) -Dm644 resources/config.ini "$(DESTDIR)$(DATADIR)/mereader-tui/config.ini"
	$(INSTALL) -Dm644 docs/mereader-tui.1 "$(DESTDIR)$(MANDIR)/man1/mereader-tui.1"
	$(INSTALL) -Dm644 LICENSE "$(DESTDIR)$(LICENSEDIR)/LICENSE"
	$(INSTALL) -Dm644 vendor/fff/LICENSE "$(DESTDIR)$(LICENSEDIR)/fff-LICENSE"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/mereader-tui"
	rm -f "$(DESTDIR)$(LIBDIR)/libfff_c.so"
	rm -f "$(DESTDIR)$(DATADIR)/mereader-tui/config.ini"
	rm -f "$(DESTDIR)$(MANDIR)/man1/mereader-tui.1"
	rm -f "$(DESTDIR)$(LICENSEDIR)/LICENSE"
	rm -f "$(DESTDIR)$(LICENSEDIR)/fff-LICENSE"
	-rmdir "$(DESTDIR)$(DATADIR)/mereader-tui" 2>/dev/null
	-rmdir "$(DESTDIR)$(LIBDIR)" 2>/dev/null
	-rmdir "$(DESTDIR)$(LICENSEDIR)" 2>/dev/null

user-install:
	$(MAKE) install PREFIX="$(HOME)/.local"

user-uninstall:
	$(MAKE) uninstall PREFIX="$(HOME)/.local"

installcheck: build/mereader-tui
	rm -rf build/install-root
	$(MAKE) install DESTDIR="$(CURDIR)/build/install-root" PREFIX=/usr
	test -x build/install-root/usr/bin/mereader-tui
	test -x build/install-root/usr/lib/mereader-tui/libfff_c.so
	test -f build/install-root/usr/share/mereader-tui/config.ini
	test -f build/install-root/usr/share/man/man1/mereader-tui.1
	test -f build/install-root/usr/share/licenses/mereader-tui/LICENSE
	test -f build/install-root/usr/share/licenses/mereader-tui/fff-LICENSE
	cmp resources/config.ini build/install-root/usr/share/mereader-tui/config.ini
	cmp docs/mereader-tui.1 build/install-root/usr/share/man/man1/mereader-tui.1
	cmp LICENSE build/install-root/usr/share/licenses/mereader-tui/LICENSE
	cmp vendor/fff/LICENSE build/install-root/usr/share/licenses/mereader-tui/fff-LICENSE
	LD_LIBRARY_PATH= LD_PRELOAD= build/install-root/usr/bin/mereader-tui --version
	XDG_CONFIG_HOME="$(CURDIR)/build/install-root/xdg-config" \
		XDG_CACHE_HOME="$(CURDIR)/build/install-root/xdg-cache" \
		LD_LIBRARY_PATH= LD_PRELOAD= \
		build/install-root/usr/bin/mereader-tui --doctor
	$(MAKE) uninstall DESTDIR="$(CURDIR)/build/install-root" PREFIX=/usr
	test ! -e build/install-root/usr/bin/mereader-tui
	test ! -e build/install-root/usr/lib/mereader-tui/libfff_c.so
	test ! -e build/install-root/usr/share/mereader-tui/config.ini
	test ! -e build/install-root/usr/share/man/man1/mereader-tui.1
	test ! -e build/install-root/usr/share/licenses/mereader-tui/LICENSE
	test ! -e build/install-root/usr/share/licenses/mereader-tui/fff-LICENSE
	test ! -e build/install-root/usr/lib/mereader-tui
	test ! -e build/install-root/usr/share/mereader-tui
	test ! -e build/install-root/usr/share/licenses/mereader-tui

clean:
	rm -rf build

-include $(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)
