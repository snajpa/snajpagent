<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Skills

Status: design settled for implementation, October 16, 2026. This document defines
planned behavior; runtime support and qualification are pending.

## Outcome and current boundary

A skill is a reusable directory of task instructions and supporting resources.
Skills must work through the ordinary local agent, every configured model/provider,
one-shot execution, queued input, attached workspaces, goal continuation and resume.

The inspected source at `6808b6a9` has instruction-path discovery, managed tools,
MCP admission, native session storage and context reconstruction, but no skills
catalog, activation, management commands or YAML dependency. Skills reuse those
owners. The fixed runtime rules and current user input continue to take precedence.

The compatibility target is the Agent Skills format and lifecycle, with explicit
Codex metadata support. Other clients' execution extensions have separately stated
semantics. A skill never authorizes an external effect by describing one.

## 1. Format and parsing

Accept a directory containing `SKILL.md`, YAML frontmatter and a Markdown body.
Implement the standard fields `name`, `description`, `license`, `compatibility`,
`metadata` and experimental `allowed-tools`, and preserve additional data fields.
Supporting directories such as `scripts/`, `references/` and `assets/` remain
ordinary files. A skill may contain other directories and files too.

Use libyaml's C data parser, integrated into native and production build recipes.
This avoids a second hand-written YAML grammar and a runtime Python requirement.
Quoted values, block scalars, Unicode, mappings, sequences and ordinary aliases
must work. Inspect declared fields with checked types; reject duplicate keys,
cyclic structures, multiple documents and unsafe/non-data tags with diagnostics.
Never instantiate objects, interpolate a shell or execute frontmatter.

The explicit validator checks the published field types, naming/length rules,
UTF-8, directory-name relationship and resource references. Runtime discovery
isolates malformed entries: valid siblings stay usable. Cosmetic naming deviations
produce a diagnostic and a stable selectable identity when safe; missing required
metadata, ambiguous YAML or unsafe input prevents activation. Validation is strict;
compatibility loading remains usable and explains every deviation.

Published advice about body size is advice. Existing file, record, tool-output and
provider transport bounds remain real limits; introduce no skill-count quota or
silent body truncation. Oversized activation fails explicitly before adoption.

## 2. Discovery, identity and trust

Default roots:

- Workspace `.agents/skills`, from the session cwd upward to its repository root;
  outside a repository, inspect the cwd's root only.
- User `~/.agents/skills` and the application's selected `$dotdir/skills`.
- Administrator `/etc/snajpagent/skills` on systems with that location, plus shipped
  skill resources resolved through the existing installed-resource mechanism.
- Explicit additional roots in local application configuration. Legacy Codex,
  Claude or organization-specific directories can be included this way.

Persistent `-d` instruction roots do not implicitly become skill roots. Discovery
uses saved session cwd, not whichever directory a frontend occupies. A missing cwd
retains user/admin availability and the existing absolute-cd recovery behavior.

Search only skill roots, never the whole working tree. Descend collection
containers, stop at a `SKILL.md` package boundary and deduplicate canonical targets.
Support linked root/package directories with explicit source attribution and
cycle detection. A linked target outside an admitted root is a separately admitted
origin; ordinary resource access does not silently traverse into another origin.

Precedence: explicit configured roots, nearest workspace, application-user root,
portable user root, administrator, bundled. Configuration order resolves explicit
root priority. Same-priority duplicates are ambiguous until qualified. Keep all
colliding entries inspectable by origin-qualified ID; never merge their bodies.
Display the selected default and shadowed alternatives. JSON/provider and terminal
escaping keep descriptions and filenames as data. Identity follows origin
and package location; a separate digest identifies the actual instruction version.

User/admin/explicitly configured roots are admitted according to local config.
Workspace skills are discoverable but require a locally stored workspace trust
choice before entering automatic model context. Repository files cannot grant
that trust themselves. Show new/changed origins and diagnostics; trust of a root
is not a claim that every future edit has been independently reviewed.

## 3. Catalog and activation

Publish compact, escaped name/description/origin records; keep bodies and resources
out of the initial catalog. Carry catalog generation in request/cache identity.
Use a searchable, cursor-paged listing when the complete catalog cannot fit the
existing output/request constraints. Report total/remaining entries truthfully.
A skipped page must never mean that its skills do not exist.

Provide one model tool, `skills`, with `list`, `show` and `activate` actions.
`list` searches/pages the catalog; `show` inspects metadata and source text without
activating it. `activate` accepts a qualified ID and optional literal arguments.
The result contains the full admitted instructions, skill root, version digest,
invocation arguments, dependency state and a bounded resource listing. Generic
filesystem/media tools inspect additional resources as needed.

Expose the same catalog, diagnostics and active bindings to terminal completion
and semantic workspace views; attachment does not create a second catalog owner.
Skill names do not shadow existing slash commands.

The model selects relevant skills from their descriptions. `show` never grants
invocation rights. An operator `/skills clear [ID]` retires active pins without
undoing tool effects or terminating commands. Explicit selection is
available through `/skills use ID -- REQUEST` and unambiguous `$skill-name`
references in direct operator input. Recognize references outside code/escaped
text; unknown or ambiguous explicit selections return a useful error. Queued
requests resolve against the catalog at actual admission, with that binding saved.
An already admitted invocation retains its original identity across retries.

Commit activation before making it active. Recheck a changed discovery version
before admission; report the change instead of silently using different metadata.
Treat skill prose as attributed, lower-priority task guidance, including when
provider adapters flatten messages. Preserve the original user request separately.

Default-prompt metadata inserts editable composer text; it does not submit input.
Arguments are literal task data. Importing a skill never evaluates command
substitution or performs recursive template expansion.

## 4. Vendor metadata and dependencies

Read `agents/openai.yaml` interface metadata, invocation policy and declared MCP
tool dependencies. Display title, short description, default prompt and available
appearance information; resolve icon paths through ordinary resource handling.
Honor `policy.allow_implicit_invocation: false`: only a matching explicit operator
selection may activate that skill. Model-generated text cannot fake that selection.

Recognize Claude invocation controls (`disable-model-invocation`, `user-invocable`
and `argument-hint`) with documented mappings to the same admission state. Conflicting
controls use the more restrictive result. Support `$ARGUMENTS`, indexed arguments
(`$ARGUMENTS[N]` and `$N`) and `${CLAUDE_SKILL_DIR}` as literal substitutions.
Tokenize indexed arguments with quoting/escaping only; perform no shell or variable
evaluation. Substitute once over the original body so inserted argument text can
never introduce another substitution. Preserve the source digest and rendered
instructions separately.
Foreign executable features such as inline shell expansion, lifecycle hooks,
model overrides and forked-agent contexts produce explicit compatibility diagnostics;
activation requiring those behaviors is refused until the skill is adapted.

`compatibility` remains descriptive text, not an executable dependency expression.
Resolve declared MCP requirements against configured server identities, transports
and endpoints. Show configured/available/missing/unauthorized states. Missing
services guide the user to existing MCP configuration/authentication commands.
A skill declaration cannot register a server, start login, install a package or
connect to a new endpoint automatically.

Parse and expose `allowed-tools`; resolve recognized tool references against the
actual local catalog. Existing permissions, read-only turns, I/O rules and exact
MCP approvals remain authoritative. Foreign matcher syntax never becomes a broad
allow rule. Additional approval follows the existing explicit policy path.

## 5. Execution and lifetime

Scripts run through ordinary managed command tools, with their normal environment,
cwd, cancellation, output retention and yielding. Provide absolute skill-root paths
so scripts can find bundled resources while the session cwd remains unchanged.
Installed scripts require an available interpreter or executable; discovery and
activation execute nothing. Assets use the existing file/image/document tools.

Bind activation to its initiating turn, or to the goal identity when invoked for
ongoing goal work. Keep exact active instructions and literal arguments through
that turn's retries, provider/model switches, compaction, detach and resume.
Read-only side queries do not replace an ongoing binding. A new ordinary task
retires prior non-goal pins; goal completion/cancellation retires that goal's pins.
History remains intact and follow-up requests may reactivate the same skill.

Deduplicate identical skill/version/argument activations within a binding. Preserve
multiple distinct skills with deterministic admission order. Their text cannot
change priority by claiming to be system instructions. Compaction reprojects the
active pinned instructions and their attribution, rather than relying on a lossy
summary to reproduce them.

The journal owns activation identity, exact loaded body, arguments and lifecycle.
Checkpoints and provider views derive from those events. Include active digests in
continuation/cache bindings. Add native required-feature/read-write compatibility
handling so an older binary cannot resume and silently drop active skill semantics.
Older sessions with no skill state retain their current behavior.

Instruction snapshots are exact. Workspace resources remain live files accessed
through ordinary tools and may change or disappear; report this honestly. Resume
never replays completed scripts, installation steps or other tool effects.

## 6. Management and updates

Offer matching terminal and noninteractive entrypoints:

- `/skills` and `snajpagent skills list/show/validate` for catalog and diagnostics.
- `use` for invocation; `enable`, `disable` and `refresh` for local selection.
- `create` for a standard scaffold, plus bundled authoring instructions usable by
  any configured model through ordinary file-writing tools.
- `install`, `update` and `remove` for application-owned packages. Accept a local
  directory or explicitly selected Git source/subdirectory/ref; record the resolved
  commit and provenance. Git-based operations require an available Git executable.

Use existing managed-process execution for slow retrieval so the agent can yield;
no additional runner, daemon, remote registry service or silent startup updater.
Review metadata and required capabilities before enabling an imported package.
Network retrieval and execution remain distinct actions; installation runs no hooks.
Copy only the selected package tree: reject path traversal, special files and
links escaping that package; omit Git internals. Submodule fetches and dependency
installations require their own explicit actions. Redact credentials from source
URLs, diagnostics and provenance receipts.

Stage, validate and atomically publish managed packages under private application
storage. Immutable installed generations keep admitted resource paths stable across
updates; retain previous generations for ongoing invocations and rollback. A small
local install receipt records source/revision and owned files. Removal touches only
that managed package, never an arbitrary workspace/user source directory. Explicit
cleanup must preserve generations with live/unknown owners. Source edits outside
managed packages remain ordinary filesystem edits.

Refresh builds a replacement catalog and swaps only after validation. It applies
at request boundaries; pending calls and pinned activations keep their admitted
version. Enabling/disabling affects future selection and never implicitly cancels
already running commands or rolls back completed effects.

## 7. Implementation shape

Keep one small skills module (`skills.c/.h`) for parsing, discovery, identities,
resolution and validation. Application glue owns commands, config and admission;
existing tool/process and MCP owners execute effects. Extend context projection,
store event/reducer/checkpoint encoding and semantic workspace state at their
existing boundaries. There is no plugin ABI, skill scheduler or alternate journal.

Canonical event/status boundaries: catalog replacement, activation accepted or
refused, dependency resolution, task-binding retirement and managed installation
publication. Reuse normal tool-start/completion records for script execution.
Expose counts/digests and inspectable errors without unconditional large traces.
Keep content lazy, avoid repeated whole-root scans per token/request, and invalidate
catalog/cache state on explicit refresh, cwd or relevant configuration changes.

## 8. Acceptance and implementation order

1. Format/parser plus deterministic resolver: real portable and Codex examples,
   aliases/block scalars/Unicode, duplicates, bad YAML, links/cycles, trust,
   precedence, collisions, missing cwd, refresh and large paged catalogs.
2. Actual CLI/provider activation: explicit and implicit selection, literal
   arguments, disabled invocation, dependencies, multiple skills and read-only
   restrictions. Discovery must have zero execution/network side effects.
3. End-to-end workflows: run a bundled script through a managed tool, inspect a
   reference, consume an asset and verify the real output. Exercise slow yield,
   cancellation, permission denial, unavailable interpreters and MCP authorization.
4. Durability and consumer equivalence: active-turn and goal compaction/resume,
   source deletion/change, model switches, old-session imports, required-feature
   rejection, queued input, detached/reattached workspace state and no tool replay.
5. Management: local/Git install, pinned provenance, interrupted retrieval,
   concurrent update, atomic rollback, live-generation retention and safe removal.
6. Run relevant permanent regressions plus the repository gate, explaining any
   baseline/environment failures. Build the combined current source and validate
   supported production dependency recipes, with native Linux/Mac smoke coverage.

Update the manual/tutorial, README/website introductory workflow, design/status,
configuration examples and Unreleased entry together with runtime implementation.
Source publication and installations require the current shipment scope; a base
version change, stable release or website deployment remains a separate decision.

## Reference baseline

Official format/integration documentation and Codex/Claude documentation were
retrieved October 16, 2026. Source receipts and retained copies accompany this
design locally. These references are compatibility inputs, not permission grants.

- Agent Skills specification: `https://agentskills.io/specification`
- Agent Skills integration: `https://agentskills.io/client-implementation/adding-skills-support`
- OpenAI skills guide: `https://learn.chatgpt.com/docs/build-skills`
- OpenAI skill-creator metadata reference: `https://github.com/openai/skills/blob/main/skills/.system/skill-creator/references/openai_yaml.md`
- Claude Code skills extensions: `https://code.claude.com/docs/en/skills`
