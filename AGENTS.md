<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Project Instructions

KISS is the first engineering rule for this repository. Use the smallest design
that correctly solves the current problem. A larger design needs a concrete
reason from the code in front of you, not a guess about future needs.

Shipment policy: once relevant tests have passed at least once, rebase onto
current master and compile the combined tree. A successful post-rebase build
is sufficient, including after conflict resolution, to integrate and push
authorized work to master; do not require
another full test run, fresh sanitizer run, or expanded validation campaign.
Preserve the existing passing evidence and report its actual scope honestly.
Fix known product failures and build failures; do not conceal them behind an
earlier pass. Publication still requires the operator's shipment authority.

Source and test line counts have no limits or per-file review thresholds.
`make sizecheck` reports counts only. Keep designs simple through ordinary
code review; reintroducing numeric limits requires an explicit operator request.

When presenting options, make the simplest viable design the default
recommendation. Only offer a larger variant as an exception, and spell out the
current concrete reason it is needed.

Do not add a second knob, mode, abstraction, helper binary, or subsystem when
one clear behavior is enough.

When a provider lacks an optional native endpoint, keep the user-facing behavior
working through the existing provider path whenever practical.

## Version authority

The operator alone decides when versions change and approves the version.
“Ship it” authorizes only the requested scope, not a version bump or a new stable
release. The complete-matrix policy applies after that decision. Keep the
approved base for authorized Git-suffixed development builds; fixture versions,
agent plans and push tokens never authorize a new base or stable promotion.
Ask before a version-changing step when the operator's decision is absent.
The canonical release workflow creates the approved Git tag first, then lets
the build system derive its version. Manual overrides remain available but are
not required for an ordinary release. See `RELEASE.md` for the release boundary.

## Regression tests

Every bug fix must include a permanent regression test in the same change.
Reproduce the reported failure before the fix and verify the corrected behavior
afterwards; record any case that cannot be reproduced. Cover relevant interactions
and failure paths, not only the successful example. Temporary probes and manual
checks support diagnosis but do not replace committed regression coverage.

Prefer Python tests using the existing CLI, PTY/tmux and local fake-provider
support. Add the coverage needed for correctness. Keep tests clear and focused,
with bounded runtime and resources.
Use C tests when an internal invariant cannot be exercised adequately through
Python. Use the existing test framework.

## User documentation

Every feature and user-visible behavior change includes its documentation in the
same change. Review the complete affected workflow: manual reference, tutorial,
troubleshooting, README/website, design/status, examples and Unreleased changelog.
Documentation is part of implementation and shipment, not deferred cleanup.
The durable local policy is
`~/ai/docs/projects/snajpagent/reference/documentation-maintenance.md`; it adds no
new harness or release-version authority. When that AI workspace is unavailable,
the requirements in this file and EDITORIAL.md still apply.

Follow [EDITORIAL.md](EDITORIAL.md) for project prose. It prohibits negation-led
reframing, personal narratives, source-detail overfitting and defensive filler.
Select information for the reader's action on that surface; preserve necessary
technical conditions and safety limits. Apply corrections to surrounding copy.

Website publication is manual-only. Source pushes do not deploy Pages.
Deploy an authorized release only with its complete downloads; follow RELEASE.md.
Keep pages.yml manual-only rather than restoring deployment on every push.

Every new version must ship binaries for the entire implemented production
matrix and update the website downloads in the same release. Follow
`RELEASE.md`; `PROD_TARGETS` is the current required matrix. New implemented
targets join automatically; experimental status is disclosed, not an excuse
to omit an artifact. Local builds and source pushes are not binary releases.

`snajpagent.1` is the complete user-manual source. Changes to user-visible
options, commands, defaults, tools, limits, lifecycle, networking or security
must update the relevant manual sections in the same change. Check claims
against source and focused tests, not older prose. Keep README and `www/index.html`
in sync when introductory workflows change; teach rollout before native
networking. Their prose budgets are 2,700 and 1,200 words respectively, excluding
code, not targets. They are unrelated to runtime context limits.

Render the affected manual text with `groff -Kutf8 -Tutf8 -man snajpagent.1` and check
its examples and links before shipping documentation changes. The existing Pages
workflow formats that same file with mandoc as `manual.html`; do not hand-edit
or check in a second generated manual. Keep the source outline task-oriented,
preserve the full reference, and derive web contents from its headings. Check
reading hierarchy, code/definition layout and keyboard navigation, not only
page overflow. Screenshots must be real program captures with
correct status fields and current rendering, never synthesized or recolored.
