# DeepSeek Native for Linux — C + GTK3
PKGS     := gtk+-3.0 libcurl json-glib-1.0 libsecret-1
CC       ?= cc
BUILD    ?= release
PREFIX   ?= $(HOME)/.local

ifeq ($(BUILD),debug)
  OPT := -O0 -g3 -fsanitize=address,undefined
  LDOPT := -fsanitize=address,undefined
  TEST_HOOKS ?= 1
else
  OPT := -O2 -g
  LDOPT :=
  TEST_HOOKS ?= 0
endif
# Automated UI test hooks (DSN_SCRIPT, DSN_SNAPSHOT, ...): on in debug builds, off in release.
# A release-speed build with hooks: make BUILD=hooks TEST_HOOKS=1
ifeq ($(TEST_HOOKS),1)
  OPT += -DDSN_TEST_HOOKS
endif

CFLAGS   += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            -Wno-sign-compare $(OPT) $(shell pkg-config --cflags $(PKGS)) -Isrc
LDLIBS   += $(shell pkg-config --libs $(PKGS)) -lm -lpthread
LDFLAGS  += $(LDOPT)

SRC      := $(wildcard src/*.c)
OBJ      := $(patsubst src/%.c,build/$(BUILD)/%.o,$(SRC))
DEP      := $(OBJ:.o=.d)
BIN      := build/deepseek-native

.PHONY: all clean run install uninstall test

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/$(BUILD)/%.o: src/%.c | build/$(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

build/$(BUILD):
	mkdir -p $@

run: $(BIN)
	./$(BIN)

APPID    := io.github.lszl84.DeepSeekNative

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/deepseek-native
	install -Dm644 data/$(APPID).desktop $(DESTDIR)$(PREFIX)/share/applications/$(APPID).desktop
	for s in 64 128 256 512; do \
	  install -Dm644 data/icons/$$s.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/$${s}x$${s}/apps/$(APPID).png; \
	done
	@if [ -z "$(DESTDIR)" ]; then \
	  gtk-update-icon-cache -q -t $(PREFIX)/share/icons/hicolor 2>/dev/null || true; \
	  update-desktop-database -q $(PREFIX)/share/applications 2>/dev/null || true; \
	fi

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/deepseek-native $(DESTDIR)$(PREFIX)/share/applications/$(APPID).desktop
	for s in 64 128 256 512; do rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/$${s}x$${s}/apps/$(APPID).png; done

# Sandbox self-test: checks Landlock confinement of shell commands (no network, no API key needed).
test: $(OBJ)
	$(CC) $(CFLAGS) -o build/sandbox_probe tests/sandbox_probe.c build/$(BUILD)/process.o build/$(BUILD)/util.o \
	  build/$(BUILD)/settings.o $(LDFLAGS) $(LDLIBS)
	./build/sandbox_probe

clean:
	rm -rf build

-include $(DEP)
