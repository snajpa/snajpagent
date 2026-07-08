# SPDX-License-Identifier: GPL-2.0-only
.POSIX:

include config.mk

BIN = snajpagent
EVIDENCE_DIR ?= build/release-evidence/current-host
RELEASE_PLATFORMS ?= linux-x86_64 linux-aarch64 macos-x86_64 macos-arm64
RELEASE_EVIDENCE_DIRS ?=
COMMON_SRC = src/base.c src/config.c src/credential.c src/secret.c src/instructions.c src/json.c src/wire.c src/context.c src/provider_retry.c src/provider.c src/tools.c src/sse.c src/responses.c src/turn.c src/store.c src/store_lookup.c src/store_lifecycle.c src/tools_patch.c src/term.c src/render.c src/cli.c src/app_events.c src/app_stream.c src/app_process.c src/app_lifecycle.c src/app_compact.c src/app_provider.c src/app.c
COMMON_OBJ = $(COMMON_SRC:.c=.o)
HEADERS = src/snajpagent.h src/base.h src/config.h src/credential.h src/secret.h src/instructions.h src/json.h src/snj_jansson.h src/snj_jansson_abi.h src/wire.h src/context.h src/provider_retry.h src/provider.h src/tools.h src/tools_patch.h src/sse.h src/responses.h src/turn.h src/store.h src/store_internal.h src/term.h src/render.h src/cli.h src/app.h src/app_internal.h
DEPFLAGS = -MMD -MP

all: $(BIN)

.c.o:
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) $(DEPFLAGS) -Isrc -c $< -o $@

$(BIN): $(COMMON_OBJ) src/main.o
	$(CC) $(LDFLAGS) -o $@ $(COMMON_OBJ) src/main.o $(LDLIBS) $(CURL_LIBS)

tests/snajpagent-fixture: $(COMMON_SRC) src/main.c tests/fixture_provider.c $(HEADERS)
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

tests/test_base: src/base.c tests/test_base.c src/base.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ src/base.c tests/test_base.c

tests/test_config: src/base.c src/config.c tests/test_config.c src/base.h src/config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ src/base.c src/config.c tests/test_config.c

tests/test_credential: src/credential.c tests/test_credential.c src/credential.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ src/credential.c tests/test_credential.c

tests/test_instructions: src/base.c src/json.c src/instructions.c tests/test_instructions.c src/base.h src/json.h src/instructions.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/json.c src/instructions.c tests/test_instructions.c $(LDLIBS)

tests/test_sse: src/base.c src/sse.c tests/test_sse.c src/base.h src/sse.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ src/base.c src/sse.c tests/test_sse.c

tests/test_json: src/base.c src/json.c tests/test_json.c src/base.h src/json.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/json.c tests/test_json.c $(LDLIBS)

tests/test_wire: src/base.c src/json.c src/wire.c tests/test_wire.c src/base.h src/json.h src/snj_jansson.h src/snj_jansson_abi.h src/wire.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/json.c src/wire.c tests/test_wire.c $(LDLIBS)

tests/test_responses: src/base.c src/json.c src/sse.c src/responses.c src/turn.c \
		tests/test_responses.c src/base.h src/json.h src/sse.h src/responses.h src/turn.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/json.c src/sse.c src/responses.c src/turn.c \
		tests/test_responses.c $(LDLIBS)

tests/test_provider_retry: src/provider_retry.c tests/test_provider_retry.c src/provider_retry.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ src/provider_retry.c tests/test_provider_retry.c

tests/test_provider_transport: $(COMMON_SRC) tests/test_provider_transport.c $(HEADERS)
	$(CC) $(CPPFLAGS) -DSNAJPAGENT_TEST_TRANSPORT_ENDPOINTS=1 $(JANSSON_CFLAGS) $(CURL_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ $(COMMON_SRC) tests/test_provider_transport.c $(LDLIBS) $(CURL_LIBS)

tests/test_context: src/base.c src/json.c src/instructions.c src/context.c src/turn.c src/store.c tests/test_context.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/json.c src/instructions.c src/context.c src/turn.c src/store.c \
		tests/test_context.c $(LDLIBS)

tests/test_render: src/base.c src/json.c src/term.c src/render.c tests/test_render.c \
		src/base.h src/json.h src/term.h src/render.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/json.c src/term.c src/render.c tests/test_render.c $(LDLIBS)

tests/test_turn: src/base.c src/json.c src/turn.c tests/test_turn.c src/base.h src/json.h src/turn.h
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/json.c src/turn.c tests/test_turn.c $(LDLIBS)


tests/test_tools: src/base.c src/json.c src/credential.c src/secret.c src/config.c src/turn.c src/tools.c src/tools_patch.c tests/test_tools.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) -O0 $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/json.c src/credential.c src/secret.c \
		src/config.c src/turn.c src/tools.c src/tools_patch.c tests/test_tools.c $(LDLIBS)

tests/test_store: CPPFLAGS += -DSNAJPAGENT_TEST_FIXTURE=1
tests/test_store: src/base.c src/json.c src/instructions.c src/turn.c src/store.c src/store_lookup.c src/store_lifecycle.c tests/test_store.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(JANSSON_CFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc \
		-o $@ src/base.c src/json.c src/instructions.c src/turn.c src/store.c src/store_lookup.c src/store_lifecycle.c tests/test_store.c $(LDLIBS)

check: tests/test_base tests/test_config tests/test_instructions tests/test_credential tests/test_sse tests/test_json tests/test_wire tests/test_responses tests/test_provider_retry tests/test_provider_transport tests/test_context tests/test_render tests/test_turn tests/test_tools tests/test_store tests/snajpagent-fixture
	./tests/test_base
	./tests/test_config
	./tests/test_instructions
	./tests/test_credential
	./tests/test_sse
	./tests/test_json
	./tests/test_wire
	./tests/test_responses
	./tests/test_provider_retry
	./tests/test_provider_transport
	./tests/test_context
	./tests/test_render
	./tests/test_turn
	./tests/test_tools
	./tests/test_store
	./tests/test_cli.sh ./tests/snajpagent-fixture
	./tools/check_spdx.sh
	$(MAKE) depscheck
	$(MAKE) portabilitycheck
	$(MAKE) depclosurecheck
	$(MAKE) evidencetoolcheck
	$(MAKE) statuscheck
	$(MAKE) sizecheck

statuscheck:
	python3 ./tools/check_status.py

depscheck:
	python3 ./tools/check_deps.py

portabilitycheck:
	python3 ./tools/check_portability.py

depclosurecheck: $(BIN)
	python3 ./tools/check_dependency_closure.py ./$(BIN)

sanitizercheck:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
		$(MAKE) check CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

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

evidencebundle: $(BIN) tests/snajpagent-fixture
	rm -rf $(EVIDENCE_DIR)
	python3 ./tools/collect_release_evidence.py ./$(BIN) $(EVIDENCE_DIR) --fixture ./tests/snajpagent-fixture --skip-live

evidencecheck:
	python3 ./tools/check_release_evidence.py $(EVIDENCE_DIR)

evidencetoolcheck:
	python3 ./tools/check_release_evidence.py --self-test
	python3 ./tools/check_release_matrix.py --self-test

evidencematrixcheck:
	test -n "$(RELEASE_EVIDENCE_DIRS)" || { printf '%s\n' 'evidencematrixcheck: set RELEASE_EVIDENCE_DIRS to per-platform evidence directories'; exit 2; }
	set --; for p in $(RELEASE_PLATFORMS); do set -- "$$@" --require-platform "$$p"; done; \
		python3 ./tools/check_release_matrix.py --require-terminal --require-live "$$@" $(RELEASE_EVIDENCE_DIRS)

releaseevidence: $(BIN) tests/snajpagent-fixture
	rm -rf $(EVIDENCE_DIR)
	python3 ./tools/collect_release_evidence.py ./$(BIN) $(EVIDENCE_DIR) --fixture ./tests/snajpagent-fixture --require-live
	python3 ./tools/check_release_evidence.py $(EVIDENCE_DIR) --require-terminal --require-live

sizecheck:
	@prod=$$(find src -type f \( -name '*.c' -o -name '*.h' \) -print0 | \
		sort -z | xargs -0 cat | wc -l | tr -d ' '); \
	all=$$(find src tests -type f \( -name '*.c' -o -name '*.h' \) -print0 | \
		sort -z | xargs -0 cat | wc -l | tr -d ' '); \
	units=$$(find src -type f -name '*.c' | wc -l | tr -d ' '); \
	largest=$$(find src -type f \( -name '*.c' -o -name '*.h' \) -exec wc -l {} + | \
		awk '$$2 != "total" && $$1 > max { max = $$1; file = $$2 } END { if (file == "") print "0 -"; else print max " " file }'); \
	largest_lines=$$(printf '%s\n' "$$largest" | awk '{ print $$1 }'); \
	largest_file=$$(printf '%s\n' "$$largest" | cut -d ' ' -f 2-); \
	printf 'production C/header lines: %s / 30000 preferred / 35000 hard\n' "$$prod"; \
	printf 'all shipped C/header lines: %s / 40000 preferred / 50000 hard\n' "$$all"; \
	printf 'production translation units: %s / 30 hard\n' "$$units"; \
	printf 'largest production C/header file: %s lines %s / 2000 review trigger\n' "$$largest_lines" "$$largest_file"; \
	if [ "$$largest_lines" -gt 2000 ]; then \
		printf 'line-budget review: %s exceeds the 2000-line simplicity-review trigger\n' "$$largest_file"; \
	fi; \
	test "$$prod" -le 35000 && test "$$all" -le 50000 && test "$$units" -le 30

clean:
	rm -f $(BIN) src/*.o src/*.d tests/test_base tests/test_config tests/test_instructions tests/test_credential tests/test_sse tests/test_json tests/test_wire tests/test_responses tests/test_provider_retry tests/test_provider_transport tests/test_context tests/test_render tests/test_turn tests/test_tools tests/test_store tests/snajpagent-fixture
	rm -rf tests/.fixture-obj build

install: $(BIN)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	chmod 0755 $(DESTDIR)$(PREFIX)/bin/$(BIN)

.PHONY: all check statuscheck depscheck portabilitycheck depclosurecheck evidencetoolcheck evidencematrixcheck sanitizercheck releasecheck livecheck evidencebundle evidencecheck releaseevidence sizecheck clean install

-include $(COMMON_OBJ:.o=.d) src/main.d
