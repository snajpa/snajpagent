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
export NO_COLOR=1
cd "$root/work"

resume_header='• You can resume this session with the following command:'

resume_count() {
    grep -F -c "$resume_header" "$1" || true
}

only_resume() {
    [ "$(resume_count "$1")" -eq 1 ]
    [ "$(wc -l < "$1")" -eq 2 ]
    [ "$(sed -n '1p' "$1")" = "$resume_header" ]
    resume_command_line=$(sed -n '2p' "$1")
    [ -n "$resume_command_line" ]
    case "$resume_command_line" in
        ' '*|"	"*) return 1 ;;
    esac
    [ ! -s "$1.without-resume" ] || return 1
}

strip_resume() {
    awk -v header="$resume_header" \
        'skip { skip = 0; next } $0 == header { skip = 1; next } { print }' \
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

python3 - "$bin" "$root" <<'PYVERSION'
import os, shutil, subprocess, sys
from pathlib import Path
repo = Path(sys.argv[1]).parent.parent
work = Path(sys.argv[2]) / "version-git"
work.mkdir()
for name in ("Makefile", "config.mk", "META"):
    shutil.copy2(repo / name, work / name)
def git(*args):
    return subprocess.check_output(["git", "-C", str(work), *args], text=True).strip()
git("init", "-q", "--initial-branch=master")
git("config", "user.name", "Version Test")
git("config", "user.email", "version@example.test")
git("add", ".")
git("commit", "-qm", "baseline")
recipe = "version-test:;@printf '%s\\n' '$(BUILD_VERSION)'"
def version():
    return subprocess.run(["make", "--no-print-directory", "-s", "--eval", recipe,
                           "version-test"], cwd=work, text=True, capture_output=True)
assert version().returncode != 0  # No guessed version without a tag.
git("tag", "0.98")
assert version().stdout.strip() == "0.98"
git("commit", "--allow-empty", "-qm", "next")
git("tag", "0.99.0")
assert version().stdout.strip() == "0.99.0"
git("commit", "--allow-empty", "-qm", "development")
assert version().stdout.strip() == "0.99.0-" + git("rev-parse", "--short", "HEAD")
(work / "dirty").touch()
assert version().stdout.strip().endswith("-dirty")
assert "VERSION" not in (repo / "META").read_text()
print("Git-tag build version: ok")
PYVERSION

$bin -h >"$root/help" 2>"$root/help.err"
[ ! -s "$root/help.err" ]
grep -q -- '-s, --listen\[=ENDPOINT\]' "$root/help"
grep -q -- '-c, --client\[=ENDPOINT\]' "$root/help"
grep -q -- '--model-nick NICK' "$root/help"
grep -q -- 'model nick (default agent0)' "$root/help"
grep -q -- '--operator-nick NICK' "$root/help"
! grep -q -- '--operator-name' "$root/help"
grep -q -- '--color\[=WHEN\]' "$root/help"
grep -q -- '--markdown' "$root/help"
grep -q -- '--no-markdown' "$root/help"
grep -q -- '--no-listen' "$root/help"
grep -q -- '--no-client' "$root/help"
grep -q "^usage: $SNAJPAGENT_TEST_NAME " "$root/help"

for args in \
    '-s --no-listen' \
    '--no-listen -s' \
    '-c --no-client' \
    '--no-client -c' \
    '-s -n worker -o WORKER' \
    '-c localhost -c localhost:6667 -n worker' \
    '-s -n worker initial'; do
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
[provider openai]
[irc]
listen = localhost:6667
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

cat >"$root/color-network-error.ini" <<'EOF'
[provider openai]
[ui]
color = always
[irc]
listen = localhost:6667
model_nick = worker
operator_nick = WORKER
EOF
set +e
$bin --config "$root/color-network-error.ini" \
    >"$root/color-network-error.out" 2>"$root/color-network-error.err"
status=$?
set -e
[ "$status" -eq 2 ]
LC_ALL=C grep -q "$(printf '\033')" "$root/color-network-error.err"

set +e
$bin --no-color -s -n worker -o WORKER \
    >"$root/no-color-error.out" 2>"$root/no-color-error.err"
status=$?
set -e
[ "$status" -eq 2 ]
! LC_ALL=C grep -q "$(printf '\033')" "$root/no-color-error.err"

set +e
$bin --color -s -n worker -o WORKER \
    >"$root/color-error.out" 2>"$root/color-error.err"
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
    $bin "$option" -n worker >"$root/empty-endpoint.out" \
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

ro_dotdir="$root/ro-state"
out=$(printf '/ro ping\n' | $bin --dotdir "$ro_dotdir" -e 2>"$root/ro.err")
[ "$out" = pong ]
out=$($bin --dotdir "$root/ro-argument" -e -- '/ro ping' 2>"$root/ro-argument.err")
[ "$out" = pong ]
ro_log=$(find "$ro_dotdir/sessions" -name events.jsonl -print)
python3 - "$ro_log" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
turn = next(event["data"] for event in events if event["type"] == "turn_started")
assert turn["text"] == "ping" and turn["read_only"] is True
PY
set +e
$bin --dotdir "$root/ro-empty" -e -- '/ro   ' >"$root/ro-empty.out" 2>"$root/ro-empty.err"
status=$?
set -e
[ "$status" -eq 2 ]

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
grep -q "^'$bin' --dotdir '$dotdir' --no-listen --no-client --resume '[0-9a-f]\\{32\\}'$" \
    "$root/err"
[ -d "$dotdir/sessions" ]
[ -d "$dotdir/trash" ]
id=$(find "$dotdir/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
[ ${#id} -eq 32 ]
[ "$(wc -l < "$dotdir/sessions/$id/events.jsonl")" -eq 5 ]

# The writer owns the exact two-line header and framing; the builder owns only
# the command, which starts at column zero. Dynamic arguments are POSIX-shell
# safe,
# prompts and credentials are absent, and the printed command really resumes.
quoted_dotdir="$root/quoted ' state"
quoted_config="$root/config/quoted ' config.ini"
mkdir -m 700 "$quoted_dotdir"
cat >"$quoted_config" <<'EOF'
[provider openai]
api_key = ${RESUME_COMMAND_SECRET}
EOF
export RESUME_COMMAND_SECRET='must-not-appear-in-the-resume-command'
out=$($bin --dotdir "$quoted_dotdir" --config "$quoted_config" \
    --color=never --markdown -e -- ping 2>"$root/quoted.err")
[ "$out" = pong ]
[ "$(resume_count "$root/quoted.err")" -eq 1 ]
! grep -q 'must-not-appear\| -- ping\| -e ' "$root/quoted.err"
grep -Fq "'\\''" "$root/quoted.err"
resume_command=$(awk -v header="$resume_header" \
    '$0 == header { getline; print; exit }' "$root/quoted.err")
resume_prefix=${resume_command% --resume *}
resume_id=${resume_command##* --resume }
eval "$resume_prefix -e --resume $resume_id -- ping" \
    >"$root/quoted-resumed.out" 2>"$root/quoted-resumed.err"
[ "$(cat "$root/quoted-resumed.out")" = pong ]
[ "$(resume_count "$root/quoted-resumed.err")" -eq 1 ]
unset RESUME_COMMAND_SECRET

# Forced color styles only the complete lifecycle header. Its reset precedes
# the newline, leaving the copy/paste command as the first uncolored bytes of
# the second physical line.
color_resume_dotdir="$root/color-resume-state"
out=$($bin --dotdir "$color_resume_dotdir" --color=always -e -- ping \
    2>"$root/color-resume.err")
[ "$out" = pong ]
python3 - "$root/color-resume.err" <<'PY'
import pathlib
import sys

header = "• You can resume this session with the following command:".encode()
lines = pathlib.Path(sys.argv[1]).read_bytes().splitlines(keepends=True)
assert lines[0] == b"\x1b[1;32m" + header + b"\x1b[0m\n", lines
assert len(lines) == 2, lines
assert lines[1].startswith(b"'"), lines
assert not lines[1][:1].isspace(), lines
assert b"\x1b" not in lines[1], lines
assert lines[1].endswith(b"\n"), lines
PY

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

for prompt in managed_wrong_handle managed_malformed managed_wrong_tool_violation managed_multiple_violation; do
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
assert all(event["data"]["result"]["handle"] == running[0]["data"]["call_id"]
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
elif prompt == "managed_malformed":
    assert len(running) == 2
    assert "interaction was rejected" in running[-1]["data"]["result"]["model_text"]
elif prompt == "managed_multiple_violation":
    assert any(event["type"] == "tool_finished" and
               event["data"]["result"]["status"] == "not_run" for event in events)
else:
    assert len([event for event in events if event["type"] == "tool_started"]) >= 3
PY
done

for prompt in managed_final_violation; do
    set +e
    $bin -e -- "$prompt" >"$root/$prompt.out" 2>"$root/$prompt.err"
    status=$?
    set -e
    [ "$status" -eq 4 ]
    [ ! -s "$root/$prompt.out" ]
    grep -q 'Unsettled commands remain' \
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
running = [event for event in events if event["type"] == "tool_finished"
           and event["data"]["result"]["status"] == "running"]
assert closed[0]["data"]["handle"] == running[0]["data"]["call_id"]
assert len(failed) == 1
assert failed[0]["data"]["class"] == "protocol"
PY
done

after=$($bin -e -vvv -- text_tool 2>"$root/text-tool.err")
[ "$after" = done ]
grep -q '^Checking first\.$' "$root/text-tool.err"
grep -Eq '→ exec_command \[[0-9a-f]{8}\]  \{"command":"fixture ok"' "$root/text-tool.err"
grep -Fq '  arguments:' "$root/text-tool.err"
grep -q '^fixture command succeeded$' "$root/text-tool.err"

cat >"$root/tool-output-cap.ini" <<'EOF'
[provider openai]
[tool]
max_output_bytes = 8
EOF
cap_state="$root/tool-output-cap-state"
after=$($bin --dotdir "$cap_state" --config "$root/tool-output-cap.ini" \
    -e -vvv -- text_tool \
    2>"$root/tool-output-cap.err")
[ "$after" = done ]
grep -q '^fixture $' "$root/tool-output-cap.err"
grep -Fq '[output truncated]' "$root/tool-output-cap.err"
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

out=$($bin -e -vv -- multi_item 2>"$root/multi.err")
[ "$out" = "Done." ]
grep -q '^Working\.$' "$root/multi.err"

# Flags select exactly one shared ladder; low levels never leak tool output.
flags=
for level in 0 1 2 3 4 5 6; do
    out=$($bin -e $flags -- text_tool 2>"$root/level-$level.err")
    [ "$out" = done ]
    if [ "$level" -eq 0 ]; then
        ! grep -q '→ exec_command' "$root/level-$level.err"
    else
        grep -q '→ exec_command' "$root/level-$level.err"
    fi
    if [ "$level" -lt 2 ]; then
        ! grep -q 'fixture command succeeded' "$root/level-$level.err"
        ! grep -q '  arguments:' "$root/level-$level.err"
    else
        grep -q '^fixture command succeeded$' "$root/level-$level.err"
    fi
    flags="$flags -v"
done
if $bin -e -vvvvvvv -- ping >"$root/seven.out" 2>"$root/seven.err"; then
    exit 1
fi
[ ! -s "$root/seven.out" ]

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


# Configuration is strict and applied before any state mutation.
cat >"$root/config.ini" <<'EOF'
[provider openai]
api_key = ${OPENAI_API_KEY}
[agent]
model = default
reasoning_effort = high
[ui]
resume_history_turns = 0
[tool]
shell = /bin/sh
secret = ${EXTRA_TOKEN}
EOF
export EXTRA_TOKEN=extra-test-secret
out=$($bin --config "$root/config.ini" -e -vvvv -- ping 2>"$root/config.err")
[ "$out" = pong ]
grep -q '^turn › .* effort=high ' "$root/config.err"
grep -q '^event › .* turn_completed synced$' "$root/config.err"

# The long dotdir switch and its default config path select one coherent root.
default_config_dotdir="$root/default-config-dotdir"
mkdir -m 700 "$default_config_dotdir"
cat >"$default_config_dotdir/config.ini" <<'EOF'
[provider openai]
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
grep -q 'invalid configuration at line 2' "$root/bad-config.err"
[ "$(resume_count "$root/bad-config.err")" -eq 0 ]
after=$(find "$dotdir/sessions" -mindepth 1 -maxdepth 1 -type d | wc -l)
[ "$before" -eq "$after" ]

# Invalid-file acceptance uses the real CLI; C retains ownership and I/O failures.
python3 - "$bin" "$root" "$dotdir" <<'PY'
import pathlib
import subprocess
import sys

binary, root, dotdir = sys.argv[1:]
path = pathlib.Path(root) / "invalid-config.ini"
cases = [
    "[ui]\nverbosity=1\nverbosity=2\n",
    "[unknown]\nvalue=1\n",
    "[ui]\nverbosity=7\n",
    "[ui]\ntyping_pause_ms=5001\n",
    "[ui]\ncolor=sometimes\n",
    "[ui]\nmarkdown=maybe\n",
    "[irc]\nhistory_lines=0\n",
    "[irc]\nname=legacy\n",
    "[irc]\noperator_name=legacy\n",
    "[irc]\nclient=localhost\nclient=localhost\n",
    "[ui]\ntyping_pause_ms=1\ntyping_pause_ms=2\n",
    "[ui]\nmarkdown=true\nmarkdown=false\n",
    "[tool]\ndefault_timeout_ms=5000\nmax_timeout_ms=4000\n",
    "[tool]\nmax_timeout_ms=4294967296\n",
    "[tool]\nmax_output_bytes=4294967296\n",
    "[tool]\ndefault_max_output_tokens=6000\n",
    "[tool]\nmax_output_tokens=0\n",
    "[tool]\nmax_output_tokens=4000000001\n",
    "[tool]\nmax_output_tokens=1\nmax_output_tokens=2\n",
    "[tool]\nsecret_env=A,A\n",
    "[provider openai]\nbase_url=ftp://example.test\n",
    "[provider openai]\nbase_url=https://example.test/a?b\n",
    "[provider openai]\napi_key=${BAD-NAME}\n",
    "[provider openai]\nexact_token_count=maybe\n",
    "[provider openai]\nnative_compaction=yes\n",
    "[agent]\nmax_goal_prompt_bytes=0\n",
    "[agent]\nmax_goal_prompt_bytes=1048577\n",
    "[agent]\nread_agents_md=maybe\n",
    "[agent]\nread_agents_md=true\nread_agents_md=false\n",
    "[agent]\nprovider=missing\n[provider present]\n",
    "[provider openai]\nopenrouter_title=\n",
    "[provider duplicate]\n[provider duplicate]\n",
    "[provider paid]\n[model-limit paid/model]\n",
    "[model-limit missing/model]\nmax_input_tokens=1\n",
    "[model-limit /model]\nmax_input_tokens=1\n",
    "[model-limit default/]\nmax_input_tokens=1\n",
    "[model-limit default/model]\nmax_input_tokens=0\n",
    "[model-limit default/model]\nmax_input_tokens=4000000001\n",
    "[model-limit default/model]\nmax_output_tokens=18446744073709551616\n",
    "[model-limit default/model]\nmax_input_tokens=1\nmax_input_tokens=2\n",
    "[model-limit default/model]\nmax_input_tokens=1\n[model-limit default/model]\nmax_output_tokens=1\n",
    "[model-limit default/model]\ncontext_window_tokens=100\nmax_input_tokens=101\n",
    "[model-limit default/model]\ncontext_window_tokens=100\nmax_output_tokens=101\n",
    "[model-limit default/model]\ncontext_window_tokens=100\nmax_input_tokens=70\nmax_output_tokens=31\n",
]
sessions = set((pathlib.Path(dotdir) / "sessions").iterdir())
for config in cases:
    path.write_text(config, encoding="utf-8")
    result = subprocess.run([binary, "--config", str(path), "-e", "--", "ping"],
                            capture_output=True, timeout=10)
    assert result.returncode == 2, (config, result)
    assert not result.stdout, (config, result.stdout)
    assert result.stderr.startswith(b"snajpagent: "), (config, result.stderr)
    assert b"--resume" not in result.stderr, (config, result.stderr)
assert set((pathlib.Path(dotdir) / "sessions").iterdir()) == sessions
PY

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
assert not any(event["type"] in ("effort_changed", "model_selection_changed")
               for event in events)
PY

# Automatic compaction is threshold-gated and durable.
auto_state="$root/auto-compact-state"
mkdir -m 700 "$auto_state"
cat >"$root/auto-compact.ini" <<'EOF'
[provider openai]
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
assert started[0]["data"]["reason"] == "proactive"
assert started[0]["data"]["count_method"] == "unknown"
assert started[0]["data"]["count_request_sha256"]
assert completed[0]["data"]["count_method"] == "unknown"
assert completed[0]["data"]["output_count_method"] == "unknown"
assert completed[0]["data"]["output_count_request_sha256"]
PY

# When the compact endpoint is disabled, compaction still uses Responses.
responses_compact_state="$root/responses-compact-state"
mkdir -m 700 "$responses_compact_state"
cat >"$root/responses-compact.ini" <<'EOF'
[provider openai]
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

# Direct Codex falls back once for an absent native endpoint and remains resumable.
fallback_state="$root/codex-compact-fallback"
mkdir -m 700 "$fallback_state"
cat >"$root/codex-compact-fallback.ini" <<'EOF'
[provider openai]
auth = chatgpt
base_url = https://chatgpt.com/backend-api/codex
auto_compact_input_tokens = 1
EOF
$bin --dotdir "$fallback_state" --config "$root/codex-compact-fallback.ini" \
    -e -- native_compact_unavailable >"$root/fallback.out" 2>"$root/fallback.err"
grep -q 'native compaction unavailable; compacting through Responses' "$root/fallback.err"
fallback_id=$(find "$fallback_state/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
python3 - "$fallback_state/sessions/$fallback_id/events.jsonl" <<'PY'
import json
import sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
starts = [e for e in events if e["type"] == "compaction_started"]
interrupted = [e for e in events if e["type"] == "compaction_interrupted"]
completed = [e for e in events if e["type"] == "compaction_completed"]
assert len(starts) == 2 and len(interrupted) == len(completed) == 1
assert interrupted[0]["data"]["reason"] == "endpoint_unavailable"
assert starts[0]["data"]["compact_id"] == interrupted[0]["data"]["compact_id"]
assert starts[1]["data"]["compact_id"] == completed[0]["data"]["compact_id"]
assert starts[0]["data"]["request_sha256"] != starts[1]["data"]["request_sha256"]
assert completed[0]["data"]["output"][0]["content"] == "fixture responses compact summary"
PY
$bin --dotdir "$fallback_state" -e --resume "$fallback_id" -- ping \
    >"$root/fallback-resume.out" 2>"$root/fallback-resume.err"
[ "$(cat "$root/fallback-resume.out")" = pong ]

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
assert started[0]["data"]["reason"] == "proactive"
assert completed[0]["data"]["output_count_method"] == "unknown"
assert responses2[0]["data"]["compact_id"] == started[0]["data"]["compact_id"]
assert responses2[0]["data"]["profile_id"]
assert responses2[0]["data"]["capability_version"]
assert responses2[0]["data"]["count_request_sha256"]
assert started[1]["seq"] > responses2[0]["seq"]
PY

# Auto tracks the usable budget; explicit/off settings remain independent.
for compact_case in default auto larger fixed below off fallback; do
    budget_state="$root/compact-budget-$compact_case"
    budget_config="$root/compact-budget-$compact_case.ini"
    mkdir -m 700 "$budget_state"
    $bin --dotdir "$budget_state" -e -- ping >"$root/budget-seed.out" \
        2>"$root/budget-seed.err"
    budget_id=$(find "$budget_state/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
    # The fixture counter reports 90,000 tokens, exactly 90% of the small
    # budget. Exercise both pre-response and post-turn/recount boundaries.
    {
        printf '[provider openai]\nexact_token_count = true\n'
        case "$compact_case" in
            auto|larger) printf 'auto_compact_input_tokens = auto\n' ;;
            fixed) printf 'auto_compact_input_tokens = 90000\n' ;;
            below) printf 'auto_compact_input_tokens = 90001\n' ;;
            off) printf 'auto_compact_input_tokens = 0\n' ;;
        esac
        case "$compact_case" in
            fallback) ;;
            *)
                printf '[model-limit openai/gpt-5.5-2026-04-23]\n'
                if [ "$compact_case" = larger ]; then
                    printf 'max_input_tokens = 1000000\n'
                else
                    printf 'max_input_tokens = 100000\n'
                fi
                ;;
        esac
    } >"$budget_config"
    $bin --dotdir "$budget_state" --config "$budget_config" -e \
        --resume "$budget_id" -- compact_budget >"$root/budget.out" \
        2>"$root/budget.err"
    python3 - "$budget_state/sessions/$budget_id/events.jsonl" "$compact_case" <<'PY'
import json
import sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
turn = [e for e in events if e["type"] == "turn_started"][-1]
response = [e for e in events if e["type"] == "response_started"
            and e["data"]["turn_id"] == turn["data"]["turn_id"]]
assert len(response) == 1
assert response[0]["data"]["input_tokens_bound"] == 90000
compacts = [e for e in events if e["type"] == "compaction_started"]
completed = [e for e in events if e["type"] == "compaction_completed"]
expected = 2 if sys.argv[2] in ("default", "auto", "fixed") else 0
assert len(compacts) == len(completed) == expected, (sys.argv[2], compacts)
assert not any(e["type"] == "turn_failed" for e in events)
if expected:
    assert all(e["data"]["reason"] == "proactive" for e in compacts)
    assert all(e["data"]["input_tokens_bound"] == 90000 for e in compacts)
    assert turn["seq"] < compacts[0]["seq"] < completed[0]["seq"] < response[0]["seq"]
    assert response[0]["seq"] < compacts[1]["seq"] < completed[1]["seq"]
PY
done

# Legacy byte/token samples are ignored; an uncounted input remains unknown.
statistical_state="$root/statistical-budget-state"
mkdir -m 700 "$statistical_state"
cat >"$statistical_state/models.json" <<'EOF'
{"providers":[{"base_url":"https://api.openai.com","models":[{"count_capability":"unsupported","default_effort":"medium","efforts":["low","medium","high"],"id":"gpt-5.5-2026-04-23","limits":{"auto_compact_input_tokens":null,"context_window_tokens":null,"effective_context_window_percent":null,"input_context_window_tokens":null,"max_context_window_tokens":null,"max_input_tokens":null,"max_output_tokens":null},"observed_hard_input_tokens":1,"observed_input_tokens":100,"observed_input_bytes":100}],"name":"openai","protocol":"openai"}],"schema_version":1,"updated_at_ms":1}
EOF
chmod 600 "$statistical_state/models.json"
cat >"$root/statistical-budget.ini" <<'EOF'
[provider openai]
auto_compact_input_tokens = 0
exact_token_count = false
EOF
$bin --dotdir "$statistical_state" --config "$root/statistical-budget.ini" \
    -e -- ping >"$root/statistical-budget.out" 2>"$root/statistical-budget.err"
[ "$(cat "$root/statistical-budget.out")" = pong ]
statistical_id=$(find "$statistical_state/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
python3 - "$statistical_state/sessions/$statistical_id/events.jsonl" <<'PY'
import json
import sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
started = [event for event in events if event["type"] == "response_started"]
assert len(started) == 1
assert started[0]["data"]["count_method"] == "unknown"
assert started[0]["data"]["input_tokens_bound"] == 0
assert not any(event["type"] == "turn_failed" for event in events)
assert len([event for event in events if event["type"] == "turn_completed"]) == 1
PY

# In contrast, a configured exact hard limit remains authoritative.
hard_state="$root/hard-budget-state"
mkdir -m 700 "$hard_state"
cat >"$root/hard-budget.ini" <<'EOF'
[provider openai]
auto_compact_input_tokens = 0
exact_token_count = true

[model-limit openai/gpt-5.5-2026-04-23]
max_input_tokens = 1
EOF
set +e
$bin --dotdir "$hard_state" --config "$root/hard-budget.ini" -e -- ping >"$root/hard-budget.out" 2>"$root/hard-budget.err"
hard_status=$?
set -e
[ "$hard_status" -eq 4 ]
hard_id=$(find "$hard_state/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
python3 - "$hard_state/sessions/$hard_id/events.jsonl" <<'PY'
import json
import sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
assert not any(event["type"] == "response_started" for event in events)
failed = [event for event in events if event["type"] == "turn_failed"]
assert len(failed) == 1
assert failed[0]["data"]["class"] == "context"
assert "hard budget 1" in failed[0]["data"]["message"]
PY

# Provider usage drives the meter without predicting later tool-cycle growth.
anchor_state="$root/context-anchor-state"
mkdir -m 700 "$anchor_state"
cat >"$root/context-anchor.ini" <<'EOF'
[agent]
model = gpt-5.6-luna
reasoning_effort = high

[provider openai]
base_url = http://127.0.0.1:2455/backend-api/codex
auto_compact_input_tokens = 0

[model-limit openai/gpt-5.6-luna]
context_window_tokens = 1050000
max_input_tokens = 922000
max_output_tokens = 128000
EOF
$bin --dotdir "$anchor_state" --config "$root/context-anchor.ini" \
    -e -- context_anchor_chain >"$root/context-anchor.out" \
    2>"$root/context-anchor.err"
[ "$(cat "$root/context-anchor.out")" = "context anchor complete" ]
anchor_id=$(find "$anchor_state/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
python3 - "$anchor_state/sessions/$anchor_id/events.jsonl" <<'PY'
import json
import sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
starts = [event for event in events if event["type"] == "response_started"]
assert len(starts) == 5
assert all(event["data"]["count_method"] == "unknown" and
           event["data"]["input_tokens_bound"] == 0 for event in starts)
assert all(event["data"]["hard_input_tokens"] == 922000
           for event in starts)
assert all(event["data"]["input_tokens_bound"] < 922000
           for event in starts)
assert not any(event["type"] == "compaction_started" for event in events)
assert not any(event["type"] == "turn_failed" for event in events)
assert len([event for event in events if event["type"] == "turn_completed"]) == 1
PY

# A typed capacity rejection before output closes the response, compacts one
# complete prefix, and retries exactly one changed provider request.
recovery_state="$root/capacity-recovery-state"
mkdir -m 700 "$recovery_state"
$bin --dotdir "$recovery_state" -e -- ping >/dev/null 2>"$root/recovery-first.err"
recovery_id=$(find "$recovery_state/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
$bin --dotdir "$recovery_state" -e --resume "$recovery_id" -- capacity_recovery >"$root/recovery.out" 2>"$root/recovery.err"
[ "$(cat "$root/recovery.out")" = "fixture answer" ]
python3 - "$recovery_state/sessions/$recovery_id/events.jsonl" <<'PY'
import json
import sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
turn = [event for event in events if event["type"] == "turn_started"][-1]
starts = [event for event in events if event["type"] == "response_started"
          and event["data"]["turn_id"] == turn["data"]["turn_id"]]
rejected = [event for event in events
            if event["type"] == "response_capacity_rejected"]
compacted = [event for event in events if event["type"] == "compaction_started"
             and event["data"]["reason"] == "provider_rejection"]
assert len(starts) == 2 and len(rejected) == 1 and len(compacted) == 1
assert starts[0]["data"]["request_sha256"] == rejected[0]["data"]["request_sha256"]
assert starts[1]["data"]["request_sha256"] != starts[0]["data"]["request_sha256"]
assert rejected[0]["data"]["observed_hard_input_tokens"] == 89999
assert len(rejected[0]["data"]["provider_source_sha256"]) == 64
assert starts[1]["data"]["capacity_source"] == "observed"
assert starts[1]["data"]["hard_input_tokens"] == 89999
assert starts[0]["seq"] < rejected[0]["seq"] < compacted[0]["seq"] < starts[1]["seq"]
PY

# A second typed rejection after recovery is terminal and is never replayed.
second_state="$root/capacity-second-state"
mkdir -m 700 "$second_state"
$bin --dotdir "$second_state" -e -- ping >/dev/null 2>"$root/second-first.err"
second_id=$(find "$second_state/sessions" -mindepth 1 -maxdepth 1 -type d -printf '%f\n')
set +e
$bin --dotdir "$second_state" -e --resume "$second_id" -- capacity_recovery_twice >"$root/second.out" 2>"$root/second.err"
second_status=$?
set -e
[ "$second_status" -eq 4 ]
python3 - "$second_state/sessions/$second_id/events.jsonl" <<'PY'
import json
import sys
events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")]
turn = [event for event in events if event["type"] == "turn_started"][-1]
starts = [event for event in events if event["type"] == "response_started"
          and event["data"]["turn_id"] == turn["data"]["turn_id"]]
rejected = [event for event in events
            if event["type"] == "response_capacity_rejected"]
failed = [event for event in events if event["type"] == "turn_failed"
          and event["data"]["turn_id"] == turn["data"]["turn_id"]]
assert len(starts) == 2 and len(rejected) == 1 and len(failed) == 1
assert failed[0]["data"]["class"] == "context"
PY

# Document roots: CLI validation, quoting, launch-relative paths and resume hints.
python3 - "$bin" "$root" <<'PY'
import json
import pathlib
import shlex
import subprocess
import sys

binary, root = sys.argv[1], pathlib.Path(sys.argv[2])
state = root / "docs-state"
workspace = root / "docs-workspace"
workspace.mkdir()
agents = workspace / "AGENTS.md"
agents.write_text("Workspace content must not be injected.\n")
dirs = [root / 'docs "quoted"', root / "docs-second"]
for directory in dirs:
    directory.mkdir()
    (directory / "AGENTS.md").write_text("Private working notes.\n")
config = root / "docs.ini"
config.write_text("[provider openai]\n[agent]\nread_agents_md=true\n")
common = [binary, "--dotdir", str(state), "--config", str(config)]
launch = root / "work"
options = ["-C", str(workspace), "-d", '../docs "quoted"',
           "-d" + str(dirs[1]), "-d", str(dirs[0] / ".")]
result = subprocess.run([*common, *options, "-e", "--", "ping"],
                        cwd=launch, text=True, capture_output=True, timeout=15)
assert result.returncode == 0, result.stderr
session = next((state / "sessions").iterdir())
def turns():
    return [e["data"] for e in map(json.loads, (session / "events.jsonl").read_text().splitlines())
            if e["type"] == "turn_started"]
expected = [str(agents), *[str(d / "AGENTS.md") for d in dirs]]
assert turns()[-1]["instructions"][-3:] == expected
command = shlex.split(result.stderr.splitlines()[-1])
assert [pathlib.Path(command[i + 1]) for i, arg in enumerate(command) if arg == "-d"] == dirs
resumed = subprocess.run([command[0], "-e", *command[1:], "--", "ping"], cwd=workspace,
                         text=True, capture_output=True, timeout=15)
assert resumed.returncode == 0, resumed.stderr
assert turns()[-1]["instructions"][-3:] == expected
for args in (["-d"], ["-d", ""], ["-d", str(agents)],
             ["-d", str(root / "missing")], ["-d", str(launch)],
             ["-d", str(dirs[0]), "-l"], ["-d", str(dirs[0]), "login", "status"]):
    result = subprocess.run([*common, *args], cwd=launch,
                            text=True, capture_output=True, timeout=15)
    assert result.returncode == 2, (args, result.stderr)
    assert len(turns()) == 2
args = []
for i in range(17):
    directory = root / f"docs-limit-{i}"
    directory.mkdir()
    (directory / "AGENTS.md").touch()
    args += ["-d", str(directory)]
result = subprocess.run([*common, *args, "-e", "--", "ping"],
                        text=True, capture_output=True, timeout=15)
assert result.returncode == 2 and "exceeds 16 files" in result.stderr, result.stderr
print("working-docs CLI: ok")
PY

TERM=xterm "$(dirname "$bin")/pty_interactive.py" "$bin" "$root/work"
TERM=dumb "$(dirname "$bin")/pty_interactive.py" "$bin" "$root/work"
TERM=xterm python3 "$(dirname "$bin")/pty_terminal_matrix.py" "$bin" "$root/work"
TERM=xterm "$(dirname "$bin")/pty_active.py" "$bin" "$root/work"
# Give PTY child teardown a short settle window before the EXIT cleanup removes
# the shared temporary state/workspace tree.
sleep 0.1
echo 'test_cli: ok'
