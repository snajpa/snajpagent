<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Terminal Markdown Rendering

This note defines the presentation-only Markdown renderer for model text. It
extends the existing streamed terminal renderer and IRC transcript renderer;
it does not add another output subsystem.

## Controls And Defaults

Markdown rendering is enabled by default. `[ui] markdown = true|false`
configures it, while `--markdown` and `--no-markdown` explicitly override the
configured value. Supplying either command-line form more than once, or
combining the two forms, is an error just like conflicting color options.

The setting affects model text painted on a terminal. Redirected output,
provider traffic, durable events, partial-response recovery, compaction input,
and IRC frames retain the model's exact UTF-8 bytes. Disabling Markdown restores
the existing terminal-safe, width-wrapped literal presentation.

## Presentation

The renderer recognizes the compact Markdown vocabulary routinely produced by
coding models:

- ATX headings;
- unordered and ordered list markers and block quotes;
- backtick or tilde fenced code blocks with optional language labels;
- conventional leading-pipe tables with a header delimiter row;
- inline code, emphasis, strong emphasis, and strikethrough; and
- escapes and inline link destinations, which remain visibly attributable.

Each top-level prose paragraph begins with `• `. Consecutive model items and
Markdown paragraph breaks have one empty terminal row between them. A soft or
hard terminal wrap continues at the left margin rather than under the first
paragraph character; explicit non-blank source line breaks within one paragraph
also remain unbulleted. Headings, list items, block quotes, and fenced code keep
their own structural markers instead of gaining a redundant paragraph bullet.

Headings, quotations, lists, code, and inline spans have readable structural
fallbacks when color is disabled. When color is active, they use only ordinary
SGR attributes and the existing broadly supported 16-color palette. Link
destinations remain visible; the renderer does not emit terminal hyperlink
control protocols. Raw HTML and unsupported Markdown stay terminal-safe and
readable rather than being interpreted as terminal control data.

A table is recognized only when consecutive lines begin, after at most three
spaces, with `|`, and its header is followed by a delimiter row with the same
number of cells. Each delimiter cell has at least three hyphens; leading and
trailing colons select left, center, or right alignment. A trailing outer pipe
is optional. Escaped pipes and pipes inside matching backtick spans do not split
cells. Inline Markdown remains active inside cells, and the header is bold when
color is enabled. Tables are bounded to 16 columns; malformed or larger
candidates retain readable literal Markdown.

When the computed grid is narrower than the terminal, box rules and padded
columns make alignment visible even without color. When it would reach or
exceed the right margin, each body row is rendered vertically as `Header:
value` fields under a compact table marker. The ordinary shared wrapping path
then keeps every value width-safe. Missing body cells are empty and surplus
body cells are ignored, matching the presentation-oriented parser's existing
leniency.

The parser is deliberately bounded and presentation-focused, not an HTML or
CommonMark conformance engine. It recognizes complete, well-formed constructs
greedily and never executes embedded content.

## Streaming And Chat

Parser state lives in the existing public-item render state. Constructs may be
divided at arbitrary provider and UTF-8 delta boundaries. Semantic text from
every complete delta is painted before its delivery callback returns; only an
incomplete UTF-8 sequence, a syntax-only delimiter/prefix, or a potential table
may remain pending. A validated table is buffered through its last consecutive
row because final column widths determine whether grid or narrow rendering is
safe. Candidate text is replayed through the ordinary Markdown path if the
required delimiter row does not validate.
The renderer resets and reapplies active attributes around each terminal write
so composer redraws, status transitions, errors, and later output cannot inherit
model styling. Existing word wrapping, exact-margin handling, typing pauses,
and stream abort behavior remain in force. Markdown text and literal terminal
text share the same punctuation-aware wrapping path; Markdown does not carry a
separate word-break implementation.

Networked final answers are intentionally buffered before being sent to IRC.
When those local or remote non-operator messages are painted in the scrolling
chat UI, the same Markdown presentation is applied after the timestamp and
sender prefix. Operator chat, topics, membership notices, and protocol
diagnostics remain literal. Fenced-code state may continue across consecutive
IRC lines from the same agent; unrelated senders cannot inherit it. Resumed
non-networked assistant history uses the same static model-text presentation.
Because IRC messages are independently framed lines, table recognition does
not join separate IRC messages into one table.

## Acceptance

- Renderer unit tests split every relevant delimiter and multibyte character
  across calls, verify the final styled and no-color presentations, exercise
  abort/reset and terminal safety, and prove delivered bytes remain exact.
- Table tests cover left/center/right alignment, inline styling, escaped pipes,
  optional trailing pipes, byte-at-a-time streaming, malformed fallback, exact
  durable bytes, disabled Markdown, and narrow-terminal labeled rows.
- Configuration tests cover the default, both Boolean values, invalid values,
  and duplicate assignment. CLI tests cover both overrides and conflicts.
- Deterministic tmux coverage checks a genuinely paced Markdown response before
  completion, its byte-exact durable form, static Markdown in the IRC chat UI,
  prose bullets, flush-left continuation lines, paragraph spacing, an aligned
  table, the disabled setting, width safety, and absence of raw escape leakage.
- Full optimized and sanitizer suites plus `make sizecheck` must pass without a
  new production translation unit or dependency.
