<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Native remote terminal and workstation exports

`snajpagent remote COMMAND…` dispatches before ordinary agent CLI/config startup.
It reads only the local terminal section, chooses a literal command vector (or
SHELL), owns a child PTY and forwards input, resize, signals and exit status.
After the child exits, it drains ready output and restores the terminal even
when a background descendant still holds the child PTY open.
It creates no local session, provider, tools, IRC runtime or media state.
`[terminal] download_dir` defaults to `~/Downloads`.

When the directly wrapped command's basename is `mosh`, the wrapper inserts `--`
before the hostname. It skips values of documented Mosh options, accepts attached
`--option=value` forms and preserves an existing separator. Unknown or abbreviated
options retain the original argv so Mosh owns their interpretation; those forms
need an explicit separator. Mosh options precede the hostname, and application
flags follow the remote command. Other commands and all argument text retain
literal forwarding. No environment override or shell interpolation is added.

Workstation file streaming uses the existing process-host child read, write and
wait operations. The proxy retains ownership of that child throughout a transfer;
the protocol layer borrows it without reaching into its platform-specific descriptors.

## Session attachment transport

The native session endpoint is rooted in its held private directory, not a
caller-supplied pathname. Bind and connect run in a short-lived, reaped child
that changes its own cwd and uses the inherited socket. This removes the
procfs dependency without imposing the Unix socket address limit on session
paths or changing the multithreaded owner's cwd or umask. The child blocks
inherited signal handlers and uses only async-signal-safe calls. The parent
retains peer authentication, endpoint identity checks and the original writer
lock. No session protocol or attachment ownership transition changes.

NetBSD kernels without a connection-credential query use kernel-generated
`LOCAL_CREDS` metadata. The listener enables it before binding; accepted sockets
inherit it, including a first write that arrives before accept. The relay
verifies the effective UID before admitting the first frame, and the frontend
verifies its destination before sending a terminal profile. Busy refusals also
wait for authentication. A successful check belongs to that connection and
survives its suspension, but never a replacement. Credential peeking runs in a
reaped syscall-only child so ancillary descriptors cannot enter the owner.

## Endpoint ownership and wire lifecycle

The wrapper recognizes the existing protocol-1 trzsz transfer marker and runs a
native regular-file endpoint. Upload selection accepts a local path; empty input,
Escape or Ctrl-C cancels. Download publication uses a temporary regular file,
streaming MD5 verification, fsync and non-overwriting hard-link publication.
When the byte-stream probe has no reply, a separate OSC 2 title challenge
offers screen-state transport. An eight-hex nonce and direction are checked by
the native workstation wrapper before it sends READY as terminal keyboard input.
The executable path, name and TERM value do not choose the transport. Explicit
trzsz-go commands retain the existing marker/wire path after the title timeout.

Stock Mosh retains title state and can skip intermediate terminal output.
The server therefore sends one short title DATA frame at a time and leaves it
in that state until a nonce-, sequence- and CRC-bound keyboard ACK arrives.
Each frame carries at most 120 raw wire bytes; an attempt digit forces a new
title on retries without changing the sequence. The client accepts each
sequence once, acknowledges duplicates, ignores stale frames and withholds
complete OSC strings before deciding whether to forward them. Protocol titles,
including oversized and incomplete ones, never reach the physical terminal.
An END title replaces the final retained data frame on completion or error.
The wrapper ignores repeated HELLO titles during an exchange and retains the
completed nonce so replay cannot start another exchange in the restored composer.
The protocol-1 file codec, digest, receipts, collision-safe publication and
terminal lease are shared with the fast SSH mode. The screen-state configuration
advertises 256-byte DATA blocks and the frame wait is bounded at 20 seconds.
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
filesystem endpoint. Each wrapper clears inherited STY, TMUX and TMUX_PANE for its new child
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
A live byte-stream reply permits fast sending. In an attached interactive
session, send_file can try the title challenge after the fast reply times out.
A positive READY allows screen-state delivery; an unsupported peer leaves the
durable export pending without sending file data. One-shot and detached turns
return queued intent, explicitly not delivery, and never start transfer frames.
There is no second local session or automatic age expiry. Idle outbox discovery
and automatic reattachment delivery remain on the fast byte-stream route;
Mosh clients can request an explicit send after reattachment.

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

## tmux client output

The current attachment profile carries TMUX and TMUX_PANE. A direct argv call to
that socket's tmux list-clients identifies the sole writable, non-control client
whose active pane matches. The route opens that client's character-device tty,
checks its owner and tty identity, and writes protocol output there. Ordinary UI
output stays in the pane. Missing or ambiguous clients fail before file frames;
no tmux options are modified. Input modes and capability probes follow the same
route. Client refresh after the transfer restores tmux's physical display state.

Other panes can render concurrently. Checked title envelopes separate file bytes
from their display output. A positive byte-stream probe enables an extended
HELLO ending in :8192; only its acknowledgement permits 8 KiB title payloads and
upload blocks. An older wrapper ignores that HELLO, then negotiates the original
120-byte envelope. Mosh cannot answer the byte-stream probe and keeps the small
mode. Decoding checks negotiated capacity before writing decoded bytes; CRC,
nonce, sequence, duplicate handling and publication receipts remain shared.

The client-terminal route is resolved afresh for each transfer and capability
probe, so tmux detach/reattach selects the current terminal. Keep the pane focused
and one writable client attached throughout a transfer. Native owner attachment
loss still cancels the transfer lease. Arbitrary nested tmux/remote-wrapper chains
and tmux control mode remain outside the qualified paths; use one workstation
wrapper outside the remote tmux connection.

## Regression surfaces

- `tests/test_remote_terminal.py`: pure startup, literal argv, Mosh option boundaries,
  terminal restoration,
  native transfer/receipt, upstream ownership, local GNU screen, detach/reattach,
  unwrapped pending and changed-source retention.
- `tests/test_tmux_transfers.py`: real tmux between file endpoints, bidirectional
  incompressible bytes, short/noisy panes, cancellation and recovery, multiple
  clients, detach/reattach, local wrapper placement, dropped files, model receipts
  and stock Mosh. Each server and its children belong to the fixture.
- `tests/test_paste_display.py`: single-line, multiline UTF-8 and long completed
  pastes render without another key, directly and through the native wrapper.
- `tests/test_remote_ssh.py`: disposable authenticated loopback SSH, remote tmux/screen
  and a second SSH hop; no shared server configuration or credentials.
- `tests/test_download_client.py`: synthetic wire, durable outbox/list/remove/clear,
  read-only and pinned Go client interoperability.
- `tests/test_upload_client.py`: pinned Go upload and protocol failure coverage.
- `tests/fixture_screen_state.py`: disposable screen-state relay with truncated
  titles, loss, replay and resize. `tests/test_remote_terminal.py` checks both
  directions, unchanged file bytes, cancellation, prompt reuse and absence of
  transfer text in captured physical-terminal output.
- The optional stock-Mosh local test starts an unmodified Mosh client and server
  over loopback, verifies upload and download bytes and terminal hygiene, and
  keeps external SSH credentials and user sessions out of fixture execution.

Qualification records distinguish PTY/path selection from desktop GUI actions.

Native uploads outside negotiated tmux title streams use the existing 1 KiB
screen-safe DATA burst even when the peer
advertises a larger wire block. An intermediate relay clears inherited STY at
its child PTY, so the agent may not see an upstream screen input queue. Larger
incompressible DATA lines reproduced a one-byte loss there. This per-frame bound
preserves streaming and acknowledged completion; it adds no file/session quota.
