<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Regression audit, 2026-10-01

The inventory covers the 32 commits listed below, reachable from local refs at
audit intake. The delivered source base
is `197dbf07328e52bdcf413050f36842b3e0ef752a`. Thirty inventory commits are
ancestors of that base; `df431147` is the original Mosh patch superseded by
`165d9120`, and `8a01098b` is an unintegrated test-only branch change.

The audit compares production changes with permanent assertions and checks how
tests are invoked. Documentation-only and fixture-maintenance commits remain in
the inventory. Historical commits are preserved; missing coverage is added in
this follow-up.

## Coverage changes

- Restore `tests/test_token_preflight.py` from the unintegrated branch and run it
  against the production executable from `make check`. It checks exact counting,
  tool continuation, resume, and authentication failure before response creation.
  The test uses an isolated home directory and a local fake provider.
- Extend `tests/test_resume_options.py` with `check_display_preferences`.
  Its real PTY asserts rendered Markdown and the colored exit header, plus
  effective verbosity, across saved settings, explicit overrides and subsequent
  resumes after configuration defaults change. Existing resume checks exercised
  topology and instruction inheritance but did not assert these display effects.

## Commit inventory

| Commit | Change class | Permanent coverage or disposition |
| --- | --- | --- |
| `8a01098b` | Test integration gap | Production token-count preflight, tool continuation, resume, and rejected count authentication. Restored `tests/test_token_preflight.py` and its `make check` entry. |
| `e99135fd` | Fixes and feature | `tools_e2e.py`: `case_cd_call_batch`, `case_home_instruction_symlink`; `test_context.c`: repeated compaction/rebase cache and legacy checkpoint; `test_config.c` and PTY `test_context_configuration_save`: s/save, session-only selection, invalid input, failed writes. |
| `0c7b2705` | Fix and feature | PTY `test_session_names` and store name replay/checkpoint checks; `tools_e2e.py::case_incomplete_stream_recovery` verifies retained partial text, completed effects once, uncompleted calls never executed, and automatic recovery notice. SSH owner survival has `lost-client` coverage. |
| `d7987bf5` | Fix | PTY `test_resume_attaches_live_session`; `test_remote_ssh.py` exercises a dropped client and a TCP blackhole, owner/native-command survival, reclaimed attachment, and subsequent interaction. |
| `fc36007b` | Fixes | `test_session_listing.py` checks bounded single-line previews, controls, Unicode and unchanged journals; `tools_e2e.py::case_native_compaction_many_items` covers 128/129/257 items and subsequent resume. |
| `448a5986` | Fixture correction | Literal IRC input in `test_session_listing.py::check_listing`; the preview assertion checks the endpoint and human-readable body. |
| `fce87d31` | Removal and compatibility | `test_session_archiving.py` covers legacy archived and pending archive-control records, list/last/name resume and append-only history; PTY `test_removed_archive_command_and_picker` and tmux help/policy cases reject the removed command. |
| `ba28132c` | Documentation | Saved-session wording only; no executable behavior changed. |
| `5213d8ee` | Feature | `test_session_listing.py::check_listing` checks the last referenced IRC message, hosted-first endpoints, seven terminal widths, cell widths, shared remaining width and unchanged history; PTY table/header assertions. |
| `1657a914` | Fix | `check_listing` creates a checkpoint with typed IRC input, removes the cached topology field and damages the old prefix; listing must recover the correct preview from verified checkpoint data. |
| `63768c84` | Fix | `test_responses.c::test_structured_keepalives_do_not_end_response` and production `test_response_keepalive.py`: named/unnamed/mixed heartbeats preserve one completion; malformed, mismatched and late response events fail. |
| `4e35be16` | Test integration | Moves `test_response_keepalive.py` to the production executable in `make check`; this audit ran that production path. |
| `2d6be502` | Feature and fixes | `test_session_states.py`: attached/detached/stored, CLI/picker/slash short selectors and read-only listing. `test_base.c::test_session_relay` checks status probes preserve ownership/generation. `test_store.c` checks ambiguity, invalid selectors and expanded collision prefixes. |
| `161b273b` | Regression strengthening | `test_base.c::test_session_relay` verifies rejected unreserved messages close without attachment, generation changes or synthetic detach notifications. |
| `3f8c9b59` | Fixes | `test_resume_options.py`: config/instruction roots, checkpoints, default/custom dotdir hints, network inheritance/overrides/disabled roles; PTY verbosity/network tests. Audit adds `check_display_preferences` for actual color/Markdown rendering, verbosity, explicit overrides and changed defaults. |
| `8a91ed8c` | Regression strengthening | `test_resume_options.py::check` removes the saved instruction root, supplies a replacement, then resumes without it; the replacement persists and the removed root is absent. |
| `5548f3a2` | Feature | `check_listing` covers accepted aliases, fallback nicks, unknown pending aliases, hosted/client order, IPv6, and spoof-like history that must not become topology metadata. |
| `df431147` | Original patch, superseded | Original Mosh-transfer branch patch; corresponding delivered implementation and tests are audited under 165d9120. It is not an additional delivered fix. |
| `0ee257c5` | Fixes | `test_irc.c` covers requested versus accepted nicks, reordered acknowledgements, successive changes, another member owning a requested name, and secondary-endpoint updates. `check_renamed_nicks` covers stale checkpoints, rename chains and damaged prefixes; PTY live nick listing. |
| `fa33c1ca` | Fixture synchronization | PTY `test_live_nick_listing` waits for history readiness before requesting a rename, then checks acknowledged names and listing output. |
| `165d9120` | Feature | `test_remote_terminal.py` covers screen-state send/receive, retry, replay, cancellation, timeout, resize, drag drafts, hidden/partial title frames and real stock-Mosh transfers. `test_upload_wire.c`, upload/download client cases and transport tests cover framing/negotiation and lifecycle. |
| `501c62bb` | Documentation | Combined README guidance only; no executable behavior changed. |
| `40df4393` | Build and documentation | Adds required platform sources to `test_model_cache` linkage; that target is built and executed in this audit. Website wording describes pending Mosh exports. |
| `c768e80e` | Fix | `test_remote_terminal.py::test_exited_child_does_not_wait_for_inherited_terminal_slave` leaves a descendant holding the PTY and requires the wrapper to return exit 7 before the descendant closes it. |
| `3d1f5c63` | Fixture correction | Stock-Mosh transfer case permits normal exit after forwarded SIGTERM; the separate external-termination case retains signal-exit and terminal-restoration assertions. |
| `31b40177` | Fixture and qualification | `test_cli.sh` uses supported lean-provider settings; `test_provider_transport.c` aligns response counts and platform capability guards. No product-code change. Relevant transport binary and terminal tests run in this audit. |
| `c4ca3bae` | Fixture correction | PTY `test_irc_update_prompt_names_update_and_replay_resolves_it` explicitly selects rollout when resuming saved IRC roles, and checks resolved durable input. |
| `45bd58cd` | Fixture correction | PTY saved-goal, inactive/queued goals, empty-session and orderly quit/resume cases account for durable launch metadata while still checking the intended lifecycle. |
| `23d4fae9` | Fixture synchronization | Tmux help and active policy cases assert archive removal; pre-request early-steering case synchronizes provider completion so admission is checked before any completed response. |
| `c25b1c0f` | Feature | `test_config.c::test_prompt_numbers`; production `test_prompt_identity.py`: saved/renamed/resumed/unnamed labels, size rejection, literal braces, endpoint-specific accepted nicks, live rename and draft preservation. |
| `660ab5db` | Fix | `test_provider_https.py`: actual TLS/ALPN offers h2 with stream resets; fixed client selects HTTP/1.1, completes once, retains partial text without replay, and rejects an untrusted certificate. |
| `197dbf07` | Fix | Stock-Mosh recording-SSH test checks application --resume/--help remain literal, option values/attached values, absolute launcher paths, explicit separators, whitespace/Unicode/empty args and inert shell syntax; real transfers omit the manual separator. |

## Execution evidence

All selected checks passed against the fixed source on macOS ARM64 and Linux
x86-64, using local fake providers, disposable sessions and lean builds.

| Host | Passing scope |
| --- | --- |
| Both | Ten C binaries: base/session relay, config, instructions, context, store, Responses parser, provider transport, IRC, upload wire and model cache. Restored preflight success/authentication cases and the new display-preference PTY case. |
| Mac | Eight production Python groups: tools E2E, session listing, legacy archive compatibility, attachment states/short selectors, resume options, trailing keepalives, provider HTTPS and prompt identities. Three remote regressions: inherited PTY exit, Mosh argv and stock-Mosh bidirectional transfer. |
| Linux | All 40 remote-terminal unittest cases and the isolated SSH suite, with no skips. Nineteen targeted `pty_active.py` cases for the audited changes. Three tmux scenarios: removed archive help, early steering before response creation, and archive refusal after policy recovery. Full resume-options checks also pass. |

The display-preference case fails against the retained `0.99.8b-161b273b` Mac
binary on the second invocation: omitted Markdown flags revert to configuration
defaults and strip the literal `**bold marker**`. The same case passes against
the delivered `197dbf07` source. The preflight addition restores an existing
test-only commit; it does not introduce a new runtime fix requiring a before/after
product change. Its rejected-authentication arm verifies that no response-create
request is sent and the turn fails durably.

`make stylecheck`, Python compilation and diff whitespace checks pass. The fetched
master remains `db7abb372c42ea6789fbde634d127fe3fc8bb0fd`, already contained in the
audited branch. Final source compilation follows the test-only commit. This is a
focused audit of the affected paths, not a new aggregate `make check`, sanitizer,
live-provider or full release-matrix qualification.

The complete commit/patch inventory and platform logs remain under
`build/regression-audit/` in the task-owned worktrees. Product source and installed
binaries are unchanged by the audit.
