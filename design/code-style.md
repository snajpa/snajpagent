<!-- SPDX-License-Identifier: GPL-2.0-only -->

# Code style

Approved 2026-09-15. Adoption is hybrid: this guide binds new and changed lines from the
approval revision onward, and the existing tree is not reflowed. Untouched code converts when it
is next edited, or in a dedicated style-only change. A style-only change is reviewed for token,
string, comment and preprocessor preservation, then run through the relevant existing tests;
`git diff -w` supports that review without proving it, because it hides a whitespace change
inside a string literal. Approved reflow revisions are listed in `.git-blame-ignore-revs`.
Whole-tree conversion is a separate, later change.

The base is the OpenZFS/illumos style adapted to this tree; the structural deviation is the
indent character: four spaces, with tabs banned.

Implementation style contract, kept in force: ordinary readable C; explicit ownership, error,
cleanup and lock paths; checked or demonstrably bounded arithmetic; no packed line-count tricks;
size budgets are architectural signals, not formatting targets. `make stylecheck` stays
deliberately mechanical: it reports objective drift and does not replace review.

## Layout

### Anchor

- Four spaces per indentation level; tabs are banned.
- 80 columns preferred, 100 hard. The hard limit is what the changed-lines check enforces. A line
  over the limit is fixed by extraction or splitting.
- A line whose overrun is one unbreakable token - a URL or string literal longer than the limit -
  is allowed when the rest of the line fits the limit.

### Files and includes

- Line 1 of every C/H file is exactly `/* SPDX-License-Identifier: GPL-2.0-only */`.
- Include guards: `#ifndef SNAJPAGENT_<NAME>_H` ... `#endif`; `#pragma once` is banned.
- Include order: the file's own header first; project headers alphabetically; one blank line;
  system headers alphabetically. `src/term.h` is the model.

### Braces and control flow

- Control statements use `) {` on one line; `} else {` and `} else if (...) {` continue on one
  line.
- Function definitions put the return type and the name on their own lines above a lone `{`.
- A braceless one-statement body is allowed only when the whole statement fits one line within 80
  columns; anything wrapped takes braces.

### Spacing

- `if (`, `for (`, `while (`, `switch (` - a space after the keyword; one space after `return`;
  no space between a function or macro name and `(`.
- One space after commas, none before `,` or `;`, no space inside parentheses or brackets.
- Binary operators take one space per side; unary operators attach (`!x`, `*p`, `&x`, `++i`).
- Pointer stars bind to the name (`char *p`); casts attach (`(size_t)x`).
- No column alignment of assignments; multiple spaces appear only for comment alignment and
  preprocessor continuations.
- The minified dialect is a defect in every form - `if(!x)return -1;`, `dir=snag_...`, `){`. It is
  the changed-lines cleanup target.

### Declarations

- Declare a local before its first use in the smallest block that owns it. C99 declarations after
  guards or at loop starts are preferred when they shorten lifetime and improve locality. Keep
  one declaration per line and initialize it when a correct initial value is available.
  Declaration placement is review judgment, not a checker rule.

### Comments

- Comments use `/* */` only; `//` comments are banned.
- A comment sits above the code it describes and explains why, the failure mode, or the
  invariant. A comment that restates the next line is noise; a stale comment is a defect and is
  updated or deleted with its code. No decorative banner art; a section break is one short
  comment line.

### Preprocessor

- Directives at column 0. A conditional block longer than about ten lines closes with
  `#endif /* CONDITION */`. Macro names keep `SNAG_UPPER_UNDERSCORE`; statement macros wrap in
  `do { ... } while (0)`.

### Wrapping

- Long conditions: one term per line with the operator at the end of the line. Continuation
  indent aligns after the opening delimiter, or adds four spaces when alignment would pass about
  40 columns from the statement start; one form per statement, not per line.

## Grouping and review policy

These rules are review obligations, deliberately not a gate.

- **Blank lines group steps.** A blank line marks a boundary between steps of a function. A step
  is the statements that together do one thing for what follows - prepare inputs, call out,
  interpret the result. Keep a step's statements adjacent; separate steps with exactly one blank
  line; never place a blank line inside a single expression, between a comment and the code it
  introduces, or between a guard and the statement it protects. When no grouping is apparent,
  split the function instead of spacing it.
- **Comments attach downward.** A comment stays with the code it introduces; the blank line goes
  before the comment when it opens a new step.
- **Cohesion and ordering.** Related definitions stay together; phases appear in execution order;
  a helper sits directly above its first caller.
- **Names promise behavior.** Avoid `handle_`, `do_` and `process_` fallbacks; when a comment
  must explain what a helper does, the name is unfinished.
- **Comments say why.** Update or delete a stale comment with its code.
- **Function shape.** Early-return guards precede the main path; extract a phase when it needs a
  name twice.
- **Reviewer checklist, changed lines.** Does each blank line mark a real boundary; does a
  statement sit in the wrong phase; does anything grouped fail to cohere; do names and comments
  still match behavior. A reviewer who asks for a grouping change names the step boundary it
  serves.
- When examples do not decide a case, follow the nearest example and add the new case to this
  guide in the same change. Tie-breaks resolve to fewer blank lines and smaller diffs.

## Mechanical enforcement

`make stylecheck` (part of `make check`) enforces the mechanical subset, full tree:

- the established checks: SPDX line, no carriage returns, no tabs, no trailing whitespace, no
  repeated blank lines, a final newline; the shell SPDX tag; the prompt-cache contract;
- no `//` comments, with string, character-literal and block-comment awareness;
- no blank line directly after a `{`-ending line and none directly before a `}`-only line.

`tools/check_style.sh --changed <base>` adds the changed-lines rules over lines added since the
explicit base: the 100-column hard limit and keyword spacing (`if(`, `for(`, `while(`, `switch(`).
It is opt-in; the base is required and a missing or unresolved base is a usage error, so plain
`make check` stays archive-safe.

Outside the checker: brace placement, general operator spacing, declaration placement, naming,
comments, function shape and grouping are review obligations. Each enforced rule ships with its
measured baseline; a rule that cannot be made false-positive-free stays a review item. Rule-set
changes carry the same authority as this policy. The tool path extends the existing checker
(`tools/check_style.sh`, with `tools/check_style.py` where string or comment awareness is needed);
a formatter pin is a separate decision.

## Other languages

- **Shell**: POSIX `sh`; `set -eu` after the SPDX comment; no tabs, four-space indent; quoted
  expansions; errors to stderr with a non-zero exit.
- **Python**: PEP 8 and the repo's habits; four-space indent; imports at the top, stdlib before
  local; no wildcard imports; bare `except:` is banned; `is None`; `if __name__ == "__main__":`
  for tool entry points.
- **Nix**: nixpkgs-style two-space indentation; no tabs; `lib`-qualified helpers; no top-level
  `with`; `#` comments above the term they describe.

## Examples

`src/office.c:30` - the minified dialect, then the conforming form:

```c
/* before */
char *snag_office_runtime(const char *program,const char *root)
{
    if(!root || !*root)return NULL;
    if(snag_path_root_len(root))return snag_strdup_checked(root,SNAG_PATH_MAX_BYTES);
    if(!program || !snag_path_root_len(program))return NULL;
    char *dir=snag_strdup_checked(program,SNAG_PATH_MAX_BYTES);if(!dir)return NULL;
    snag_path_slashes(dir);
    char *last=strrchr(dir,'/');
    if(!last) {free(dir);return NULL;}
    size_t root_len=snag_path_root_len(dir);
    if((size_t)(last-dir)<root_len)dir[root_len]=0;else *last=0;
    char *path=snag_path_join(dir,root);free(dir);return path;
}

/* after */
char *snag_office_runtime(const char *program, const char *root)
{
    char *dir;
    char *last;
    size_t root_len;
    char *path;

    if (!root || !*root) return NULL;
    if (snag_path_root_len(root)) return snag_strdup_checked(root, SNAG_PATH_MAX_BYTES);
    if (!program || !snag_path_root_len(program)) return NULL;

    dir = snag_strdup_checked(program, SNAG_PATH_MAX_BYTES);
    if (!dir) return NULL;
    snag_path_slashes(dir);

    last = strrchr(dir, '/');
    if (!last) {
        free(dir);
        return NULL;
    }

    root_len = snag_path_root_len(dir);
    if ((size_t)(last - dir) < root_len) {
        dir[root_len] = 0;
    } else {
        *last = 0;
    }

    path = snag_path_join(dir, root);
    free(dir);
    return path;
}
```

`src/term.c` - grouping three steps, with a why-comment on the first:

```c
/* before */
    *action = SNAG_TERM_NONE;
    *text = NULL;
    if (!term->input_only && term->cancel_pending) {
        term->cancel_pending = false;
        if (snag_term_hide(term) < 0 || snag_term_write(STDERR_FILENO, "\n^C\n", 4u) < 0) return -1;
        clear_output_baseline(term);
    }
    if (consume_resize(term) < 0 || flush_completions(term) < 0) return -1;

/* after */
    *action = SNAG_TERM_NONE;
    *text = NULL;

    /* A cancelled line is echoed once, then the output baseline restarts. */
    if (!term->input_only && term->cancel_pending) {
        term->cancel_pending = false;
        if (snag_term_hide(term) < 0 ||
            snag_term_write(STDERR_FILENO, "\n^C\n", 4u) < 0)
            return -1;
        clear_output_baseline(term);
    }

    /* Pre-wait maintenance: resize, completions, spinner, hold deadline. One phase. */
    if (consume_resize(term) < 0 || flush_completions(term) < 0) return -1;
```

Micro examples:

```c
if(!reported)reported=snag_buf_printf(&usage_text, ...);       /* before */
if (!reported) reported = snag_buf_printf(&usage_text, ...);   /* after  */

return snag_fail(error, error_size, EINVAL, "invalid provider endpoint ...");  /* before: over the limit */
return snag_fail(error, error_size, EINVAL,
                 "invalid provider endpoint ...");                            /* after: wrapped */

// retry once when the provider races       /* before, banned */
/* retry once when the provider races */    /* after */
```

A long condition, already the tree's model (`src/store.c`):

```c
if (snag_json_integer_u64(ref, begin[s], &from) < 0 ||
    snag_json_integer_u64(ref, end[s], &to) < 0 ||
    from != process->collected_bytes[s] || to != process->output_bytes[s]) {
    clause = "output-ref-bounds";
    goto invalid;
}
```
