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

The parser is deliberately bounded and presentation-focused, not an HTML or
CommonMark conformance engine. It recognizes complete, well-formed constructs
greedily and never executes embedded content.

## Streaming And Chat

Parser state lives in the existing public-item render state. Constructs may be
divided at arbitrary provider and UTF-8 delta boundaries. Semantic text from
every complete delta is painted before its delivery callback returns; only an
incomplete UTF-8 sequence or a syntax-only delimiter/prefix may remain pending.
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

## Acceptance

- Renderer unit tests split every relevant delimiter and multibyte character
  across calls, verify the final styled and no-color presentations, exercise
  abort/reset and terminal safety, and prove delivered bytes remain exact.
- Configuration tests cover the default, both Boolean values, invalid values,
  and duplicate assignment. CLI tests cover both overrides and conflicts.
- Deterministic tmux coverage checks a genuinely paced Markdown response before
  completion, its byte-exact durable form, static Markdown in the IRC chat UI,
  prose bullets, flush-left continuation lines, paragraph spacing, the disabled
  setting, width safety, and absence of raw escape leakage.
- Full optimized and sanitizer suites plus `make sizecheck` must pass without a
  new production translation unit or dependency.
