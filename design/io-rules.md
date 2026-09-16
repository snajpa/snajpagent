<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Model and I/O rules

A small, stateless policy filter for what the model may do. Rules live in the
normal configuration file as one ordered list, are checked at a real trust
boundary, and either allow or deny each proposed action.

This is **filtering, not containment**. A rule decides whether a proposed action
is admitted. It never makes an underlying interpreter safe: matching a command
string with a regular expression is not a sandbox, and a denied operation is
reported as not run rather than silently swallowed.

## 1. The model in one page

Two things to learn:

1. An **envelope** describes one operation about to cross a boundary.
2. **Rules** in declaration order match the envelope; the first match decides.

```ini
[rule deny-recursive-delete]
match  = {"/tool":"^exec_command$","/text":"rm[[:space:]]+-[a-z]*r[a-z]*f"}
action = deny
message = "Recursive force-delete is disabled. Delete specific paths instead."
```

A rule is one `[rule NAME]` section with three keys: `match`, `action`,
`message`. Every key besides these three — chains, jumps, thresholds,
templates, helper verbs — fails configuration load loudly instead of becoming
silent policy.

### Envelope

The engine matches against a JSON object of immutable facts plus one payload.
Current boundary: a model tool call.

| Field | Meaning |
| --- | --- |
| `boundary` | always `out` (proposed effect) in this build |
| `kind` | `tool_call` today |
| `surface` | who originated it: `model`, `operator`, `irc`, `host` |
| `tool` | native tool name, e.g. `exec_command`, `apply_patch`, `read_file` |
| `value` | the operation payload (here: the tool's argument object) |
| `text` | canonical JSON of `value`, for text matching |

### Rules

| Key | Meaning |
| --- | --- |
| `match` | JSON object of `/json/pointer` → POSIX extended regex; **all** must match. Absent means match everything. |
| `action` | `allow` or `deny`. The first matching rule decides; no match allows. |
| `message` | message shown to the model for `deny`. A value starting with `"` is decoded as a JSON string. Refused on `allow`. |

Every matched rule records one fixed audit line,
`rule=<name> decision=<allow|deny> tool=<tool>`, as a durable `rule_log`
event — including rules after the deciding one, so a trailing match-all rule
audits the whole session without changing any verdict. Logging never changes
the verdict. At most 256 rules, 63-byte names, 64 KiB per message.

## 2. Worked examples

### Deny destructive shell commands

```ini
[rule deny-rm-rf]
match   = {"/tool":"^exec_command$","/text":"rm[[:space:]]+-[^[:space:]]*[rf]"}
action  = deny
message = "Recursive or forced rm is disabled. Remove explicit paths instead."

[rule deny-device-write]
match   = {"/text":"(mkfs|dd[[:space:]]+[^|]*of=/dev/)"}
action  = deny
message = "Raw device writes are disabled in this workspace."
```

### Deny credential exfiltration

```ini
[rule deny-secret-upload]
match   = {"/tool":"^exec_command$","/text":"(curl|wget)[^\n]*\\$\\{?[A-Z_]*KEY"}
action  = deny
message = "Refusing to send environment secrets to a network command."
```

### Read-only workspace mode

```ini
[rule deny-writes]
match   = {"/tool":"^(apply_patch|exec_command)$"}
action  = deny
message = "This workspace is read-only. Inspect with read_file and grep."
```

### Default-deny with an explicit allowlist

Put allowed tools first and a match-all `deny` last:

```ini
[rule allow-commands]
match  = {"/tool":"^exec_command$"}
action = allow

[rule deny-everything-else]
action  = deny
message = "Only the audited command tool is permitted here."
```

### Audit everything, decide nothing

A trailing match-all `allow` logs each call without deciding it:

```ini
[rule audit-tool-calls]
action = allow
```

### Deny encoded or nested execution

```ini
[rule deny-encoded-exec]
match   = {"/tool":"^exec_command$","/text":"(base64[[:space:]]+-d|eval[[:space:]]|\\$\\(|`)"}
action  = deny
message = "Encoded or nested execution is disabled; run the real operation explicitly."
```

### Match a structured argument, not text

`match` walks JSON pointers, so a rule can target one argument field:

```ini
[rule deny-outside-workspace]
match   = {"/tool":"^apply_patch$","/value/patch":"\\.\\./"}
action  = deny
message = "Patches may not escape the workspace with .. paths."
```

## 3. Semantics and limits

- **Bounded.** At most 256 rules and 63-byte names; one linear scan per call.
- **Deterministic.** Validation happens at configuration load; there is no
  runtime rule-generation path and no hidden default action.
- **Not stateful.** A rule behaves identically on every evaluation.
- **One wired boundary.** Only the model tool-call boundary is evaluated.
- **POSIX regular expressions.** `match` uses `regcomp(REG_EXTENDED)`, whole
  substring semantics; anchor with `^`/`$` yourself.

## 4. Why this shape

The engine is a pure function over one ordered list: first match wins, every
match is logged the same way, and there is no second state machine. That keeps
every evaluation reproducible from the envelope and every policy readable top
to bottom. Filtering stays filtering; confinement stays with the tools
themselves.

See `src/rules.c` (engine), `src/config.c` (`[rule NAME]` parsing),
`tests/test_rules.c` (matching, order, invalid definitions),
`tests/rules_e2e.py` (real-binary end-to-end) and `examples/io-rules/`.

## 5. Migration from 0.99.7

0.99.7 shipped chains, jump/return flow, thresholds, log templates and the
insert/command/confirm/value verbs. 0.99.8 keeps only `match`, `action`
(`allow`/`deny`) and `message`; any other rule key fails startup with a
pointer here. Rewrite guide:

| 0.99.7 | 0.99.8 |
| --- | --- |
| `chain = out` | delete the line; all rules form one list |
| `action = reject` + `text` | `action = deny` + `message` (same text) |
| `action = accept` | `action = allow` |
| `action = pass` without `log` | delete the rule (a pass decided nothing) |
| `action = pass` with `log` | trailing match-all `action = allow` (fixed audit line replaces the template) |
| `jump`/`target` chains | paste the target chain's rules at the jump, in order |
| `return` | delete it; earlier rules already decided by position |
| `at_least` thresholds | no replacement; re-request if you enforced one |
| `insert`/`command`/`confirm`/`value` | no replacement; re-request with your use case |

## 6. Handoff and lessons for upstream

This is a filter, not a sandbox: a rule can deny a call, but it does not confine
an admitted call, and matching text is never proof of containment.

- Never describe regex matching as security containment.
- Real process, filesystem and network confinement of `exec_command` remains
  open work and belongs to a native owner, not to rule text.
- The exploration and modification tools constrain by construction
  (workspace-relative paths, atomic replacement, no symlink traversal).
