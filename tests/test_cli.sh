#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
bin=$1
case "$bin" in /*) ;; *) bin=$(pwd)/$bin ;; esac
root=$(mktemp -d /tmp/snajpagent-cli-XXXXXX)
cleanup() {
    rm -rf "$root"
}
trap cleanup EXIT
trap 'cleanup; exit 143' HUP INT TERM
mkdir -m 700 "$root/state" "$root/work"
export XDG_STATE_HOME="$root/state"
mkdir -m 700 "$root/config"
export XDG_CONFIG_HOME="$root/config"
cd "$root/work"

set +e
LC_ALL=C $bin -l >"$root/locale.out" 2>"$root/locale.err"
status=$?
set -e
[ "$status" -eq 2 ]
grep -q 'UTF-8 locale is required' "$root/locale.err"
export LC_ALL=C.utf8

out=$($bin -e -- ping 2>"$root/err")
[ "$out" = pong ]
[ ! -s "$root/err" ]
id=$(find "$root/state/snajpagent/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
[ ${#id} -eq 32 ]
[ "$(wc -l < "$root/state/snajpagent/sessions/$id/events.jsonl")" -eq 5 ]

out=$($bin -e -r "$id" -- ping 2>"$root/err")
[ "$out" = pong ]
[ ! -s "$root/err" ]
[ "$(wc -l < "$root/state/snajpagent/sessions/$id/events.jsonl")" -eq 9 ]
$bin -l >"$root/list" 2>"$root/err"
grep -q "^$(printf %.8s "$id").*2" "$root/list"

set +e
$bin -e -- empty >"$root/empty.out" 2>"$root/empty.err"
status=$?
set -e
[ "$status" -eq 4 ]
[ ! -s "$root/empty.out" ]
grep -q 'provider completed without a final answer' "$root/empty.err"

$bin -e -vvvv -- repeat >"$root/repeat.out" 2>"$root/repeat.err"
[ "$(cat "$root/repeat.out")" = haha ]
grep -q 'event .* turn_completed synced' "$root/repeat.err"

$bin -e -- utf8 >"$root/utf8.out" 2>"$root/utf8.err"
[ "$(cat "$root/utf8.out")" = "€" ]
[ ! -s "$root/utf8.err" ]

set +e
$bin -e -- crash >"$root/crash.out" 2>"$root/crash.err"
crash_status=$?
set -e
[ "$crash_status" -eq 99 ]
[ ! -s "$root/crash.out" ]
crash_id=$(grep -rl '"text":"crash"' "$root/state/snajpagent/sessions" | sed 's|/events.jsonl$||;s|.*/||')
$bin -e -r "$crash_id" -- ping >"$root/crash-recovered.out" 2>"$root/crash-recovered.err"
[ "$(cat "$root/crash-recovered.out")" = pong ]
grep -q 'recovered an interrupted turn' "$root/crash-recovered.err"

$bin -e -- provider_fail >"$root/fail.out" 2>"$root/fail.err" && exit 1
[ ! -s "$root/fail.out" ]
grep -q 'fixture provider failed' "$root/fail.err"
fail_id=$(grep -rl 'fixture provider failed' "$root/state/snajpagent/sessions" | sed 's|/events.jsonl$||;s|.*/||')
$bin -e -r "$fail_id" -- ping >"$root/recovered.out" 2>"$root/recovered.err"
[ "$(cat "$root/recovered.out")" = pong ]
[ ! -s "$root/recovered.err" ]

mkdir -m 700 "$root/work2"
out=$($bin -e -r -C "$root/work2" "$id" -- ping 2>"$root/relocate.err")
[ "$out" = pong ]
[ ! -s "$root/relocate.err" ]
python3 - "$root/state/snajpagent/sessions/$id/events.jsonl" "$root/work2" <<'PY'
import json
import sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
relocations = [event for event in events if event["type"] == "workspace_changed"]
assert len(relocations) == 1
assert relocations[0]["data"]["new_workspace"] == sys.argv[2]
turns = [event for event in events if event["type"] == "turn_started"]
assert turns[-1]["data"]["workspace"] == sys.argv[2]
PY
(cd "$root/work2" && XDG_STATE_HOME="$root/state" $bin -l >"$root/list2" 2>"$root/list2.err")
grep -q "^$(printf %.8s "$id")" "$root/list2"

out=$($bin -e -- tool_only 2>"$root/tool.err")
[ "$out" = "tool complete" ]
[ ! -s "$root/tool.err" ]

after=$($bin -e -- text_tool 2>"$root/text-tool.err")
[ "$after" = done ]
grep -q '^Checking first\.$' "$root/text-tool.err"

out=$($bin -e -v -- multi_item 2>"$root/multi.err")
[ "$out" = "Done." ]
grep -q '^Working\.$' "$root/multi.err"
grep -q '^reason › Checked the fixture\.$' "$root/multi.err"

set +e
$bin -e -- final_plus_call >"$root/conflict.out" 2>"$root/conflict.err"
status=$?
set -e
[ "$status" -eq 4 ]
[ ! -s "$root/conflict.out" ]
grep -q 'terminal answer with tool calls' "$root/conflict.err"
conflict_log=$(grep -rl 'protocol_conflict' "$root/state/snajpagent/sessions" | head -n 1)
grep -q '"status":"not_run"' "$conflict_log"
! grep -q '"type":"tool_started"' "$conflict_log"

set +e
$bin -e -- tool_crash >"$root/tool-crash.out" 2>"$root/tool-crash.err"
status=$?
set -e
[ "$status" -eq 98 ]
tool_crash_id=$(grep -rl '"text":"tool_crash"' "$root/state/snajpagent/sessions" | sed 's|/events.jsonl$||;s|.*/||')
out=$($bin -e -r "$tool_crash_id" -- ping 2>"$root/tool-recovery.err")
[ "$out" = pong ]
grep -q 'unfinished tool work' "$root/tool-recovery.err"
grep -q '"status":"outcome_unknown"' "$root/state/snajpagent/sessions/$tool_crash_id/events.jsonl"


# Configuration is strict, additive, and applied before any state mutation.
cat >"$root/config.ini" <<'EOF'
[agent]
model = default
reasoning_effort = high
[ui]
verbosity = 3
resume_history_turns = 0
[tool]
shell = /bin/sh
secret_env = EXTRA_TOKEN
EOF
out=$($bin -c "$root/config.ini" -e -v -- ping 2>"$root/config.err")
[ "$out" = pong ]
grep -q '^turn › .* effort=high ' "$root/config.err"
grep -q '^event › .* turn_completed synced$' "$root/config.err"
cat >"$root/bad-config.ini" <<'EOF'
[ui]
verbosity = 1
verbosity = 2
EOF
before=$(find "$root/state/snajpagent/sessions" -mindepth 1 -maxdepth 1 -type d | wc -l)
set +e
$bin -c "$root/bad-config.ini" -e -- ping >"$root/bad-config.out" 2>"$root/bad-config.err"
status=$?
set -e
[ "$status" -eq 2 ]
[ ! -s "$root/bad-config.out" ]
grep -q 'invalid configuration at line 3' "$root/bad-config.err"
after=$(find "$root/state/snajpagent/sessions" -mindepth 1 -maxdepth 1 -type d | wc -l)
[ "$before" -eq "$after" ]

# Resume command-line settings are consumed by one admitted turn only.
override_state="$root/override-state"
mkdir -m 700 "$override_state"
XDG_STATE_HOME="$override_state" $bin -e -- ping >/dev/null 2>"$root/override.err"
override_id=$(find "$override_state/snajpagent/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
XDG_STATE_HOME="$override_state" $bin -e -o low -r "$override_id" -- ping >/dev/null 2>"$root/override.err"
XDG_STATE_HOME="$override_state" $bin -e -r "$override_id" -- ping >/dev/null 2>"$root/override.err"
python3 - "$override_state/snajpagent/sessions/$override_id/events.jsonl" <<'PY'
import json
import sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
turns = [event["data"]["config"] for event in events
         if event["type"] == "turn_started"]
assert [turn["effort"] for turn in turns] == ["medium", "low", "medium"]
assert not any(event["type"] == "effort_changed"
               for event in events)
PY

# Automatic compaction is threshold-gated and durable.
auto_state="$root/auto-compact-state"
mkdir -m 700 "$auto_state"
cat >"$root/auto-compact.ini" <<'EOF'
[provider]
auto_compact_input_tokens = 1
EOF
XDG_STATE_HOME="$auto_state" $bin -c "$root/auto-compact.ini" -e -vvvv -- ping >"$root/auto-compact.out" 2>"$root/auto-compact.err"
[ "$(cat "$root/auto-compact.out")" = pong ]
auto_id=$(find "$auto_state/snajpagent/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
python3 - "$auto_state/snajpagent/sessions/$auto_id/events.jsonl" <<'PY'
import json
import sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
started = [event for event in events if event["type"] == "compaction_started"]
completed = [event for event in events if event["type"] == "compaction_completed"]
assert len(started) == 1 and len(completed) == 1
assert started[0]["data"]["reason"] == "automatic"
assert started[0]["data"]["count_method"] == "qualified_upper_bound"
assert started[0]["data"]["count_request_sha256"]
assert completed[0]["data"]["count_method"] == "qualified_upper_bound"
assert completed[0]["data"]["output_count_method"] == "qualified_upper_bound"
assert completed[0]["data"]["output_count_request_sha256"]
PY

# When the compact endpoint is disabled, compaction still uses Responses.
responses_compact_state="$root/responses-compact-state"
mkdir -m 700 "$responses_compact_state"
cat >"$root/responses-compact.ini" <<'EOF'
[provider]
auto_compact_input_tokens = 1
native_compaction = false
EOF
XDG_STATE_HOME="$responses_compact_state" $bin -c "$root/responses-compact.ini" -e -vvvv -- ping >"$root/responses-compact.out" 2>"$root/responses-compact.err"
[ "$(cat "$root/responses-compact.out")" = pong ]
responses_compact_id=$(find "$responses_compact_state/snajpagent/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
python3 - "$responses_compact_state/snajpagent/sessions/$responses_compact_id/events.jsonl" <<'PY'
import json
import sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
started = [event for event in events if event["type"] == "compaction_started"]
completed = [event for event in events if event["type"] == "compaction_completed"]
assert len(started) == 1 and len(completed) == 1
assert completed[0]["data"]["output"][0]["type"] == "message"
assert completed[0]["data"]["output"][0]["role"] == "developer"
assert completed[0]["data"]["output"][0]["content"] == "fixture responses compact summary"
PY

# Automatic pre-response compaction can compact existing history before a
# resumed turn's response, then post-turn compaction can compact that new turn.
pre_state="$root/pre-response-compact-state"
mkdir -m 700 "$pre_state"
XDG_STATE_HOME="$pre_state" $bin -e -- ping >"$root/pre-first.out" 2>"$root/pre-first.err"
[ "$(cat "$root/pre-first.out")" = pong ]
pre_id=$(find "$pre_state/snajpagent/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
XDG_STATE_HOME="$pre_state" $bin -c "$root/auto-compact.ini" -e -r "$pre_id" -- ping >"$root/pre-second.out" 2>"$root/pre-second.err"
[ "$(cat "$root/pre-second.out")" = pong ]
python3 - "$pre_state/snajpagent/sessions/$pre_id/events.jsonl" <<'PY'
import json
import sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
turns = [event for event in events if event["type"] == "turn_started"]
assert len(turns) == 2
turn2 = turns[1]
responses2 = [event for event in events
              if event["type"] == "response_started"
              and event["data"]["turn_id"] == turn2["data"]["turn_id"]]
assert len(responses2) == 1
started = [event for event in events if event["type"] == "compaction_started"]
completed = [event for event in events if event["type"] == "compaction_completed"]
assert len(started) == 2 and len(completed) == 2
assert turn2["seq"] < started[0]["seq"] < completed[0]["seq"] < responses2[0]["seq"]
assert started[0]["data"]["source_seq"] == turn2["seq"] - 1
assert started[0]["data"]["reason"] == "automatic"
assert completed[0]["data"]["output_count_method"] == "qualified_upper_bound"
assert responses2[0]["data"]["compact_id"] == started[0]["data"]["compact_id"]
assert responses2[0]["data"]["profile_id"]
assert responses2[0]["data"]["capability_version"]
assert responses2[0]["data"]["count_request_sha256"]
assert started[1]["seq"] > responses2[0]["seq"]
PY

TERM=xterm "$(dirname "$bin")/pty_interactive.py" "$bin" "$root/work"
TERM=dumb "$(dirname "$bin")/pty_interactive.py" "$bin" "$root/work"
TERM=xterm "$(dirname "$bin")/pty_resize.py" "$bin" "$root/work"
TERM=xterm "$(dirname "$bin")/pty_suspend.py" "$bin" "$root/work"
TERM=xterm python3 "$(dirname "$bin")/pty_terminal_matrix.py" "$bin" "$root/work"
TERM=xterm "$(dirname "$bin")/pty_active.py" "$bin" "$root/work"
# Give PTY child teardown a short settle window before the EXIT cleanup removes
# the shared temporary state/workspace tree.
sleep 0.1
echo 'test_cli: ok'
