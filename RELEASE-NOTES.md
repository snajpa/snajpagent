<!-- SPDX-License-Identifier: GPL-2.0-only -->

# snajpagent 0.99.8b — September 27, 2026

This release follows the immutable 0.99.8 tag. It includes the long-session
recovery, prompt, terminal-upload and model-selection changes developed since
that tag, plus updater support for this letter-suffixed stable version.

## Changes

- Long-running sessions with accumulated media and tool history recover from
  repeated compaction or active-request rejection without re-compacting the
  same already-covered prefix. Completed tool effects and retained attachments
  remain durable. The original first native provider error was not recoverable
  from the journal; the fix is supported by reproductions and fixture coverage,
  not a claimed reconstruction of that missing response.
- The measured context percentage repaints during active progress and at the
  idle prompt. A just-submitted input stays visible when the next composer is
  blocked, without a second echo after admission.
- Wrapped POSIX terminals can receive regular files with the stock `trz` drag
  action or `/upload`. Successful batches become private unsent attachments;
  failed transfers restore terminal input and preserve the pending draft.
  `/attach` also accepts opaque files. Resume and deletion remove abandoned
  upload staging only under the operation's ownership and shape checks.
- The model's `select_model` tool accepts only rows and effort variants in the
  current provider catalog. Selector `cache` refreshes and lists those rows;
  failed refresh leaves the prior catalog and selection intact. The operator's
  typed `/model` still accepts uncached names.
- The updater and release channel accept `0.99.8b` as a stable version after
  `0.99.8`. Stable installations stay on `latest`; Git-suffixed development
  builds stay on `latest-dev`. Existing processes continue running their mapped
  executables until their operators restart them.
- Checkpoint serialization, upload transfer formatting, and upload checksum
  errors use interfaces available in the older BSD build SDKs; compilation
  does not establish new runtime qualification on those operating systems.

## Scope and known limits

The upload client was exercised in a disposable PTY using a pinned stock Go
client, including binary multi-file success and malformed-image rollback;
synthetic wire cases cover other failures. A desktop GUI drag action, nested
SSH, a terminal-specific relay, and prompt/scrollback behavior outside the
PTY fixtures were not qualified. The iTerm2 saved-scrollback display issue is
intentionally left open. The old running processes whose high CPU usage was
sampled mapped older deleted executables. A newly installed 0.99.8 development
snapshot mapped its own inode, used 0.160 CPU seconds for twelve fake-provider
tool calls and zero CPU time while idle for five seconds; this is not a
real-provider or large-journal performance guarantee. No live session was
signaled or restarted during the fix work.

The release ships the implemented `PROD_TARGETS` matrix with separate matching
symbols, manual, source and dependency notices. Build success is not runtime
qualification on every platform. Linux i686 legacy stays outside that matrix:
its pinned uClibc dependency chain remains unbuildable; the opt-in recipe and
honest compatibility notes remain in source and on the download page.

## Downloads and updates

Choose the executable matching the OS, architecture and ABI, compare its
SHA-256 with the published `SHA256SUMS`, and consult the download page for
minimum requirements and tested configurations. The official stable updater
installs a verified replacement in the background and displays one restart
notice; it never restarts a running session. Set `[agent] auto_update = false`
to opt out. Development builds default to updates off. Older release notes and
assets remain available under their original GitHub tags.
