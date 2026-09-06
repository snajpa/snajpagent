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

When presenting options, make the simplest viable design the default
recommendation. Only offer a larger variant as an exception, and spell out the
current concrete reason it is needed.

Do not add a second knob, mode, abstraction, helper binary, or subsystem when
one clear behavior is enough.

When a provider lacks an optional native endpoint, keep the user-facing behavior
working through the existing provider path whenever practical.

## User documentation

Website publication is held for the 0.99.1 release. Source changes may ship
to origin/master without deploying Pages. Keep pages.yml manual-only; do not
dispatch it or restore automatic deployment before that release. Follow
RELEASE.md for the release-time deployment and complete downloads.

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

Render the affected manual text with `groff -Tutf8 -man snajpagent.1` and check
its examples and links before shipping documentation changes. The existing Pages
workflow formats that same file as `manual.html`; do not hand-edit or check in
a second generated manual. Screenshots must be real program captures with
correct status fields and current rendering, never synthesized or recolored.
