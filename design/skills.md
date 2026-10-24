<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Skills and temporary working contexts

## Purpose

An existing session can take on a project's developing-agent role or use a task
harness, complete a bounded piece of work, then return to its parent workspace.
The conversation, selected model, network identity, running commands and broader
assignment remain the same. Persistent documents, state, memory and notes provide
continuity whether or not the session has a native goal.

`work_context` exposes `enter`, `leave` and `status`. Enter requires an existing
`path` and a `task` describing the temporary role and outcome. The current role,
brief and return point are explicit model context. `cd` remains directory
navigation: it refreshes project guidance and skills while retaining an active
temporary role. One active visit is sufficient for a worker using different
projects or roles sequentially; another enter preserves the original parent
return point and replaces the previous visit. There is no visit stack.

## Atomic state and lifetime

Preparation resolves the target directory, instruction references, skill catalog,
active skill bindings and result before committing one `work_context_changed`
event. A preparation failure leaves the old cwd and guidance intact. The event
adopts immutable snapshots together. Running commands retain their own cwd.
The native journal, checkpoint payload source and cwd text index retain the same
state for tail replay and indexed resume; there is no second context registry.

Task and goal skill bindings survive a visit. With one unfinished goal, an
activation binds to its ID. With several, it records their unfinished IDs in
`goal_ids` and remains active while any of those IDs is unfinished. Restart and
fork preserve this scope; later unrelated goals cannot revive it. Skills activated within a visit
carry its ID and survive subsequent turns until leave or replacement. Leave
retires those visit bindings, restores task guidance and the saved cwd, and can
record a supplied summary without a separate model request. If the return directory
has disappeared, leave accepts an explicit replacement path. Neither entering nor
leaving creates, completes or changes a native goal. Read-only turns expose status
and reject context mutations.

Current context is projected separately from historical tool results and compaction
summaries. An explicit null state after leaving prevents earlier visits from being
mistaken for the current role. Sessions that have never used context switching
need no additional role projection. Cache identity incorporates the current visit
ID alongside skill catalog generation and active snapshots.

## Discovery and precedence

For each current directory, walk from that directory to its nearest Git root
inclusive, or inspect only that directory outside a repository. At each level,
search `.snajpagent/skills`, `.agents/skills`, then `.claude/skills`. An active
visit's root also remains discoverable when the worker navigates to a product
repository. Visit-root skills take precedence over product-directory skills.
Configured application roots follow; their relative paths resolve against the
selected dotdir. The default application root is `$dotdir/skills`. An empty
`[skills].roots` array disables application roots while preserving project roots.

Canonical directory identities deduplicate symlink aliases and break cycles,
including shared harness-core collections outside the originating tree. Earlier
roots shadow later same-named packages. Multiple different packages with the same
name at one precedence remain ambiguous; a qualified ID disambiguates them.
`/skills paths` reports roots, canonical targets, presence, diagnostics, linked
origins and managed selections. Retained `.store` generations remain excluded
from recursive discovery and are selected through installation receipts.

## Loading and management

Discovery reads `SKILL.md` and optional `agents/openai.yaml` to build a metadata
catalog. Body and vendor frontmatter are omitted from the model's catalog listing.
Show/activate reread and check the instruction digest against discovery; changed
instructions require refresh. Resource trees remain lazy. Explicit validation
checks resource references and the complete manifest; managed imports verify and
copy packages atomically. Unrelated resource changes do not invalidate an already
pinned instruction snapshot. Retry and resume retain that exact snapshot even if
its source is removed. Refresh keeps loaded snapshots pinned. Explicit activation
of updated instructions replaces the same skill ID and arguments; different
arguments retain separate bindings.

The native `skills` tool provides list, show, activate, deactivate and refresh.
Activate loads a binding; deactivate requires an ID or unambiguous name and removes
all bindings of that identity from subsequent request guidance. Multiple snapshots
of one ID remain unambiguous, including snapshots retained by older clients.
A name matching different IDs requires a qualified ID. Both actions emit lifecycle
bullets after saving the binding state. A missing active selection returns zero
removals. An unchanged
catalog is reused within a turn instead of rescanning on every tool call. Arguments
are literal substitutions, including `$ARGUMENTS`, numbered arguments and the
portable skill-directory marker. `$name` and `/skills use ID -- REQUEST` support
explicit task selection; unknown dollar words and prices remain literal.

Approval/review records, enable/disable lists, bans, admission-origin lists and the
alternate inspection inventory are removed. Legacy curation fields are ignored;
retained files are preserved. Configuration selects available packages. The OS
controls filesystem/process access and services retain their authentication.
Skill prose remains task guidance below runtime rules and current user input.
Vendor hooks, model names, contexts and subagent declarations are metadata: reading
or activating a skill does not execute them or silently select another model.
Scripts use ordinary managed command tools.

## Using existing workflows

Start by reading the actual project or harness entrypoint and follow its pointers.
Keep designs, implementation evidence and investigation reports in that workflow's
ordinary files. Use scripts only when their concrete inputs and effects fit the
task. A vendor-specific launcher is not needed to read portable role instructions
or invoke an ordinary helper with the current runtime.

A single existing worker is the default. Review findings can be produced
sequentially, but self-review must be labeled accurately. A workflow requiring
independent reviewers retains that gate: an explicitly adapted single-worker
workflow must describe its changed review contract instead of claiming the original
gate passed. When actual independent work is useful, ordinary CLI processes are
available; no native child-session hierarchy is introduced. Production investigation
harnesses retain their read-only and non-destructive rules; disposable local evidence
is sufficient for runtime qualification and does not authorize production access.

Bare `/skills` reports loaded guidance; `/skills list` browses the catalog.
Interactive commands and the standalone CLI display concise human-readable reports;
explicit CLI `--json` and native model tool results retain machine-readable data.

## Verification

Permanent CLI/provider coverage lives in `tests/skills.py` and
`tests/test_work_context.py`, selected by `make check`. Native format/checkpoint
coverage exercises context adoption, instruction replacement, later skill updates,
leave and indexed reopen in the ordinary store suite. Parser tests preserve YAML,
literal arguments and lazy snapshot behavior. Real-workflow qualification and
allocation/copy comparisons accompany delivery; fixture success alone does not
establish an unchanged vendor workflow or production authorization.
