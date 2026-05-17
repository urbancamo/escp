# escpmd feature showcase

This document exercises every GitHub-Flavoured Markdown construct that
`escpmd` understands. Pipe it through the binary to confirm a build
renders the whole feature set without crashing or skipping anything:

```sh
escpmd --page-width 80 docs/escpmd-features.md | lp -d pp404
# or, to inspect the byte stream first:
escpmd --no-init --no-ff --page-width 80 docs/escpmd-features.md | hexdump -C
```

## Headings

The six ATX heading levels each map to a distinct printer state.
Levels 1 and 2 use double-width; level 1 is also double-height.

# Heading level 1 — double-width + double-height + bold

## Heading level 2 — double-width + bold

### Heading level 3 — bold + underline

#### Heading level 4 — bold

##### Heading level 5 — bold

###### Heading level 6 — bold

## Paragraphs and line breaks

A paragraph is one or more lines of text separated from the next
paragraph by a blank line. Soft line breaks inside a paragraph
are flattened to a single space by md4c's `COLLAPSEWHITESPACE` flag.

Two spaces at the end of a line force a hard line break.  
Here the previous line ended with two trailing spaces, so this text
begins on a new printed line within the same paragraph.

## Inline emphasis

You can render text as *italic*, **bold**, ***bold and italic***,
~~strikethrough~~ (rendered with underline since ESC/P has no
strikethrough), and `inline code` (Courier + condensed).

Inline styles nest correctly: **bold containing *italic* inside**,
*italic with `code` inside*, and **bold with ~~strike~~ inside**.

## Code

Inline code: `printf("hello, world\n");` switches to Courier and
condensed pitch and back.

Fenced code blocks switch to Courier + condensed + draft quality for
the duration of the block:

```c
#include <stdio.h>

int main(void) {
    for (int i = 0; i < 3; i++)
        printf("line %d\n", i);
    return 0;
}
```

Indented code blocks (four leading spaces) work the same way:

    $ make
    $ make test
    $ sudo make install

Language tags on fences (` ```python `) are accepted but currently
ignored — the rendering does not change per language.

## Lists

Unordered tight list:

- one
- two
- three

Unordered loose list (blank lines between items):

- alpha

- beta

- gamma

Ordered list (counter resumes from whatever start you give):

1. first item
2. second item
3. third item

Nested lists indent by two spaces per level:

- outer
  - inner
    - deepest
  - back to inner
- second outer

Task lists (GFM extension — the `[ ]` / `[x]` is treated as literal text):

- [ ] open task
- [x] completed task
- [ ] another open task

## Block quotes

Block-quoted text is rendered in italic for the whole quote:

> The good news is that as Liberal Arts colleges and Music Schools
> close all over the country, all of the high-end Steinway grand
> pianos are being sold off cheap.
>
> — Edward Tufte

## Thematic break

A line of three or more `-`, `*` or `_` characters becomes an HR.
With software wrap enabled the rule scales to the printable width;
otherwise it prints a fixed 72 dashes.

---

## Links

Default rendering: only the visible text, underlined.

Look at the [escp project on GitHub](https://github.com/anthropics/escp).

Autolinks (no separate text) print the URL underlined:

See <https://example.org/api/v1> for details, or write to
<solutions@agilemindswork.uk>.

Pass `--link-urls` on the command line to also write ` (url)` after
the underlined text for explicit references.

## Images

Image alt text is rendered in italic; the source path is not printed.

![PSI PP-404 calibration sheet](pp404-cal.png)

## Tables

GFM tables render as plain text columns with `" | "` separators; the
header row is bold:

| Pitch | Speed (LQ) | Use case                |
|-------|-----------:|-------------------------|
| 10    | 33 cps     | normal correspondence   |
| 12    | 40 cps     | letters, drafts         |
| 15    | 50 cps     | spreadsheets, listings  |

## Smiley shortcodes (with `--smileys`)

Pass `--smileys` to expand a curated subset of GitHub gemoji into
ASCII smileys:

:smile: hello world  
:wink: keep an eye on this  
:warning: be careful  
:thumbsup: looks good  
:heart: open source  
:rocket: ship it

Without `--smileys`, those `:name:` tokens pass through untouched.

## Entities and special characters

Named HTML entities are decoded: &amp; &lt; &gt; &copy; &reg; &trade;
&hellip; &mdash; &ndash; &ldquo;hello&rdquo; &nbsp; &bull;

Numeric entities work too: &#65; (= A), &#x2014; (= em-dash).

Unicode in the source is mapped to the active charset (ISO-8859-1
by default) via iconv `//TRANSLIT`:

- accented Latin-1: café, naïve, façade, jalapeño
- smart punctuation: "double" 'single' — em — en –
- ellipsis…
- copyright © registered ® trademark ™
- bullet • middle-dot ·
- everything outside the target charset becomes `?` (so e.g. CJK 中文
  prints as `??`)

## HTML passthrough

Raw HTML blocks and inline HTML are dropped — escpmd is a text-mode
filter, not a browser. <span style="color:red">This inline HTML
becomes plain text without the tags.</span>

## End of document

If you reached this line on paper, every supported construct rendered
without aborting. The final `FF` should advance to the next sheet
unless you invoked the program with `--no-ff`.
