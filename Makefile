# SPDX-License-Identifier: GPL-2.0-only
.POSIX:

include config.mk
include META

GIT_HEAD := $(shell git rev-parse --verify HEAD 2>/dev/null)
GIT_REVISION := $(shell git rev-parse --short HEAD 2>/dev/null)
GIT_VERSION_TAG := $(shell git rev-parse -q --verify 'refs/tags/$(VERSION)^{commit}' 2>/dev/null)
GIT_DIRTY := $(shell test -z "$$(git status --porcelain 2>/dev/null)" || printf '%s' '-dirty')
ifeq ($(GIT_HEAD),)
$(error a Git HEAD is required to derive the build version)
endif
ifeq ($(GIT_HEAD)$(GIT_DIRTY),$(GIT_VERSION_TAG))
BUILD_VERSION := $(VERSION)
else
BUILD_VERSION := $(VERSION)-$(GIT_REVISION)$(GIT_DIRTY)
endif
override CPPFLAGS += -DSNAJPAGENT_NAME='"$(NAME)"' -DSNAJPAGENT_VERSION='"$(BUILD_VERSION)"'
override CFLAGS += -pthread
override LDFLAGS += -pthread

BIN = $(NAME)
TARGET_OS := $(shell uname -s)
ifeq ($(TARGET_OS),Darwin)
DEBUG_SYMBOLS = $(BIN).dSYM
else
DEBUG_SYMBOLS = $(BIN).debug
endif
EVIDENCE_DIR ?= build/release-evidence/current-host
RELEASE_PLATFORMS ?= linux-x86_64 linux-aarch64 macos-x86_64 macos-arm64
RELEASE_EVIDENCE_DIRS ?=
LIVE_CONFIG ?= $(HOME)/.$(NAME)/config.ini
LIVE_WORKSPACE ?= $(CURDIR)
LIVE_RESULT_ROOT ?=
TMUX_TEST_ROOT ?= $(CURDIR)/build/tmux-test
COMMON_SRC = src/base.c src/platform.c src/config.c src/secret_source.c src/credential.c src/auth.c src/auth_http.c src/login.c src/secret.c src/instructions.c src/json.c src/wire.c src/context.c src/provider_retry.c src/provider.c src/model_cache.c src/tools.c src/tools_read.c src/irc.c src/irc_runtime.c src/sse.c src/responses.c src/turn.c src/store.c src/store_lookup.c src/store_lifecycle.c src/tools_patch.c src/history.c src/term.c src/render.c src/render_prepare.c src/cli.c src/ui.c src/app_events.c src/app_stream.c src/app_lifecycle.c src/app_compact.c src/app_provider.c src/app.c
COMMON_OBJ = $(COMMON_SRC:.c=.o)
HEADERS = src/snajpagent.h src/base.h src/config.h src/secret_source.h src/credential.h src/auth.h src/login.h src/secret.h src/instructions.h src/json.h src/snag_jansson.h src/snag_jansson_abi.h src/wire.h src/context.h src/provider_retry.h src/provider.h src/model_cache.h src/tools.h src/tools_patch.h src/irc.h src/irc_internal.h src/sse.h src/responses.h src/turn.h src/store.h src/store_internal.h src/term.h src/render.h src/cli.h src/app.h src/app_internal.h src/ui.h src/history.h
DEPFLAGS = -MMD -MP
FIXTURE_BIN = tests/$(NAME)-fixture
TEST_BIN = tests/test_base tests/test_config tests/test_irc tests/test_instructions tests/test_credential tests/test_sse tests/test_json tests/test_wire tests/test_responses tests/test_provider_retry tests/test_provider_transport tests/test_context tests/test_model_cache tests/test_render tests/test_turn tests/test_tools tests/test_store $(FIXTURE_BIN)
BUILD_INPUTS = build/.build-inputs

all: $(BIN)

.c.o:
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) $(DEPFLAGS) -Isrc -c $< -o $@

$(BIN): $(COMMON_OBJ) src/main.o
	@set -e; stage=$$(mktemp -d build/.link.XXXXXX); \
	trap 'rm -rf "$$stage"' 0 1 2 3 15; \
	$(CC) $(LDFLAGS) -o "$$stage/$(BIN)" $(COMMON_OBJ) src/main.o $(LDLIBS) $(CURL_LIBS); \
	if test '$(DEBUG)' = 0; then \
		if test '$(TARGET_OS)' = Darwin; then \
			$(DSYMUTIL) "$$stage/$(BIN)" -o "$$stage/$(DEBUG_SYMBOLS)"; \
			$(STRIP) -S -x "$$stage/$(BIN)"; \
		else \
			$(OBJCOPY) --only-keep-debug "$$stage/$(BIN)" "$$stage/$(DEBUG_SYMBOLS)"; \
			$(STRIP) --strip-all "$$stage/$(BIN)"; \
			$(OBJCOPY) --add-gnu-debuglink="$$stage/$(DEBUG_SYMBOLS)" "$$stage/$(BIN)"; \
		fi; \
		rm -rf '$(DEBUG_SYMBOLS)'; \
		mv "$$stage/$(DEBUG_SYMBOLS)" '$(DEBUG_SYMBOLS)'; \
	fi; \
	mv "$$stage/$(BIN)" '$@'

$(COMMON_OBJ) src/main.o $(TEST_BIN): $(BUILD_INPUTS)

$(BUILD_INPUTS): FORCE
	@mkdir -p build
	@tmp='$@.tmp'; \
	trap 'rm -f "$$tmp"' 0 1 2 3 15; \
	{ \
		printf '%s\n' 'CC=$(CC)'; \
		printf '%s\n' 'DEBUG=$(DEBUG)'; \
		printf '%s\n' 'TARGET_OS=$(TARGET_OS)'; \
		printf '%s\n' 'STRIP=$(STRIP)'; \
		printf '%s\n' 'OBJCOPY=$(OBJCOPY)'; \
		printf '%s\n' 'DSYMUTIL=$(DSYMUTIL)'; \
		printf '%s\n' 'CPPFLAGS=$(CPPFLAGS)'; \
		printf '%s\n' 'JANSSON_CFLAGS=$(JANSSON_CFLAGS)'; \
		printf '%s\n' 'CURL_CFLAGS=$(CURL_CFLAGS)'; \
		printf '%s\n' 'CFLAGS=$(CFLAGS)'; \
		printf '%s\n' 'DEPFLAGS=$(DEPFLAGS)'; \
		printf '%s\n' 'LDFLAGS=$(LDFLAGS)'; \
		printf '%s\n' 'LDLIBS=$(LDLIBS)'; \
		printf '%s\n' 'CURL_LIBS=$(CURL_LIBS)'; \
		cksum Makefile config.mk META; \
	} >"$$tmp"; \
	if test -r '$@' && cmp -s "$$tmp" '$@'; then \
		if test '$(DEBUG)' = 0 && test ! -e '$(DEBUG_SYMBOLS)'; then \
			rm -f $(BIN); \
			touch '$@'; \
		fi; \
	else \
		rm -f $(BIN) $(COMMON_OBJ) src/main.o $(TEST_BIN); \
		rm -rf tests/.fixture-obj $(BIN).debug $(BIN).dSYM; \
		mv -f "$$tmp" '$@'; \
	fi

$(FIXTURE_BIN): $(COMMON_SRC) src/main.c tests/fixture_provider.c $(HEADERS)
	rm -rf tests/.fixture-obj
	mkdir -p tests/.fixture-obj/src tests/.fixture-obj/tests
	for f in $(COMMON_SRC) src/main.c tests/fixture_provider.c; do \
		o=tests/.fixture-obj/$${f%.c}.o; \
		echo "  CC $$f"; $(CC) $(CPPFLAGS) -DSNAJPAGENT_TEST_FIXTURE=1 $(JANSSON_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) -O0 -Isrc -c $$f -o $$o || exit 1; \
	done
	objs=; for f in $(COMMON_SRC) src/main.c tests/fixture_provider.c; do \
		objs="$$objs tests/.fixture-obj/$${f%.c}.o"; \
	done; \
	$(CC) $(LDFLAGS) -o $@ $$objs $(LDLIBS) $(CURL_LIBS)

tests/test_base: src/base.c src/platform.c tests/test_base.c src/base.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ src/base.c src/platform.c tests/test_base.c

tests/test_config: src/base.c src/platform.c src/config.c src/secret_source.c tests/test_config.c src/base.h src/config.h src/secret_source.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ src/base.c src/platform.c src/config.c src/secret_source.c tests/test_config.c $(LDLIBS)

tests/test_irc: src/base.c src/platform.c src/config.c src/secret_source.c src/irc.c src/irc_runtime.c tests/test_irc.c src/base.h src/config.h src/secret_source.h src/cli.h src/irc.h src/irc_internal.h src/snajpagent.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ \
		src/base.c src/platform.c src/config.c src/secret_source.c src/irc.c src/irc_runtime.c tests/test_irc.c $(LDLIBS)

tests/test_credential: src/base.c src/platform.c src/credential.c src/secret_source.c tests/test_credential.c src/base.h src/credential.h src/secret_source.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ src/base.c src/platform.c src/credential.c src/secret_source.c tests/test_credential.c $(LDLIBS)

tests/test_instructions: src/base.c src/platform.c src/json.c src/instructions.c tests/test_instructions.c src/base.h src/json.h src/instructions.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/platform.c src/json.c src/instructions.c tests/test_instructions.c $(LDLIBS)

tests/test_sse: src/base.c src/platform.c src/sse.c tests/test_sse.c src/base.h src/sse.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ src/base.c src/platform.c src/sse.c tests/test_sse.c

tests/test_json: src/base.c src/platform.c src/json.c tests/test_json.c src/base.h src/json.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/platform.c src/json.c tests/test_json.c $(LDLIBS)

tests/test_wire: src/base.c src/platform.c src/json.c src/wire.c tests/test_wire.c src/base.h src/json.h src/snag_jansson.h src/snag_jansson_abi.h src/wire.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/platform.c src/json.c src/wire.c tests/test_wire.c $(LDLIBS)

tests/test_responses: src/base.c src/platform.c src/json.c src/sse.c src/responses.c src/turn.c \
		tests/test_responses.c src/base.h src/json.h src/sse.h src/responses.h src/turn.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/platform.c src/json.c src/sse.c src/responses.c src/turn.c \
		tests/test_responses.c $(LDLIBS)

tests/test_provider_retry: src/provider_retry.c tests/test_provider_retry.c src/provider_retry.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ src/provider_retry.c tests/test_provider_retry.c

tests/test_provider_transport: $(COMMON_SRC) tests/test_provider_transport.c $(HEADERS)
	$(CC) $(CPPFLAGS) -DSNAJPAGENT_TEST_TRANSPORT_ENDPOINTS=1 $(JANSSON_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ $(COMMON_SRC) tests/test_provider_transport.c $(LDLIBS) $(CURL_LIBS)

tests/test_context: src/base.c src/platform.c src/config.c src/secret_source.c src/json.c src/instructions.c src/context.c src/turn.c src/store.c src/store_lookup.c src/store_lifecycle.c tests/test_context.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/platform.c src/config.c src/secret_source.c src/json.c src/instructions.c src/context.c src/turn.c src/store.c src/store_lookup.c src/store_lifecycle.c \
		tests/test_context.c $(LDLIBS)

tests/test_model_cache: src/base.c src/platform.c src/config.c src/secret_source.c src/json.c src/instructions.c src/turn.c src/store.c src/model_cache.c tests/test_model_cache.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/platform.c src/config.c src/secret_source.c src/json.c src/instructions.c src/turn.c src/store.c src/model_cache.c \
		tests/test_model_cache.c $(LDLIBS)

tests/test_render: src/base.c src/platform.c src/json.c src/history.c src/term.c src/render.c src/render_prepare.c tests/test_render.c \
		src/base.h src/json.h src/term.h src/render.h src/snajpagent.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/platform.c src/json.c src/history.c src/term.c src/render.c src/render_prepare.c tests/test_render.c $(LDLIBS)

tests/test_turn: src/base.c src/platform.c src/json.c src/turn.c tests/test_turn.c src/base.h src/json.h src/turn.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/platform.c src/json.c src/turn.c tests/test_turn.c $(LDLIBS)


tests/test_tools: src/base.c src/platform.c src/json.c src/wire.c src/credential.c src/secret.c src/config.c src/secret_source.c src/turn.c src/tools.c src/tools_read.c src/tools_patch.c tests/test_tools.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) -O0 $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/platform.c src/json.c src/wire.c src/credential.c src/secret.c \
		src/config.c src/secret_source.c src/turn.c src/tools.c src/tools_read.c src/tools_patch.c tests/test_tools.c $(LDLIBS)

tests/test_store: src/base.c src/platform.c src/json.c src/instructions.c src/turn.c src/store.c src/store_lookup.c src/store_lifecycle.c tests/test_store.c $(HEADERS)
	$(CC) $(CPPFLAGS) -DSNAJPAGENT_TEST_FIXTURE=1 $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/platform.c src/json.c src/instructions.c src/turn.c src/store.c src/store_lookup.c src/store_lifecycle.c tests/test_store.c $(LDLIBS)

check: $(TEST_BIN)
	./tests/test_base
	./tests/test_config
	./tests/test_irc
	./tests/test_instructions
	./tests/test_credential
	./tests/test_sse
	./tests/test_json
	./tests/test_wire
	./tests/test_responses
	./tests/test_provider_retry
	./tests/test_provider_transport
	./tests/test_context
	./tests/test_model_cache
	./tests/test_render
	./tests/test_turn
	./tests/test_tools
	./tests/test_store
	SNAJPAGENT_TEST_NAME='$(NAME)' SNAJPAGENT_TEST_VERSION='$(BUILD_VERSION)' \
		./tests/test_cli.sh ./$(FIXTURE_BIN)
	@if command -v tmux >/dev/null 2>&1; then \
		$(MAKE) tmuxcheck; \
	else \
		printf '%s\n' 'tmux_terminal: skipped (tmux unavailable)'; \
	fi
	$(MAKE) stylecheck
	$(MAKE) depscheck
	$(MAKE) portabilitycheck
	$(MAKE) depclosurecheck
	$(MAKE) evidencetoolcheck
	$(MAKE) sizecheck

stylecheck:
	./tools/check_style.sh

depscheck:
	python3 ./tools/check_deps.py

portabilitycheck:
	python3 ./tools/check_portability.py

depclosurecheck: $(BIN)
	python3 ./tools/check_dependency_closure.py ./$(BIN)

sanitizercheck:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(MAKE) DEBUG=1 check CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

releasecheck:
	$(MAKE) check
	if command -v clang >/dev/null 2>&1; then \
		$(MAKE) clean && $(MAKE) CC=clang check; \
	else \
		printf '%s\n' 'releasecheck: clang unavailable; skipped clang check'; \
	fi
	$(MAKE) sanitizercheck

livecheck: $(BIN)
	python3 ./tools/live_provider_check.py ./$(BIN)

tmuxcheck: $(BIN) $(FIXTURE_BIN)
	@command -v tmux >/dev/null 2>&1 || { \
		printf '%s\n' 'tmuxcheck: tmux is required' >&2; exit 2; \
	}
	@test -n "$(TMUX_TEST_ROOT)"
	@case "$(abspath $(TMUX_TEST_ROOT))" in \
		"$(abspath $(CURDIR))/build/"*) ;; \
		*) printf '%s\n' 'tmuxcheck: TMUX_TEST_ROOT must be below build/' >&2; \
		   exit 2 ;; \
	esac
	rm -rf "$(TMUX_TEST_ROOT)"
	mkdir -p -m 700 "$(TMUX_TEST_ROOT)/home" "$(TMUX_TEST_ROOT)/work"
	HOME="$(TMUX_TEST_ROOT)/home" LC_ALL=C.utf8 \
		python3 ./tests/tmux_terminal.py fixture \
		./$(FIXTURE_BIN) "$(TMUX_TEST_ROOT)/work" \
		"$(TMUX_TEST_ROOT)/run"
	HOME="$(TMUX_TEST_ROOT)/home" LC_ALL=C.utf8 \
		python3 ./tests/tmux_terminal.py irc ./$(BIN) \
		"$(TMUX_TEST_ROOT)/irc"

terminallivecheck: $(BIN)
	@test -n "$(LIVE_RESULT_ROOT)" || { \
		printf '%s\n' 'terminallivecheck: set LIVE_RESULT_ROOT' >&2; exit 2; \
	}
	@test -f "$(LIVE_CONFIG)" || { \
		printf '%s\n' 'terminallivecheck: LIVE_CONFIG is not a file' >&2; exit 2; \
	}
	python3 ./tests/tmux_terminal.py live ./$(BIN) \
		"$(LIVE_WORKSPACE)" "$(LIVE_CONFIG)" "$(LIVE_RESULT_ROOT)"

evidencebundle: $(BIN) $(FIXTURE_BIN)
	rm -rf $(EVIDENCE_DIR)
	python3 ./tools/collect_release_evidence.py ./$(BIN) $(EVIDENCE_DIR) --fixture ./$(FIXTURE_BIN) --skip-live

evidencecheck:
	python3 ./tools/check_release_evidence.py $(EVIDENCE_DIR)

evidencetoolcheck:
	python3 ./tools/check_release_evidence.py --self-test
	python3 ./tools/check_release_matrix.py --self-test

evidencematrixcheck:
	test -n "$(RELEASE_EVIDENCE_DIRS)" || { printf '%s\n' 'evidencematrixcheck: set RELEASE_EVIDENCE_DIRS to per-platform evidence directories'; exit 2; }
	set --; for p in $(RELEASE_PLATFORMS); do set -- "$$@" --require-platform "$$p"; done; \
		python3 ./tools/check_release_matrix.py --require-terminal --require-live "$$@" $(RELEASE_EVIDENCE_DIRS)

releaseevidence: $(BIN) $(FIXTURE_BIN)
	rm -rf $(EVIDENCE_DIR)
	python3 ./tools/collect_release_evidence.py ./$(BIN) $(EVIDENCE_DIR) --fixture ./$(FIXTURE_BIN) --require-live
	python3 ./tools/check_release_evidence.py $(EVIDENCE_DIR) --require-terminal --require-live

sizecheck:
	@prod_c=$$(find src -type f -name '*.c' -print0 | \
		sort -z | xargs -0 cat | wc -l | tr -d ' '); \
	prod_h=$$(find src -type f -name '*.h' -print0 | \
		sort -z | xargs -0 cat | wc -l | tr -d ' '); \
	test_c=$$(find tests -type f -name '*.c' -print0 | \
		sort -z | xargs -0 cat | wc -l | tr -d ' '); \
	prod_c_soft=32768; prod_c_hard=49152; \
	prod_h_soft=16384; prod_h_hard=65536; \
	test_c_soft=16384; test_c_hard=32768; \
	largest=$$(find src -type f \( -name '*.c' -o -name '*.h' \) -exec wc -l {} + | \
		awk '$$2 != "total" && $$1 > max { max = $$1; file = $$2 } END { if (file == "") print "0 -"; else print max " " file }'); \
	largest_lines=$$(printf '%s\n' "$$largest" | awk '{ print $$1 }'); \
	largest_file=$$(printf '%s\n' "$$largest" | cut -d ' ' -f 2-); \
	printf 'production C lines: %s / %s soft / %s hard\n' \
		"$$prod_c" "$$prod_c_soft" "$$prod_c_hard"; \
	printf 'production header lines: %s / %s soft / %s hard\n' \
		"$$prod_h" "$$prod_h_soft" "$$prod_h_hard"; \
	printf 'test C lines: %s / %s soft / %s hard\n' \
		"$$test_c" "$$test_c_soft" "$$test_c_hard"; \
	printf 'largest production C/header file: %s lines %s / 2000 review trigger\n' "$$largest_lines" "$$largest_file"; \
	if [ "$$prod_c" -gt "$$prod_c_soft" ]; then \
		printf 'source-budget review: production C exceeds the %s-line soft limit\n' "$$prod_c_soft"; \
	fi; \
	if [ "$$prod_h" -gt "$$prod_h_soft" ]; then \
		printf 'source-budget review: production headers exceed the %s-line soft limit\n' "$$prod_h_soft"; \
	fi; \
	if [ "$$test_c" -gt "$$test_c_soft" ]; then \
		printf 'source-budget review: test C exceeds the %s-line soft limit\n' "$$test_c_soft"; \
	fi; \
	if [ "$$largest_lines" -gt 2000 ]; then \
		printf 'line-budget review: %s exceeds the 2000-line simplicity-review trigger\n' "$$largest_file"; \
	fi; \
	test "$$prod_c" -le "$$prod_c_hard" && \
		test "$$prod_h" -le "$$prod_h_hard" && \
		test "$$test_c" -le "$$test_c_hard"

clean:
	rm -f $(BIN) src/*.o src/*.d $(TEST_BIN)
	rm -rf tests/.fixture-obj build $(BIN).debug $(BIN).dSYM

help:
	@printf '%s\n' \
		'make                  Host-native production build (default DEBUG=0)' \
		'make DEBUG=1          Debug build: -Og, symbols, frame pointers, no stripping' \
		'make -jN              Parallel host build; no cross-builds or VMs' \
		'make prod-linux-x86_64 Self-contained Linux x86-64 via pinned Nix; network/cache on first build' \
		'make prod-linux-aarch64 Self-contained Linux ARM64 via pinned Nix' \
		'make prod-macos-arm64  macOS ARM64 with static application libraries via pinned Nix' \
		'make prod-macos-x86_64 macOS Intel with static application libraries via pinned Nix' \
		'make prod-macos-universal Native ARM64+Intel Mach-O file and matching dSYM' \
		'make install          Build/install production by default' \
		'make DEBUG=1 install  Deliberately build/install debug instead' \
		'make check            Unit, CLI, terminal (if tmux exists), source/dependency checks' \
		'make sanitizercheck   Clean and run unstripped ASan/UBSan checks' \
		'make tmuxcheck        Local fixture and production IRC terminal tests; needs tmux' \
		'make releasecheck     GCC/default, available Clang, and sanitizer checks' \
		'make stylecheck depscheck portabilitycheck depclosurecheck sizecheck' \
		'                      Focused source, portability, dependency and size checks' \
		'make clean            Remove local build/test outputs and debug symbols' \
		'make help             Show this help only; no build, tests or network' \
		'' \
		'Overrides: DEBUG=0|1 CC=... CPPFLAGS=... CFLAGS=... LDFLAGS=...' \
		'           PREFIX=/usr/local DESTDIR=... STRIP=... OBJCOPY=... DSYMUTIL=...' \
		'Explicit compiler flags replace profile defaults; thread flags stay required.' \
		'Production symbols: $(DEBUG_SYMBOLS) beside the binary; not needed to run it.' \
		'Keep matching production symbols for crashes; DEBUG=1 is a different build.' \
		'Live targets (livecheck, terminallivecheck, releaseevidence) use network/' \
		'credentials and may incur provider charges; never part of make or help.'

prod-macos-universal: prod-macos-arm64 prod-macos-x86_64

prod-linux-x86_64 prod-linux-aarch64 prod-macos-arm64 prod-macos-x86_64 prod-macos-universal:
	@test '$(DEBUG)' = 0 || { printf '%s\n' '$@: production only; use DEBUG=0' >&2; exit 2; }
	@mkdir -p build/matrix
	nix-build nix/portable.nix -A $(patsubst prod-%,%,$@) \
		--argstr buildVersion '$(BUILD_VERSION)' --argstr buildRevision '$(GIT_HEAD)' \
		--max-jobs 1 --cores 1 --out-link build/matrix/$(patsubst prod-%,%,$@)

install: $(BIN) $(BIN).1
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	chmod 0755 $(DESTDIR)$(PREFIX)/bin/$(BIN)
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	cp $(BIN).1 $(DESTDIR)$(MANPREFIX)/man1/$(BIN).1
	chmod 0644 $(DESTDIR)$(MANPREFIX)/man1/$(BIN).1

FORCE:

.PHONY: all check stylecheck depscheck portabilitycheck depclosurecheck evidencetoolcheck evidencematrixcheck sanitizercheck releasecheck livecheck tmuxcheck terminallivecheck evidencebundle evidencecheck releaseevidence sizecheck clean install help prod-linux-x86_64 prod-linux-aarch64 prod-macos-arm64 prod-macos-x86_64 prod-macos-universal FORCE

-include $(COMMON_OBJ:.o=.d) src/main.d
