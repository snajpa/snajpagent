<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Native remote terminal and workstation exports

`snajpagent remote COMMAND…` dispatches before ordinary agent CLI/config startup.
It reads only the local terminal section, chooses a literal command vector (or
SHELL), owns a child PTY and forwards input, resize, signals and exit status.
It creates no local session, provider, tools, IRC runtime or media state.
`[terminal] download_dir` defaults to `~/Downloads`.

## Session attachment transport

The native session endpoint is rooted in its held private directory, not a
caller-supplied pathname. Bind and connect run in a short-lived, reaped child
that changes its own cwd and uses the inherited socket. This removes the
procfs dependency without imposing the Unix socket address limit on session
paths or changing the multithreaded owner's cwd or umask. The child blocks
inherited signal handlers and uses only async-signal-safe calls. The parent
retains peer authentication, endpoint identity checks and the original writer
lock. No session protocol or attachment ownership transition changes.

## Endpoint ownership and wire lifecycle

The wrapper recognizes the existing protocol-1 trzsz transfer marker and runs a
native regular-file endpoint. Upload selection accepts a local path; empty input,
Escape or Ctrl-C cancels. Download publication uses a temporary regular file,
streaming MD5 verification, fsync and non-overwriting hard-link publication.
The final EXIT carries native receipt JSON with actual landed paths. The remote
application restores its terminal lease before rendering that receipt through
ordinary UI text, preserving the composer and cursor. Buffered post-transfer
output and keyboard bytes return to their respective owners.
For a native session, the exclusive transfer lease carries its attachment
identity. A disconnected or replaced client cancels the unfinished exchange;
the old protocol input is discarded before returning to the composer. Completed
verified results remain valid. Workstation probe replies are bound to the same
attachment, and replacements start with fresh capability discovery.
Transfers keep the conversation on the main screen. The native endpoint shows
one rate-limited progress line with direction, byte count and percentage,
shortening it for narrow terminals and clearing it before agent output resumes.
The percentage reports transferred bytes; digest acknowledgement and final EXIT
still determine successful completion. Legacy Go clients retain their own
selection/progress UI and the same protocol-1 framing.

A private CSI probe with a fresh decimal nonce discovers a native client without
starting file-transfer frames. GNU screen receives it through DCS passthrough.
Replies are consumed as terminal events and accepted only against the current
nonce and response window. An inner wrapper probes upstream before spawning its
child; an upstream native wrapper makes it a byte relay rather than a second
filesystem endpoint. Each wrapper clears inherited STY for its new child
PTY. Relays re-envelope recognized probes, markers and protocol frame lines for
the parent GNU screen; ordinary UI output stays in its normal display lifecycle.
Keep the workstation wrapper outside SSH hops and reattach through it. Extra
nested screen backends need a wrapper at the intermediate boundary to relay
passthrough; arbitrary nested multiplexers are outside the qualified paths.

The Go client remains interoperable for explicit `/receive` and `/send`.
Its unchanged protocol does not answer the native availability probe; model
exports with that client remain queued until native attachment or explicit
operator download and queue removal. This preserves detached-screen safety.

## Durable outbox

`send_file` first opens and hashes the regular source or accepted session asset,
then commits `download_queued` to the existing remote session. Each item contains
a stable 32-hex ID, source path/name, size, mtime, SHA256 and enqueue timestamp.
A live native reply permits sending. Otherwise the tool returns queued intent,
explicitly not delivery, and never starts transfer frames. There is no second
local session or automatic age expiry.

Interactive idle processing probes when exports exist. A transition to a live
client flushes a queue snapshot at a safe UI boundary; continued replies avoid
repeated stale-source errors. Unwrapped attachment leaves the queue intact.
Delivery opens and verifies the same FD used for streaming and compares the
transmitted SHA256 before sending the publication-authorizing MD5 frame.
Only digest acknowledgement and final EXIT allow `download_removed`. Failed,
interrupted or uncertain attempts remain pending. If the client landed bytes
but its completion acknowledgement was lost, the next attempt may create a
collision-safe duplicate; the outbox does not claim exactly-once delivery.

`download_queue` lists metadata or commits removal/clear events, optionally with
a reason. Stale entries can be removed without reopening the source. Removal
cancels export intent only, preserving source and already-landed files. Read-only
permits list and refuses all mutations. Replay, checkpoints and session staging
carry the queue; deletion follows ordinary session deletion.

## Regression surfaces

- `tests/test_remote_terminal.py`: pure startup, literal argv, terminal restoration,
  native transfer/receipt, upstream ownership, local GNU screen, detach/reattach,
  unwrapped pending and changed-source retention.
- `tests/test_remote_ssh.py`: disposable authenticated loopback SSH, remote screen
  and a second SSH hop; no shared server configuration or credentials.
- `tests/test_download_client.py`: synthetic wire, durable outbox/list/remove/clear,
  read-only and pinned Go client interoperability.
- `tests/test_upload_client.py`: pinned Go upload and protocol failure coverage.

Qualification records distinguish PTY/path selection from desktop GUI actions.

Native uploads use the existing 1 KiB screen-safe DATA burst even when the peer
advertises a larger wire block. An intermediate relay clears inherited STY at
its child PTY, so the agent may not see an upstream screen input queue. Larger
incompressible DATA lines reproduced a one-byte loss there. This per-frame bound
preserves streaming and acknowledged completion; it adds no file/session quota.
