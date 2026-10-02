<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent 0.99.8c — October 2, 2026

This release combines terminal, session, provider and context-recovery fixes with
voice-continuity changes. Sessions retain JSONL storage.

## Changes

- Session names, resume selection and status listings preserve live ownership and
  saved options. Listings group sessions by status and show ten stored sessions
  by default; `-l N` selects another count.
- Command reports use the configured pager. Active-turn verbosity controls remain
  responsive during blocked engine work, with immediate terminal feedback.
  Successful reports remain available to voice observations; `/cat` contents stay
  operator-only.
- Native terminal attachment publishes a handover after the destination frontend
  confirms it is bound. Input and audio activation follow that acknowledgement;
  managed work stays with the session owner through detach and attachment changes.
- Context preparation retains cached request prefixes, retry notices and completed
  tool results across compaction, recovery and model changes. Token preflight keeps
  generation-only controls out of count requests. HTTP response streams use
  HTTP/1.1 and preserve completed work across connection failures.
- Voice captions accumulate transcript fragments. Voice and working-model guidance
  distinguishes their roles: execution remains with the working model, and original
  ASR text stays separate from derived paraphrases and approval decisions.
  Voice intent remains enabled through work and recoverable failures, with honest
  unavailable states. Conversational guidance supports progress, quiet and resume
  requests while work continues.
- POSIX command children reset inherited ignored SIGINT so managed interruption
  works when the agent is launched in the background as well as in the foreground.
  Parent signal settings remain unchanged.

The changelog contains the complete change list.

## Scope and compatibility

Hermetic provider, CLI, PTY and terminal fixtures exercise these paths. Device and
operating-system runtime observations retain their recorded revisions in
`QUALIFICATION.md`; compilation does not qualify a newer executable on that device.
Long-session voice continuity and recovery against live providers remain
unqualified by the fixture results.

Native attachment uses a new private protocol. Use a compatible executable to
attach to an older running owner and finish or stop it deliberately. Existing
processes keep their running code until their operators restart them.

Production recipes enable device audio and native voice backends across the
implemented matrix. File decoding, configured provider routes and device access
are separate capabilities. Office rendering uses the separately installed runtime
documented in the manual.

## Downloads and updates

The release includes the complete implemented `PROD_TARGETS` matrix, matching
symbols, manual, application and dependency sources, notices and `SHA256SUMS`.
Select the exact OS, architecture and ABI, then verify its checksum before
installation. Experimental platforms retain their qualification labels. macOS
builds retain the documented per-file quarantine exception for a verified download.

Official stable builds use `latest` and default automatic updates to on. Updating
replaces the executable for the next launch; it does not replace running processes.
Git-suffixed development builds use `latest-dev` and default automatic updates to
off. Older releases and their immutable assets remain available under their
original GitHub tags.
