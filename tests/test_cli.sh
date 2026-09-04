#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
bin=$1
: "${SNAJPAGENT_TEST_NAME:?missing product name}"
: "${SNAJPAGENT_TEST_VERSION:?missing product version}"
case "$bin" in /*) ;; *) bin=$(pwd)/$bin ;; esac
bin=$(cd "$(dirname "$bin")" && pwd -P)/$(basename "$bin")
root=$(mktemp -d "${TMPDIR:-/tmp}/snajpagent-cli-XXXXXX")
cleanup() {
    rm -rf "$root"
}
trap cleanup EXIT
trap 'cleanup; exit 143' HUP INT TERM
mkdir -m 700 "$root/home" "$root/work" "$root/config"
export HOME="$root/home"
unset CODEX_HOME
unset OPENAI_API_KEY
dotdir="$HOME/.$SNAJPAGENT_TEST_NAME"
export SNAJPAGENT_DOTDIR="$dotdir"
export SNAJPAGENT_TEST_ROOT="$root"
cd "$root/work"

resume_count() {
    grep -c '^resume:$' "$1" || true
}

only_resume() {
    [ "$(resume_count "$1")" -eq 1 ]
    [ "$(wc -l < "$1")" -eq 2 ]
    [ "$(sed -n '1p' "$1")" = 'resume:' ]
    resume_command_line=$(sed -n '2p' "$1")
    [ -n "$resume_command_line" ]
    case "$resume_command_line" in
        ' '*|"	"*) return 1 ;;
    esac
    [ ! -s "$1.without-resume" ] || return 1
}

strip_resume() {
    awk 'skip { skip = 0; next } /^resume:$/ { skip = 1; next } { print }' \
        "$1" >"$1.without-resume"
}

set +e
LC_ALL=C $bin -l >"$root/locale.out" 2>"$root/locale.err"
status=$?
set -e
[ "$status" -eq 2 ]
grep -q 'UTF-8 locale is required' "$root/locale.err"
export LC_ALL=C.utf8

version=$($bin -V)
[ "$version" = "$SNAJPAGENT_TEST_NAME $SNAJPAGENT_TEST_VERSION" ]

$bin -h >"$root/help" 2>"$root/help.err"
[ ! -s "$root/help.err" ]
grep -q -- '-d, --daemon' "$root/help"
grep -q -- '-c, --client\[=ENDPOINT\]' "$root/help"
grep -q -- '--model-nick NICK' "$root/help"
grep -q -- '--operator-nick NICK' "$root/help"
! grep -q -- '--operator-name' "$root/help"
grep -q -- '--color\[=WHEN\]' "$root/help"
grep -q -- '--markdown' "$root/help"
grep -q -- '--no-markdown' "$root/help"
grep -q "^usage: $SNAJPAGENT_TEST_NAME " "$root/help"

for args in \
    '-d' \
    '-r room -n worker' \
    '-d -n worker -o WORKER' \
    '-c localhost -c localhost:6667 -n worker' \
    '-d -n worker initial'; do
    set +e
    # These arguments contain no quoting-sensitive values.
    $bin $args >"$root/network-invalid.out" 2>"$root/network-invalid.err"
    status=$?
    set -e
    [ "$status" -eq 2 ]
    [ ! -s "$root/network-invalid.out" ]
done
grep -q 'networked initial chat text must follow --' \
    "$root/network-invalid.err"

set +e
$bin -e --model-nick=worker --operator-nick alice -- ping \
    >"$root/network-long.out" 2>"$root/network-long.err"
status=$?
set -e
[ "$status" -eq 2 ]
grep -q -- '-e cannot be combined with network options' "$root/network-long.err"

for option in --name --operator-name; do
    set +e
    $bin "$option" stale -l >"$root/old-nick-option.out" \
        2>"$root/old-nick-option.err"
    status=$?
    set -e
    [ "$status" -eq 2 ]
    grep -q "unknown option $option" "$root/old-nick-option.err"
done

cat >"$root/network-config.ini" <<'EOF'
[irc]
daemon = true
model_nick = worker
EOF
set +e
$bin --config "$root/network-config.ini" initial >"$root/network-config.out" \
    2>"$root/network-config.err"
status=$?
set -e
[ "$status" -eq 2 ]
grep -q 'networked initial chat text must follow --' \
    "$root/network-config.err"

cat >"$root/listen-only.ini" <<'EOF'
[irc]
listen = localhost:6667
EOF
set +e
$bin --config "$root/listen-only.ini" >"$root/listen-only.out" \
    2>"$root/listen-only.err"
status=$?
set -e
[ "$status" -eq 2 ]
grep -q 'require a server or client role' "$root/listen-only.err"

cat >"$root/color-network-error.ini" <<'EOF'
[ui]
color = always
[irc]
daemon = true
EOF
set +e
$bin --config "$root/color-network-error.ini" \
    >"$root/color-network-error.out" 2>"$root/color-network-error.err"
status=$?
set -e
[ "$status" -eq 2 ]
LC_ALL=C grep -q "$(printf '\033')" "$root/color-network-error.err"

set +e
$bin --no-color -d >"$root/no-color-error.out" 2>"$root/no-color-error.err"
status=$?
set -e
[ "$status" -eq 2 ]
! LC_ALL=C grep -q "$(printf '\033')" "$root/no-color-error.err"

set +e
$bin --color -d >"$root/color-error.out" 2>"$root/color-error.err"
status=$?
set -e
[ "$status" -eq 2 ]
LC_ALL=C grep -q "$(printf '\033')" "$root/color-error.err"

set +e
$bin --color=rainbow >"$root/bad-color.out" 2>"$root/bad-color.err"
status=$?
set -e
[ "$status" -eq 2 ]
grep -q 'accepts auto, always, or never' "$root/bad-color.err"

for args in '--markdown --markdown' '--no-markdown --no-markdown' \
            '--markdown --no-markdown'; do
    set +e
    # These arguments contain no quoting-sensitive values.
    $bin $args -l >"$root/bad-markdown.out" 2>"$root/bad-markdown.err"
    status=$?
    set -e
    [ "$status" -eq 2 ]
    grep -q 'duplicate --.*markdown option' "$root/bad-markdown.err"
done

for option in --client= --listen=; do
    set +e
    $bin "$option" -d -n worker >"$root/empty-endpoint.out" \
        2>"$root/empty-endpoint.err"
    status=$?
    set -e
    [ "$status" -eq 2 ]
    grep -q 'requires a nonempty endpoint' "$root/empty-endpoint.err"
done

stdin_dotdir="$root/stdin-state"
out=$(printf 'ping\n' | $bin --dotdir "$stdin_dotdir" -e \
    2>"$root/stdin.err")
[ "$out" = pong ]
strip_resume "$root/stdin.err"
only_resume "$root/stdin.err"
stdin_log=$(find "$stdin_dotdir/sessions" -name events.jsonl -print)
python3 - "$stdin_log" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
started = [event for event in events if event["type"] == "turn_started"]
assert len(started) == 1
assert started[0]["data"]["text"] == "ping"
PY

goal_dotdir="$root/model-goal-state"
out=$($bin --dotdir "$goal_dotdir" -e -- \
    'please create a persistent goal' 2>"$root/model-goal.err")
[ "$out" = 'model-created checkpointgoal done' ]
goal_log=$(find "$goal_dotdir/sessions" -name events.jsonl -print)
python3 - "$goal_log" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
started = [event for event in events if event["type"] == "goal_started"]
turns = [event for event in events if event["type"] == "turn_started"]
completed = [event for event in events if event["type"] == "goal_completed"]
assert len(started) == 1
assert started[0]["data"]["prompt"] == "model-created goal"
assert [event["data"]["input_kind"] for event in turns] == ["direct", "goal"]
assert len(completed) == 1 and completed[0]["data"]["actor"] == "model"
PY

set +e
$bin -e </dev/null >"$root/empty-stdin.out" 2>"$root/empty-stdin.err"
status=$?
set -e
[ "$status" -eq 2 ]
[ ! -s "$root/empty-stdin.out" ]
grep -q 'stdin prompt is empty' "$root/empty-stdin.err"

# The established explicit argument form remains supported alongside stdin.
out=$($bin -e -- ping 2>"$root/err")
[ "$out" = pong ]
strip_resume "$root/err"
only_resume "$root/err"
grep -q "^'$bin' --dotdir '$dotdir' --resume '[0-9a-f]\\{32\\}'$" \
    "$root/err"
[ -d "$dotdir/sessions" ]
[ -d "$dotdir/trash" ]
id=$(find "$dotdir/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
[ ${#id} -eq 32 ]
[ "$(wc -l < "$dotdir/sessions/$id/events.jsonl")" -eq 5 ]

# The writer owns exactly "resume:\nCOMMAND\n"; the command starts at column
# zero, dynamic arguments are POSIX-shell safe,
# prompts and credentials are absent, and the printed command really resumes.
quoted_dotdir="$root/quoted ' state"
quoted_config="$root/config/quoted ' config.ini"
mkdir -m 700 "$quoted_dotdir"
cat >"$quoted_config" <<'EOF'
[provider]
api_key_env = RESUME_COMMAND_SECRET
EOF
export RESUME_COMMAND_SECRET='must-not-appear-in-the-resume-command'
out=$($bin --dotdir "$quoted_dotdir" --config "$quoted_config" \
    --color=never --markdown -e -- ping 2>"$root/quoted.err")
[ "$out" = pong ]
[ "$(resume_count "$root/quoted.err")" -eq 1 ]
! grep -q 'must-not-appear\| -- ping\| -e ' "$root/quoted.err"
grep -Fq "'\\''" "$root/quoted.err"
resume_command=$(sed -n '/^resume:$/ { n; p; }' "$root/quoted.err")
resume_prefix=${resume_command% --resume *}
resume_id=${resume_command##* --resume }
eval "$resume_prefix -e --resume $resume_id -- ping" \
    >"$root/quoted-resumed.out" 2>"$root/quoted-resumed.err"
[ "$(cat "$root/quoted-resumed.out")" = pong ]
[ "$(resume_count "$root/quoted-resumed.err")" -eq 1 ]
unset RESUME_COMMAND_SECRET

out=$($bin -e --resume "$id" -- ping 2>"$root/err")
[ "$out" = pong ]
strip_resume "$root/err"
only_resume "$root/err"
[ "$(wc -l < "$dotdir/sessions/$id/events.jsonl")" -eq 9 ]
$bin -l >"$root/list" 2>"$root/err"
grep -q "^$(printf %.8s "$id").*2" "$root/list"

set +e
$bin -e -- empty >"$root/empty.out" 2>"$root/empty.err"
status=$?
set -e
[ "$status" -eq 4 ]
[ ! -s "$root/empty.out" ]
grep -q 'provider completed without a final answer' "$root/empty.err"
[ "$(resume_count "$root/empty.err")" -eq 1 ]

$bin -e -vvvv -- repeat >"$root/repeat.out" 2>"$root/repeat.err"
[ "$(cat "$root/repeat.out")" = haha ]
grep -q 'event .* turn_completed synced' "$root/repeat.err"

$bin -e -- utf8 >"$root/utf8.out" 2>"$root/utf8.err"
[ "$(cat "$root/utf8.out")" = "€" ]
strip_resume "$root/utf8.err"
only_resume "$root/utf8.err"

set +e
$bin -e -- crash >"$root/crash.out" 2>"$root/crash.err"
crash_status=$?
set -e
[ "$crash_status" -eq 99 ]
[ ! -s "$root/crash.out" ]
crash_id=$(grep -rl '"text":"crash"' "$dotdir/sessions" | sed 's|/events.jsonl$||;s|.*/||')
$bin -e --resume "$crash_id" -- ping >"$root/crash-recovered.out" 2>"$root/crash-recovered.err"
[ "$(cat "$root/crash-recovered.out")" = pong ]
grep -q 'recovered an interrupted turn' "$root/crash-recovered.err"

$bin -e -- provider_fail >"$root/fail.out" 2>"$root/fail.err" && exit 1
[ ! -s "$root/fail.out" ]
grep -q 'fixture provider failed' "$root/fail.err"
[ "$(resume_count "$root/fail.err")" -eq 1 ]
fail_id=$(grep -rl 'fixture provider failed' "$dotdir/sessions" | sed 's|/events.jsonl$||;s|.*/||')
$bin -e --resume "$fail_id" -- ping >"$root/recovered.out" 2>"$root/recovered.err"
[ "$(cat "$root/recovered.out")" = pong ]
strip_resume "$root/recovered.err"
only_resume "$root/recovered.err"

mkdir -m 700 "$root/work2"
out=$($bin -e --resume -C "$root/work2" "$id" -- ping 2>"$root/relocate.err")
[ "$out" = pong ]
strip_resume "$root/relocate.err"
only_resume "$root/relocate.err"
python3 - "$dotdir/sessions/$id/events.jsonl" "$root/work2" <<'PY'
import json
import sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
relocations = [event for event in events if event["type"] == "workspace_changed"]
assert len(relocations) == 1
assert relocations[0]["data"]["new_workspace"] == sys.argv[2]
turns = [event for event in events if event["type"] == "turn_started"]
assert turns[-1]["data"]["workspace"] == sys.argv[2]
PY
(cd "$root/work2" && $bin -l >"$root/list2" 2>"$root/list2.err")
grep -q "^$(printf %.8s "$id")" "$root/list2"

out=$($bin -e -- tool_only 2>"$root/tool.err")
[ "$out" = "tool complete" ]
strip_resume "$root/tool.err"
only_resume "$root/tool.err"

out=$($bin -e -- many_cycles 2>"$root/many-cycles.err")
[ "$out" = "130th cycle complete" ]
strip_resume "$root/many-cycles.err"
only_resume "$root/many-cycles.err"
many_cycles_id=$(grep -rl '"text":"many_cycles"' "$dotdir/sessions" |
    sed 's|/events.jsonl$||;s|.*/||')
out=$($bin -e --resume "$many_cycles_id" -- ping 2>"$root/many-cycles-resume.err")
[ "$out" = pong ]
strip_resume "$root/many-cycles-resume.err"
only_resume "$root/many-cycles-resume.err"

for prompt in managed_wrong_handle managed_malformed; do
    out=$($bin -e -- "$prompt" 2>"$root/$prompt.err")
    [ "$out" = "managed process recovered" ]
    strip_resume "$root/$prompt.err"
    only_resume "$root/$prompt.err"
    managed_id=$(grep -rl "\"text\":\"$prompt\"" \
        "$dotdir/sessions" | sed 's|/events.jsonl$||;s|.*/||')
    python3 - "$dotdir/sessions/$managed_id/events.jsonl" \
        "$prompt" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
prompt = sys.argv[2]
assert not any(event["type"] in {"process_closed", "turn_failed"}
               for event in events)
assert events[-1]["type"] == "turn_completed"
running = [event for event in events if event["type"] == "tool_finished"
           and event["data"]["result"]["status"] == "running"]
assert running
assert all(event["data"]["result"]["handle"] == "a" * 32
           for event in running)
if prompt == "managed_wrong_handle":
    mismatches = [event for event in events
                  if event["type"] == "tool_finished"
                  and event["data"]["result"]["status"] == "not_run"
                  and event["data"]["result"]["reason"] ==
                      "managed_process_handle_mismatch"]
    assert len(mismatches) == 2
    started = {event["data"]["call_id"] for event in events
               if event["type"] == "tool_started"}
    assert all(event["data"]["call_id"] not in started
               for event in mismatches)
else:
    assert len(running) == 2
    assert "interaction was rejected" in running[-1]["data"]["result"]["model_text"]
PY
done

for prompt in managed_final_violation managed_wrong_tool_violation managed_multiple_violation; do
    set +e
    $bin -e -- "$prompt" >"$root/$prompt.out" 2>"$root/$prompt.err"
    status=$?
    set -e
    [ "$status" -eq 4 ]
    [ ! -s "$root/$prompt.out" ]
    grep -q 'provider response violated unresolved managed process ordering' \
        "$root/$prompt.err"
    managed_id=$(grep -rl "\"text\":\"$prompt\"" \
        "$dotdir/sessions" | sed 's|/events.jsonl$||;s|.*/||')
    python3 - "$dotdir/sessions/$managed_id/events.jsonl" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
closed = [event for event in events if event["type"] == "process_closed"]
failed = [event for event in events if event["type"] == "turn_failed"]
assert len(closed) == 1
assert closed[0]["data"]["cause"] == "protocol_failure"
assert closed[0]["data"]["handle"] == "a" * 32
assert len(failed) == 1
assert failed[0]["data"]["class"] == "protocol"
PY
done

after=$($bin -e -v -- text_tool 2>"$root/text-tool.err")
[ "$after" = done ]
grep -q '^Checking first\.$' "$root/text-tool.err"
grep -Fq "→ exec  timeout=1000ms  'fixture ok'" "$root/text-tool.err"
grep -Fq 'arguments: {"command":"fixture ok"' "$root/text-tool.err"
grep -q '^fixture command succeeded$' "$root/text-tool.err"

cat >"$root/tool-output-cap.ini" <<'EOF'
[ui]
verbosity = 1
[tool]
max_output_bytes = 8
EOF
cap_state="$root/tool-output-cap-state"
after=$($bin --dotdir "$cap_state" --config "$root/tool-output-cap.ini" \
    -e -- text_tool \
    2>"$root/tool-output-cap.err")
[ "$after" = done ]
grep -q '^fixture $' "$root/tool-output-cap.err"
grep -q 'output bytes hidden by max_output_bytes' "$root/tool-output-cap.err"
! grep -q '^fixture command succeeded$' "$root/tool-output-cap.err"
python3 - "$cap_state" <<'PY'
import json
import pathlib
import sys

logs = list((pathlib.Path(sys.argv[1]) / "sessions").glob("*/events.jsonl"))
assert len(logs) == 1
events = [json.loads(line) for line in logs[0].read_text().splitlines()]
finished = [event for event in events if event["type"] == "tool_finished"]
assert len(finished) == 1
assert finished[0]["data"]["result"]["model_text"] == "fixture command succeeded"
PY

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
conflict_log=$(grep -rl 'protocol_conflict' "$dotdir/sessions" | head -n 1)
grep -q '"status":"not_run"' "$conflict_log"
! grep -q '"type":"tool_started"' "$conflict_log"

set +e
$bin -e -- tool_crash >"$root/tool-crash.out" 2>"$root/tool-crash.err"
status=$?
set -e
[ "$status" -eq 98 ]
tool_crash_id=$(grep -rl '"text":"tool_crash"' "$dotdir/sessions" | sed 's|/events.jsonl$||;s|.*/||')
out=$($bin -e --resume "$tool_crash_id" -- ping 2>"$root/tool-recovery.err")
[ "$out" = pong ]
grep -q 'unfinished tool work' "$root/tool-recovery.err"
grep -q '"status":"outcome_unknown"' "$dotdir/sessions/$tool_crash_id/events.jsonl"


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
out=$($bin --config "$root/config.ini" -e -v -- ping 2>"$root/config.err")
[ "$out" = pong ]
grep -q '^turn › .* effort=high ' "$root/config.err"
grep -q '^event › .* turn_completed synced$' "$root/config.err"

# The long dotdir switch and its default config path select one coherent root.
default_config_dotdir="$root/default-config-dotdir"
mkdir -m 700 "$default_config_dotdir"
cat >"$default_config_dotdir/config.ini" <<'EOF'
[agent]
model = config-default-model
reasoning_effort = custom-default-effort
EOF
out=$($bin --dotdir="$default_config_dotdir" -e -vvvv -- ping 2>"$root/default-config.err")
[ "$out" = pong ]
grep -q 'model=config-default-model · effort=custom-default-effort' \
    "$root/default-config.err"
[ -d "$default_config_dotdir/sessions" ]
[ -d "$default_config_dotdir/trash" ]

cat >"$root/bad-config.ini" <<'EOF'
[ui]
verbosity = 1
verbosity = 2
EOF
before=$(find "$dotdir/sessions" -mindepth 1 -maxdepth 1 -type d | wc -l)
set +e
$bin --config "$root/bad-config.ini" -e -- ping >"$root/bad-config.out" 2>"$root/bad-config.err"
status=$?
set -e
[ "$status" -eq 2 ]
[ ! -s "$root/bad-config.out" ]
grep -q 'invalid configuration at line 3' "$root/bad-config.err"
[ "$(resume_count "$root/bad-config.err")" -eq 0 ]
after=$(find "$dotdir/sessions" -mindepth 1 -maxdepth 1 -type d | wc -l)
[ "$before" -eq "$after" ]

# Catchable shutdown signals unwind one-shot work through normal cleanup and
# preserve conventional 128+signal exit statuses.
for signal_case in 'INT 130' 'HUP 129' 'TERM 143'; do
    set -- $signal_case
    signal_name=$1
    expected_status=$2
    signal_state="$root/signal-$signal_name"
    $bin --dotdir "$signal_state" -e -- one_shot_signal_wait \
        >"$root/signal-$signal_name.out" \
        2>"$root/signal-$signal_name.err" &
    signal_pid=$!
    attempt=0
    while ! grep -q 'waiting for shutdown' "$root/signal-$signal_name.err"; do
        kill -0 "$signal_pid"
        attempt=$((attempt + 1))
        [ "$attempt" -lt 200 ]
        sleep 0.01
    done
    kill -s "$signal_name" "$signal_pid"
    set +e
    wait "$signal_pid"
    signal_status=$?
    set -e
    [ "$signal_status" -eq "$expected_status" ]
    [ "$(resume_count "$root/signal-$signal_name.err")" -eq 1 ]
done

# Resume command-line settings are consumed by one admitted turn only.
override_state="$root/override-state"
mkdir -m 700 "$override_state"
$bin --dotdir "$override_state" -e -- ping >/dev/null 2>"$root/override.err"
override_id=$(find "$override_state/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
$bin --dotdir "$override_state" -e --effort low --resume "$override_id" -- ping >/dev/null 2>"$root/override.err"
$bin --dotdir "$override_state" -e --resume "$override_id" -- ping >/dev/null 2>"$root/override.err"
python3 - "$override_state/sessions/$override_id/events.jsonl" <<'PY'
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
$bin --dotdir "$auto_state" --config "$root/auto-compact.ini" -e -- ping >"$root/auto-compact.out" 2>"$root/auto-compact.err"
[ "$(cat "$root/auto-compact.out")" = pong ]
grep -Fx '• Compacted' "$root/auto-compact.err"
! grep -q 'event ›' "$root/auto-compact.err"
auto_id=$(find "$auto_state/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
python3 - "$auto_state/sessions/$auto_id/events.jsonl" <<'PY'
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
$bin --dotdir "$responses_compact_state" --config "$root/responses-compact.ini" -e -vvvv -- ping >"$root/responses-compact.out" 2>"$root/responses-compact.err"
[ "$(cat "$root/responses-compact.out")" = pong ]
responses_compact_id=$(find "$responses_compact_state/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
python3 - "$responses_compact_state/sessions/$responses_compact_id/events.jsonl" <<'PY'
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
$bin --dotdir "$pre_state" -e -- ping >"$root/pre-first.out" 2>"$root/pre-first.err"
[ "$(cat "$root/pre-first.out")" = pong ]
pre_id=$(find "$pre_state/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
$bin --dotdir "$pre_state" --config "$root/auto-compact.ini" -e --resume "$pre_id" -- ping >"$root/pre-second.out" 2>"$root/pre-second.err"
[ "$(cat "$root/pre-second.out")" = pong ]
python3 - "$pre_state/sessions/$pre_id/events.jsonl" <<'PY'
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
