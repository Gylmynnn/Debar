# debar - build & install helper around Meson.
#
# Default prefix is ~/.local so `make install` works without sudo (the binary
# lands in ~/.local/bin, which is normally already on PATH).
# For a system-wide install: sudo make install PREFIX=/usr/local
# Packaging/staging: make install DESTDIR=/tmp/pkgroot PREFIX=/usr

PREFIX ?= $(HOME)/.local
BUILD_DIR ?= build
BINDIR := $(PREFIX)/bin

.DEFAULT_GOAL := build

.PHONY: all build i install uninstall run clean help

all: build

$(BUILD_DIR)/build.ninja:
	meson setup $(BUILD_DIR)

build: $(BUILD_DIR)/build.ninja
	meson compile -C $(BUILD_DIR)

i:
	@rm -rf $(BUILD_DIR)
	@meson setup $(BUILD_DIR)
	@meson compile -C $(BUILD_DIR)

install: build
	install -Dm755 $(BUILD_DIR)/debar $(DESTDIR)$(BINDIR)/debar
	@echo "debar installed to $(DESTDIR)$(BINDIR)/debar"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/debar
	@echo "debar removed from $(DESTDIR)$(BINDIR)/debar"

run: build
	./$(BUILD_DIR)/debar

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Targets:"
	@echo "  make / make build   incremental build"
	@echo "  make i              clean full rebuild"
	@echo "  make install        install to $(BINDIR)"
	@echo "  make uninstall      remove the installed binary"
	@echo "  make run            build and run the bar"
	@echo "  make clean          remove the build directory"
	@echo ""
	@echo "Variables:"
	@echo "  PREFIX=$(PREFIX)"
	@echo "  BUILD_DIR=$(BUILD_DIR)"
	@echo "  DESTDIR=$(DESTDIR)"
