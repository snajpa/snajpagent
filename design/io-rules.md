<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Model and I/O rules

A small, stateless policy filter for what the model may do. Rules live in the
normal configuration file, are checked at a real trust boundary, and can pass,
deny, log, or jump to a reusable policy chain.

This is **filtering, not containment**. A rule decides whether a proposed action
is admitted. It never makes an underlying interpreter safe: matching a command
string with a regular expression is not a sandbox, and a denied operation is
reported as not run rather than silently swallowed.

## 1. The model in one page

Three things to learn:

1. An **envelope** describes one operation about to cross a boundary.
2. **Rules** in an ordered chain match the envelope and produce a verdict.
3. **Chains** are named rule lists; `jump` reuses one from another.

```ini
[rule deny-recursive-delete]
chain  = out
match  = {"/kind":"^tool_call$","/text":"rm[[:space:]]+-[a-z]*r[a-z]*f"}
action = reject
text   = "Recursive force-delete is disabled. Delete specific paths instead."
```

### Envelope

The engine matches against a JSON object of immutable facts plus one payload.
Current boundary: a model tool call (`out` / `tool_call`).

| Field | Meaning |
| --- | --- |
| `boundary` | `out` (proposed effect) or `in` (returned data); `event` for lifecycle notices |
| `kind` | `tool_call` today |
| `surface` | who originated it: `model`, `operator`, `irc`, `host` |
| `tool` | native tool name, e.g. `exec_command`, `apply_patch`, `read_file` |
| `value` | the operation payload (here: the tool's argument object) |
| `text` | canonical JSON of `value`, for text matching |

### Rules

A rule is one `[rule NAME]` section. Every key is optional except `chain` and
`action`; unknown keys, bad regexes, duplicate names, and unreachable `jump`
targets fail configuration load, so a typo never becomes silent policy.

| Key | Meaning |
| --- | --- |
| `chain` | which chain this rule belongs to (entry chain `out`, or a `jump` target) |
| `action` | `pass`, `accept`, `reject`, `jump`, or `return` |
| `match` | JSON object of `/json/pointer` → POSIX extended regex; **all** must match |
| `at_least` | JSON object of `/json/pointer` → inclusive integer lower bound |
| `text` | message for `reject`; `%{/pointer}` is replaced from the envelope, `%%` is a literal `%` |
| `target` | chain name for `jump` |
| `log` | template recorded when the rule matches; does not change the verdict |

A `text` or `log` value that starts with `"` is decoded as a JSON string, so
escapes and newlines survive; other values are literal.

### Flow

- Rules run in declaration order within a chain.
- Falling off a chain passes; a rule reaches no verdict by itself.
- `pass` only continues; it never exempts a call from a later `reject`.
- `accept` stops evaluation immediately and allows the operation. Put it before
  a catch-all `reject` to build an allowlist.
- `reject` is **sticky**: it marks the operation denied and traversal continues,
  so later logging rules still run.
- `jump` enters another chain; `return` leaves the current one. A `return` at
  the entry chain stops evaluation.
- A rule may carry a `log` template and any action; logging never changes flow.
- A match is a *whole-operation* decision. There is no "first match wins":
  explicit `reject` rules define denial, which keeps policy declarative.

## 2. Worked examples

### Deny destructive shell commands

```ini
[rule deny-rm-rf]
chain  = out
match  = {"/tool":"^exec_command$","/text":"rm[[:space:]]+-[^[:space:]]*[rf]"}
action = reject
text   = "Recursive or forced rm is disabled. Remove explicit paths instead."

[rule deny-device-write]
chain  = out
match  = {"/text":"(mkfs|dd[[:space:]]+[^|]*of=/dev/)"}
action = reject
text   = "Raw device writes are disabled in this workspace."
```

### Deny credential exfiltration

```ini
[rule deny-secret-upload]
chain  = out
match  = {"/tool":"^exec_command$","/text":"(curl|wget)[^\n]*\\$\\{?[A-Z_]*KEY"}
action = reject
text   = "Refusing to send environment secrets to a network command."
```

### Read-only workspace mode

```ini
[rule deny-writes]
chain  = out
match  = {"/tool":"^(apply_patch|write_stdin|exec_command)$"}
action = reject
text   = "This workspace is read-only. Inspect with read_file, grep and list_files."
```

### Default-deny with an explicit allowlist

`accept` stops evaluation, so an allowlist can sit in front of a catch-all
`reject`. A `pass` rule would not exempt the call from that later denial:

```ini
[rule allow-commands]
chain  = out
match  = {"/tool":"^exec_command$"}
action = accept

[rule deny-everything-else]
chain  = out
action = reject
text   = "Only the audited command tool is permitted here."
```

### Reusable policy chains

```ini
[rule inspect]
chain  = out
match  = {"/kind":"^tool_call$"}
action = jump
target = host-policy

[rule no-network]
chain  = host-policy
match  = {"/text":"(curl|wget|ssh|scp|nc)[[:space:]]"}
action = reject
text   = "Network clients are disabled on this host."

[rule no-privilege]
chain  = host-policy
match  = {"/text":"(^|[|;&[:space:]])(sudo|doas|su)[[:space:]]"}
action = reject
text   = "Privilege escalation is disabled."
```

### Audit everything, decide nothing

```ini
[rule audit-tool-calls]
chain  = out
match  = {"/kind":"^tool_call$"}
action = pass
log    = "tool=%{/tool} args=%{/text}"
```

The line is written as a durable `rule_log` event. Template values are envelope
data, never re-interpreted as configuration or operator input.

### Deny encoded or nested execution

```ini
[rule deny-encoded-exec]
chain  = out
match  = {"/tool":"^exec_command$","/text":"(base64[[:space:]]+-d|eval[[:space:]]|\\$\\(|`)"}
action = reject
text   = "Encoded or nested execution is disabled; run the real operation explicitly."
```

### Match a structured argument, not text

`match` walks JSON pointers, so a rule can target one argument field:

```ini
[rule deny-outside-workspace]
chain  = out
match  = {"/tool":"^apply_patch$","/value/patch":"\\.\\./"}
action = reject
text   = "Patches may not escape the workspace with .. paths."
```

### Thresholds

`at_least` matches only known integers and never a missing field:

```ini
[rule large-write]
chain  = out
at_least = {"/value/bytes":1048576}
action = reject
text   = "Writes over 1 MiB are disabled here."
```

## 3. Semantics and limits

- **Bounded.** At most 256 rules, 64 chains, 4096 visited rules per evaluation,
  63-byte names, 64 KiB per text template.
- **Deterministic.** Validation happens at configuration load; there is no
  runtime rule-generation path and no hidden default action.
- **Not stateful.** There is no `once`, rate limit, or per-session counter in the
  engine, so a rule behaves identically on every evaluation.
- **No helper processes.** Rules cannot run programs or rewrite payloads in this
  build. `replace`, `insert`, `confirm` and external decisions are the next
  slice; they are intentionally absent rather than accepted and ignored.
- **One wired boundary.** Only the model tool-call boundary (`out`/`tool_call`)
  is evaluated in this build. `in` and `event` chains are refused in
  configuration until their hosts exist, so a rule can never silently never fire.
- **POSIX regular expressions.** `match` uses `regcomp(REG_EXTENDED)`, whole
  substring semantics; anchor with `^`/`$` yourself.

## 4. Why this shape

The engine is a pure function. It owns no journal, no threads, no timers and no
second state machine. The calling owner performs the only side effects:
recording a `rule_log` event and producing a factual not-run result. That keeps
the rule language small, makes every evaluation reproducible from the envelope,
and avoids the replay, scope and consent state that a stateful filter would drag
in. Filtering stays filtering; confinement stays with the tools themselves.

See `src/rules.c` (engine), `src/config.c` (`[rule NAME]` parsing),
`tests/test_rules.c` (matching, flow, veto, invalid definitions) and
`tests/rules_e2e.py` (real-binary end-to-end: deny, allow, allowlist, jump, log,
threshold, return, multi-call, resume durability and startup refusals).
