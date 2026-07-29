# escpmd — Markdown → ESC/P mapping

Every Markdown construct that `escpmd` understands maps to a fixed
ESC/P or ESC/P 2 byte sequence. The table below is the contract: each
row is also covered by a test case under `tests/md-cases/`.

All sequences are produced through the shared `escp_codes` module —
the same one `escp(1)` uses — so behaviour is bit-identical when you
chain the two tools.

## Document framing

| When                | Bytes emitted                                                                      |
|---------------------|------------------------------------------------------------------------------------|
| Document start      | `ESC @`  +  body style: `ESC x q`, pitch, `ESC k n` (font), `ESC <lpi>`, margins   |
| Document end        | `FF` + `ESC @`                                                                     |
| Between input files | `FF`                                                                               |

The body prologue and trailing FF can be suppressed with `--no-init`
and `--no-ff` respectively, so that `escpmd` can be embedded inside a
pipeline that manages the printer state itself (typically via
`escp(1)`).

## Block constructs

| Markdown               | ESC/P translation                                                                      |
|------------------------|----------------------------------------------------------------------------------------|
| Paragraph break        | `LF LF`                                                                                |
| Soft / hard line break | `LF`                                                                                   |
| Heading H1             | `ESC W 1` + `ESC w 1` + `ESC E` … `LF` + `ESC F` + `ESC w 0` + `ESC W 0`               |
| Heading H2             | `ESC W 1` + `ESC E` … `LF` + `ESC F` + `ESC W 0`                                       |
| Heading H3             | `ESC E` + `ESC - 1` … `LF` + `ESC - 0` + `ESC F`                                       |
| Heading H4–H6          | `ESC E` … `LF` + `ESC F`                                                               |
| Bullet list item       | indent (2 spaces per nest) + `"* "` + body + `LF`                                      |
| Ordered list item      | indent + `"N. "` (counter per nest level) + body + `LF`                                |
| Block quote            | `ESC 4` … body … `LF` + `ESC 5`                                                        |
| Thematic break (`---`) | 72 × `-` + `LF`                                                                        |
| Fenced code block      | `ESC k 02` + `SI` + `ESC x 0` … `LF`-terminated lines … `ESC x 1` + `DC2` + `ESC k 00` |
| HTML block             | dropped                                                                                |
| Table                  | Courier font; column-aligned; word-wrapped; headers bold; auto-sized to fit page   |

## Inline constructs

| Markdown        | ESC/P translation                                                                                                                                                                                                       |
|-----------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `**strong**`    | `ESC E` … `ESC F` (bold)                                                                                                                                                                                                |
| `*emphasis*`    | `ESC 4` … `ESC 5` (italic)                                                                                                                                                                                              |
| `~~strike~~`    | `ESC - 1` … `ESC - 0` (underline — ESC/P has no strikethrough)                                                                                                                                                          |
| `` `code` ``    | `ESC k 02` + `SI` … `DC2` + `ESC k 00`                                                                                                                                                                                  |
| `[text](url)`   | `ESC - 1` + text + `ESC - 0` (underlined visible text only; pass `--link-urls` to also append `" (url)"`)                                                                                                              |
| `<https://x>`   | `ESC - 1` + url + `ESC - 0`                                                                                                                                                                                             |
| `![alt](src)`   | `ESC 4` + alt + `ESC 5`                                                                                                                                                                                                 |
| Entity `&amp;`  | decoded to `&` (plus `&lt;`, `&gt;`, `&quot;`, `&apos;`, `&nbsp;`, `&copy;`, `&reg;`, `&trade;`, `&hellip;`, `&mdash;`, `&ndash;`, `&lsquo;`, `&rsquo;`, `&ldquo;`, `&rdquo;`, `&bull;`, and numeric `&#NNN;`/`&#xHH;`) |
| Raw inline HTML | dropped                                                                                                                                                                                                                 |

## Margins and word-wrap

By default `escpmd` does not wrap paragraphs in software — long lines
flow to the printer unbroken and the printer's own hardware wrap (if a
right margin was emitted) takes care of any overflow.

Pass `--page-width N` (or set both `--left-margin` and `--right-margin`,
in which case the printable width is taken as `right - left`) to
enable software word-wrap. With a known width:

- Paragraphs wrap at word boundaries: the space that triggered the
  wrap is dropped and replaced by `LF`.
- The thematic break rule scales to fill the printable width instead
  of the hard-coded 72-char default.
- List items push a continuation indent (the visible width of the
  bullet plus its preceding indent) — so a wrapped bullet line carries
  on under the text column, not under the bullet itself.
- Inline style spans persist across the wrap: `**foo bar**` wrapping
  between the two words still emits a single `ESC E … ESC F` pair, so
  bold stays on for `bar` on the second line.
- Words that are *individually* longer than the printable width are
  emitted intact — there is no force-break inside a word.

Hardware margins (`ESC l`, `ESC Q`) are still written during the
prologue when `--left-margin` / `--right-margin` are given, as a
safety net for the printer firmware even when software wrap is also in
effect.

## Style nesting

Bold, italic, underline, courier, condensed and draft-quality are all
implemented as **counters**, not toggles. Pushing the style increments
the counter; popping decrements it; the corresponding ESC sequence is
only emitted when the counter crosses zero. This means `**bold *and
italic* end**` correctly leaves bold on across the inner italic span:

```
ESC E "bold " ESC 4 "and italic" ESC 5 " end" ESC F
```

## Table rendering and auto-sizing

Tables are **buffered and measured** before emission so columns can be
properly aligned and the table can be auto-sized to fit the page width.

**Courier font:**
- All tables are rendered in Courier font (`ESC k 02`)
- The original body font is restored after the table (`ESC k 00`)

**Column alignment:**
- Each column is measured to find its maximum cell width
- Column widths are constrained to a maximum of 30 characters
- Cells are padded with spaces to align to their column width
- Columns are separated with `" | "` (3 characters)
- Header cells are rendered in bold

**Word wrapping:**
- Cell content that exceeds the column width is automatically wrapped
- Wrapping occurs at word boundaries
- Multi-line cells create multi-line rows
- All cells in a row align vertically across wrapped lines

**Auto-sizing strategy:**

When a `--page-width` or margin-derived width is known, `escpmd`
automatically adjusts the table style to fit:

1. Try current CPI (no changes)
2. If too wide: enable condensed mode (`SI`)
3. If still too wide: switch to 12 CPI
4. If still too wide: switch to 12 CPI + condensed
5. If still too wide: switch to 15 CPI
6. If still too wide: switch to 15 CPI + condensed (and let it overflow)

After the table, the original CPI and condensed state are restored.

**Without a page width:**
Tables are sized for wide-carriage printer assumptions (10 CPI = 132 columns,
12 CPI = 158 columns, 15 CPI = 198 columns). Set `--page-width` or margins
to override for narrow-carriage printers.

## UTF-8 → target encoding (iconv)

Markdown is UTF-8; ESC/P printers are not. `escpmd` opens a single
`iconv(3)` descriptor at startup and converts every byte that goes to
the printer:

    iconv_open("<charset>//TRANSLIT", "UTF-8")
    # falls back to plain "<charset>" if //TRANSLIT is unavailable

The default target is **ISO-8859-1**, which covers Western European
text without further work. Override with `--charset NAME` — anything
your platform's `iconv -l` lists, e.g. `ISO-8859-15`, `ASCII`,
`CP437`, `MAC`, `KOI8-R`.

Thanks to `//TRANSLIT`, smart quotes, em/en dashes, ellipsis, NBSP and
similar typographic codepoints are folded down to safe equivalents
automatically — there is no longer a hand-coded transliteration table.
Anything still un-mappable becomes a `?`.

If you need a richer character set, also set the printer's character
table with `escp --country N` / `escp --charset N` before the document
and use `--no-init` so `escpmd` does not overwrite your prologue.

## Emoji shortcodes (`--smileys`)

Off by default. With `--smileys`, GitHub-style `:name:` shortcodes are
rewritten to an ASCII equivalent **before** the iconv pass. Unknown
shortcodes are passed through untouched, so `:not_a_real_one:` stays
intact and only the curated subset below changes:

| Shortcode                          | ASCII      |
|------------------------------------|------------|
| `:smile:` `:slight_smile:`         | `:)`       |
| `:smiley:` `:grin:` `:grinning:`   | `:D`       |
| `:wink:`                           | `;)`       |
| `:laughing:`                       | `xD`       |
| `:joy:` `:sob:` `:cry:`            | `:'(` / `:'D` |
| `:heart:`                          | `<3`       |
| `:thumbsup:` `:+1:`                | `(y)`      |
| `:thumbsdown:` `:-1:`              | `(n)`      |
| `:ok:` `:ok_hand:`                 | `(ok)`     |
| `:check:` `:cross:` `:x:`          | `[x]`      |
| `:warning:`                        | `[!]`      |
| `:rocket:`                         | `==>`      |
| `:point_left:` `:arrow_left:`      | `<-`       |
| `:point_right:` `:arrow_right:`    | `->`       |
| `:tada:` `:party:` `:hooray:`      | `\o/`      |
| `:wave:`                           | `o/`       |
| `:sunglasses:`                     | `B)`       |
| `:stuck_out_tongue:` `:tongue:`    | `:P`       |
| `:rage:`                           | `>:O`      |
| `:zzz:`                            | `Zzz`      |
| …                                  | (see `src/escpmd.c` for the full table) |
