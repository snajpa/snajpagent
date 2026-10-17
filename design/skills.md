<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Skills

Status: revised design, October 16, 2026. Native implementation is in progress;
qualification and shipment of this policy are pending. This revision replaces the
earlier ambient-root and root-trust admission model.

## Outcome and trust contract

A skill is a reusable package of instructions and supporting resources. Skills
work through ordinary local agents, configured models/providers, queued input,
one-shot execution, workspaces, goals and resume. Runtime rules, current user
input and ordinary tool permissions retain their existing precedence.

The central contract is **operator-curated, approved versions only**. Discovery,
installation, a familiar publisher, a trusted directory, a `$name` reference and
an explicit request to inspect a package do not constitute skill approval.
Unapproved skill contents are adversarial input, including names, descriptions,
vendor metadata, filenames, references, scripts and assets.

The format target remains Agent Skills with Codex metadata and documented Claude
compatibility. Format compatibility confers no admission rights. Approved prose
is attributed lower-priority task guidance, never application policy or an effect
authorization. Approval on one machine does not silently approve another copy on
another machine.

## 1. Discovery roots and configuration

Default discovery uses only the selected application's dotdir: `$dotdir/skills`.
There is no automatic workspace, `~/.agents`, `~/.codex`, administrator, PATH or
bundled-resource root. Launch cwd, `-d` working-document roots, repository files
and frontend attachment do not add search locations.

Expose the entire ordered root list in the normal application configuration:

```ini
[skills]
roots = ["skills"]
```

This is a JSON array of directory paths. Relative paths resolve against the
selected dotdir, not launch/session cwd. Absolute paths select additional roots
explicitly. An empty array disables discovery. Perform no shell evaluation or
arbitrary environment expansion. Configuration rendering includes the effective
default; omitted configuration does not conceal an additional search tier.

`/skills paths` and `snajpagent skills paths --json` enumerate, in order:

- Each configured path, resolved absolute path, canonical target and presence.
- Whether it is the default or explicitly configured, and any path diagnostic.
- Application-owned package selections and their actual generation paths within
  that root, including retained storage excluded from ordinary discovery.
- Every linked origin that would need separate admission or review.

Store managed generations below `$dotdir/skills/.store`. Local receipts select
published packages; discovery does not recursively admit retained generations.
`.store` is reserved managed storage, not an unreported second root. The paths
report explains these selections and exclusions. A configured external root only
makes candidates available for operator review; it approves none of its packages.

Search only configured roots, descend collection containers and stop at a
`SKILL.md` boundary. Deduplicate canonical targets and detect cycles. Attribute
linked roots/packages explicitly. Links escaping a package are refused during
managed import; no linked origin gains approval from its parent. Stable identity
includes source and package location; a separate digest identifies package content.
Configuration order establishes precedence among *approved eligible* candidates.
Same-priority collisions require a qualified ID. Inspect shadowed alternatives;
never merge bodies or let an unapproved duplicate hide an approved package.

## 2. Local approval, bans and review

`$dotdir/skills.json` holds private, effectively owned local policy and managed
receipts. Use checked types, atomic replace, file/directory synchronization and
the existing locking primitives. Repository files and package metadata cannot
modify policy. Keep the records inspectable and version the policy schema.

Separate these states:

- **Pending:** discovered/imported but lacking approval for its exact content.
- **Approved:** a recorded direct operator decision matches the immutable package
  snapshot, source identity and package digest.
- **Changed:** a previously approved source/version differs; the replacement is
  pending review, while an already admitted immutable pin keeps its own version.
- **Disabled:** an approved version excluded from future normal selection.
- **Banned:** an explicit persistent exclusion, taking precedence over approval,
  enablement, root priority and all skill invocation forms.

Approval fingerprints the full package manifest: relative paths, file kinds,
executable bits and file bytes, including frontmatter, body, vendor metadata,
references, scripts and assets. Deterministic traversal excludes Git internals.
Reject special files, cycles and escaping links; import permitted internal links
as ordinary files/directories with recorded provenance. Checked reads detect
concurrent changes. Record the source revision and the exact reviewed snapshot.
A change to any approved content requires a new review; approval of a directory
is not approval of its future contents.

Review makes a private staged snapshot. The operator sees its source, resolved
revision, complete content digest, capabilities, dependencies, manifest/diff and
validation diagnostics. `/skills approve ID --digest HASH` approves exactly that
reviewed snapshot. Verify the digest again at adoption; refuse a race rather than
approving different bytes. Only direct operator command admission may create an
approval. Model tool calls, generated slash text, timers, queued model text and
package instructions cannot do so. The model-facing `skills` tool has no approval,
enable, policy-write or all-entries action. Installing and validating can be
ordinary requested tool work; approval remains an operator decision.

`/skills ban <skill>` accepts a name or a catalog-qualified ID and records a
name-wide exclusion across sources and versions. Resolve an ID through the
operator inventory, including hidden entries. Allow banning an absent standard
name so removal/reinstallation does not erase the exclusion. Name-wide bans
prevent relocation, another root or an update from bypassing the decision.
Record the selected identity/source for explanation, without narrowing the ban.
Ban revokes approvals for that name and retires its active guidance before the
next provider request. `/skills unban <skill>` removes the exclusion but restores
no approval; a subsequent explicit review/approval is required.

Normal catalogs, completion, model context, model list/show/activate results and
operator `/skills` listings omit banned and unapproved contents. Unknown/hidden
lookups give a generic unavailable response to the model rather than disclosing
hidden descriptions. Operator-facing diagnostics may state the exclusion reason.

A launch/CLI **`--all-skills`** option exposes pending, changed, disabled and banned
entries only to operator inventory/review surfaces, clearly labeled by state.
It never approves, enables, invokes, unbans, places hostile metadata in model
context or changes tool permissions. The model has no equivalent parameter.
`/skills review ID` is an explicit operator inspection of candidate data, not an
activation. If the operator asks a model to help review adversarial files, keep
that a separately attributed, request-scoped review task with ordinary permissions;
its report cannot approve the package.

These application admission controls are not an OS sandbox: a process with the
same user's unrestricted filesystem access can modify that user's files. Preserve
existing I/O/exact-approval policy for writes to curation state. Do not claim the
application can protect a policy file from every same-UID process, and do not give
skill instructions authority to request or perform policy edits.

Private policy inspection uses the existing component-wise no-follow path checks.
Select a canonical absolute dotdir; a symlinked parent is rejected, including
operating-system temporary-directory aliases. This preserves the host filesystem
inspection boundary while package-root links remain explicitly enumerated.

## 3. Parsing and validation

Use libyaml's C data parser in native and production recipes. Accept YAML
frontmatter and a Markdown body in `SKILL.md`; parse standard `name`, `description`,
`license`, `compatibility`, `metadata` and experimental `allowed-tools`, preserving
additional data fields. Ordinary aliases, quoted/block scalars, Unicode, mappings
and sequences work. Reject duplicate keys, cycles, multiple documents, unsafe tags,
invalid UTF-8 and malformed declared types with diagnostics. Execute nothing.

The explicit validator checks published field types, naming/length rules and
directory-name correspondence. It resolves local inline Markdown link destinations
outside code spans/fences and vendor icon paths inside the package; missing or
escaping resources fail validation. External links are never fetched. Isolate
malformed entries so valid siblings remain inspectable. Compatibility loading can
explain cosmetic deviations, but cannot bypass approval or unsafe-input rejection.
Descriptions from pending packages belong only in operator review data.

Body-size advice remains advice. Existing file, record, output and transport bounds
apply; add no skill-count quota and silently truncate no adopted instructions.
An oversized approval/activation fails explicitly before adoption.

## 4. Catalog, tools and invocation

Build one catalog with an operator inventory and a filtered eligible projection;
frontends do not rescan independently. Only approved, enabled, unbanned versions
enter the model's compact name/description/origin catalog. Bodies and resources
remain lazy. Include catalog generation and active package digests in request/cache
identity. Search/page within existing output bounds and report filtered counts and
remaining entries truthfully. Large inventories are not silently absent.

One model tool, `skills`, provides:

- `list`: search/page the eligible catalog.
- `show`: inspect an eligible approved version without activating it.
- `activate`: pin an eligible approved version with optional literal arguments.

Activation returns admitted instructions, immutable absolute root, source/package
digests, literal arguments, dependency state and a bounded resource listing. Normal
filesystem/media tools inspect approved supporting resources. Neither activation
nor discovery executes scripts, installs dependencies or contacts a service.

`/skills use ID -- REQUEST` and unambiguous `$skill-name` in direct operator input
select approved versions. Recognize references outside code and escaped text.
Explicit selection does not approve pending or banned packages. Model text cannot
fake an explicit selection for a skill whose policy requires one. Queued requests
resolve and check approval/bans at actual admission, and save their exact binding.
Refresh/cwd/config changes replace the derived catalog at request boundaries;
pending calls retain admitted versions, subject to explicit revocation.

Commit acceptance before projecting active instructions or returning success.
Inspection and activation recheck the complete package manifest against the
catalog entry before returning contents; a changed snapshot requires refresh.
Recheck source/review identity before adoption and report mismatches. Preserve the
original user request independently. Skill names never shadow existing commands.
`/skills clear [ID]` retires pins without undoing completed effects or stopping
commands. Default-prompt metadata inserts editable text and does not submit it.

## 5. Vendor metadata and capabilities

Read `agents/openai.yaml` interface metadata, invocation policy and declared MCP
requirements only through the same review/approval boundary. Resolve icon paths as
package resources. Honor `policy.allow_implicit_invocation: false` in addition to
local approval. Map Claude `disable-model-invocation`, `user-invocable` and
`argument-hint` conservatively; conflicting controls use the restrictive result.

Support `$ARGUMENTS`, `$ARGUMENTS[N]`, `$N` and `${CLAUDE_SKILL_DIR}` as single-pass
literal substitutions. Indexed argument tokenization handles quoting/escaping
without shell/variable evaluation. Preserve source body and rendered instructions
separately. Foreign inline shell expansion, hooks, model overrides and forked-agent
contexts produce compatibility diagnostics and refuse unsupported activation.

`compatibility` is descriptive text. Resolve MCP identities/transports/endpoints
against actual configuration/authentication/catalog state; expose
configured/available/missing/unauthorized states. Declared requirements never
register a server, connect a new endpoint, log in or install anything automatically.

Parse/expose `allowed-tools`; resolve exact recognized references against the
actual catalog. Foreign matcher syntax grants nothing. Skill approval itself does
not grant tool or MCP approval, widen filesystem/network policy or override a
read-only turn. Follow existing explicit approval paths for effects.

## 6. Execution, lifetime and revocation

Scripts use existing managed commands with normal cwd, environment, yielding,
output retention and cancellation. Supply immutable absolute package roots so
resources remain stable without changing session cwd. Report missing interpreters
or resources honestly. No alternate runner, daemon or automatic updater is needed.

Bind exact instructions, rendered arguments, source identity and approved package
digest to the initiating turn or active goal. Retain them through retries,
provider/model switches, compaction, detach and resume; reconstruct from the
journal/checkpoints, never a lossy summary or edited source tree. Deduplicate
identical activations within a binding and preserve deterministic multi-skill order.
A new ordinary task retires non-goal pins. Read-only side queries do not replace
ongoing bindings; goal completion/cancellation retires goal pins.

Recheck local revocations on resume and before each new provider request. Ban or
explicit approval revocation invalidates matching pins even if their snapshot
survives. Record retirement and explain the operator-visible reason; an old
checkpoint never restores revoked authority. An already transmitted provider
request cannot be retracted. Bans add no tool effects, undo completed work or
silently kill existing commands; normal cancellation remains available. Preserve
the original immutable package for other live/unknown owners and forensic history.

The native journal owns approval-bound activation identity and exact snapshots.
Policy remains locally authoritative for current revocations. Required-feature and
checkpoint compatibility handling prevents older binaries from silently resuming
skill semantics they cannot enforce, including the curated-policy revision.
Old sessions without skills keep their existing behavior. Resume never repeats
completed scripts or installation effects.

## 7. Management and authoring

Provide matching operator terminal and noninteractive inventory/management paths:
`list`, `paths`, `show`, `validate`, `review`, `create`, `install`, `update`,
`remove`, `enable`, `disable`, `ban`, `unban`, `refresh` and `clear` where applicable.
Approval is restricted to direct operator admission as described above. `--all-skills`
is inspection-only. Display state and the exact revision/digest in review results.

Local/Git installation stages only the selected package, records resolved commit
and provenance, validates, then atomically publishes a *pending* candidate. Use
existing managed execution for retrieval/yield/cancel; disable Git hooks, filters,
submodule fetches and automatic dependency installation. Reject traversal,
special files and escaping links; omit Git internals and redact source credentials.
Interrupted/concurrent/failed updates preserve the published generation and policy.

Updates require approval for the new manifest before selection. Preserve older
immutable generations for active pins and rollback. Removal affects managed
receipts only, not arbitrary source roots, and preserves ban/revocation records.
Cleanup never discards generations with live/unknown owners. User-source edits
produce pending candidates; never execute directly from an unapproved live tree.

Supply authoring guidance as a standard reviewable package that can be explicitly
copied/imported into the dotdir. Shipping an example does not create a hidden
bundled root or preapprove it. Creation writes a scaffold only; generated skills
remain pending until operator approval. No shipped authoring package self-approves
or grants approval to its outputs.

## 8. Acceptance and delivery

Permanent tests must exercise the real CLI/provider/workspace paths:

1. Defaults enumerate dotdir only. Workspace/admin/home/bundled locations stay
   undiscovered unless configured. Relative roots bind to dotdir; empty roots,
   missing roots, links, collisions and every effective path are explained.
2. Pending/adversarial descriptions, default prompts and dependencies stay out of
   model context/completion/list/show. No discovery/inspection execution, login,
   network call or automatic install occurs. Explicit `$name` cannot approve.
3. Only direct operator approval of the exact reviewed manifest admits content.
   Race/content/resource/script/mode changes invalidate the replacement; source
   relocation and updates cannot inherit approval accidentally.
4. Ban is persistent, name-wide and higher priority than approval. Ordinary views
   hide bans; `--all-skills` only exposes operator review. Reinstall, rename of a
   source directory, another root, unban and old checkpoints cannot restore approval.
5. Approved package scripts/references/assets work through ordinary tools, with
   real output checks, yielding, cancellation, permission denial, unavailable
   dependencies and exact MCP authorization. Adversarial review stays data-only.
6. Compaction/retry/model-switch/resume retain exact eligible pins; revocation
   retires them before another request. Exercise goals, read-only side queries,
   queued input, detached workspaces, required-feature rejection and no effect replay.
7. Atomic local/Git import/update/remove, concurrency, rollback, private policy,
   stable identities, retained generations and complete production dependency builds.

For real official-skill qualification, present selected source revisions and exact
package manifests for operator approval before admitting their instructions to
Luna. Isolate test sessions/artifacts/configuration and synthetic data; a test
approval does not silently become a workflow approval on either machine.

Update the complete manual/tutorial, README/website workflow, configuration examples,
status and Unreleased entry with implementation. Deliver a consistent curation
policy on both machines with the authorized binaries/manuals. Preserve base version,
existing sessions, credentials and gateway defaults; stable/tag/site publication
remains separately authorized. Current source implements the curated admission
path. Full-feature platform qualification and approved Luna workflow acceptance
remain in progress until their recorded delivery checks complete.

## Reference baseline

Official format/integration and Codex/Claude documentation were retrieved
October 16, 2026. They are compatibility inputs, not permission grants. The local
curation contract above governs admission even where another client auto-discovers
or invokes skills. Retained source receipts accompany the design.

- Agent Skills specification: `https://agentskills.io/specification`
- Agent Skills integration: `https://agentskills.io/client-implementation/adding-skills-support`
- OpenAI skills guide: `https://learn.chatgpt.com/docs/build-skills`
- OpenAI metadata reference: `https://github.com/openai/skills/blob/main/skills/.system/skill-creator/references/openai_yaml.md`
- Claude extensions: `https://code.claude.com/docs/en/skills`
