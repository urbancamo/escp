# escpmd — Implementation Plan

`escpmd` is a Unix-style filter that reads Markdown (CommonMark subset) and
emits an ESC/P 2 byte stream suitable for piping straight to a dot-matrix
printer. It is a sibling of `escp`: where `escp` is a low-level emitter of
individual control codes, `escpmd` interprets a document.

## Implementation Stages

1. **Refactor `escp` to expose a reusable ESC/P core.**
   Extract the byte-emission primitives and the command-encoding helpers
   from `src/escp.c` into a new library module shared by both programs:
   - `src/escp_codes.h` — public API
   - `src/escp_codes.c` — implementation
   The existing `escp` CLI becomes a thin wrapper that translates options
   into calls on this API. The new `escpmd` program links against the
   same object. The existing test suite (`make test`) must still pass
   unchanged after the refactor.

2. **Define the shared `escp_codes` API.**
   Functions are stateless emitters that take a `FILE *` so callers
   control output destination. Cover at minimum:
   - byte/word/parameter-prefix helpers (`escp_emit`, `escp_emit_word`,
     `escp_emit_escp2_word`)
   - initialisation / reset (`escp_init`)
   - pitch and font (`escp_set_cpi`, `escp_set_font`, `escp_set_quality`,
     `escp_set_proportional`)
   - style toggles (`escp_bold`, `escp_italic`, `escp_underline`,
     `escp_double_strike`, `escp_double_width`, `escp_double_height`,
     `escp_script`, `escp_style`, `escp_master`)
   - layout (`escp_lpi`, `escp_line_spacing_*`, `escp_left_margin`,
     `escp_right_margin`, `escp_page_lines`, `escp_skip_perf`)
   - control bytes (`escp_lf`, `escp_cr`, `escp_ff`, `escp_ht`, …)
   - character set (`escp_charset`, `escp_country`)

3. **Choose the Markdown parser.**
   Use **md4c** (https://github.com/mity/md4c) — single-source, MIT-licensed,
   C99, no runtime deps, CommonMark + GFM extensions, callback-driven so it
   maps naturally onto ESC/P's stream model. Vendor the two source files
   into `src/vendor/md4c/` so portability is preserved on macOS, Linux and
   the BSDs without external package installs.

4. **Design the Markdown → ESC/P mapping.**
   Document the mapping in `docs/escpmd-mapping.md`. Initial mapping:

   | Markdown element       | ESC/P translation                                     |
   |------------------------|-------------------------------------------------------|
   | Document start         | `ESC @` (init), `ESC x 1` (LQ), `ESC k 0` (Roman)     |
   | Document end           | `FF` (form feed) and `ESC @`                          |
   | Paragraph break        | `LF LF`                                               |
   | Soft line break        | `LF`                                                  |
   | Hard line break        | `LF`                                                  |
   | Heading H1             | double-width + double-height + bold, then cancel + LF |
   | Heading H2             | double-width + bold                                   |
   | Heading H3             | bold + underline                                      |
   | Heading H4–H6          | bold                                                  |
   | `**strong**`           | `ESC E` … `ESC F`                                     |
   | `*emphasis*`           | `ESC 4` … `ESC 5` (italic)                            |
   | `~~strike~~`           | ESC/P has no strikethrough; render with underline     |
   | `` `code` ``           | switch to Courier + condensed, then restore           |
   | Fenced code block      | Courier + condensed + draft; LF on each line          |
   | Bullet list item       | `"  • "` prefix per item, hanging indent              |
   | Ordered list item      | `"  N. "` prefix per item                             |
   | Block quote            | `"> "` prefix; italic body                            |
   | Thematic break (`---`) | line of `-` characters at current cpi, then LF        |
   | Link `[t](u)`          | print text; URL in parentheses after if not identical |
   | Image                  | print alt text in italic                              |
   | Tables (GFM)           | render as plain aligned columns using current cpi     |
   | HTML / raw inline      | pass through as text                                  |

   The mapping is implemented inside `escpmd`, calling `escp_codes`
   functions only — `escpmd` itself never writes raw ESC sequences.

5. **CLI surface.**
   ```
   escpmd [OPTIONS] [FILE...]

       -o, --output FILE   write to FILE instead of stdout
       -i, --no-init       skip the initialise prologue
       -F, --no-ff         skip the trailing form-feed
           --cpi N         body text pitch (10|12|15, default 10)
           --font NAME     body font (default roman)
           --quality Q     draft|lq (default lq)
           --lpi N         lines per inch (default 6)
           --left-margin N
           --right-margin N
       -h, --help
       -V, --version
   ```
   With no `FILE` arguments, read Markdown from stdin. With multiple files,
   concatenate them with a form-feed between documents.

6. **Implement `escpmd`.**
   - `src/escpmd.c` — argument parsing, file slurping, md4c invocation
   - State machine fed by md4c block/span enter/leave callbacks; keeps a
     small style stack so nested constructs (e.g. bold inside a heading)
     unwind correctly.
   - Output buffering: write straight to the `FILE *` chosen by `-o`.
   - UTF-8 input is transliterated to the active character set with a
     conservative ASCII fallback (`?` for unmapped code points), mirroring
     the `iconv … //TRANSLIT//IGNORE` step used in the example pipelines.

7. **Makefile updates.**
   Add an `escpmd` target alongside `escp`:
   - new objects: `src/escp_codes.o`, `src/escpmd.o`, `src/vendor/md4c/*.o`
   - both binaries depend on `escp_codes.o`
   - `make test` runs both test suites; `make install` installs both
     binaries and both man pages.

8. **Tests.**
   - Unit-style: extend `tests/cases.txt` framework with a parallel
     `tests/md-cases/` directory of `*.md` → `*.expected-hex` pairs.
     Each case feeds the markdown through `escpmd` and compares the byte
     output against a hand-verified hex literal.
   - Cover every row of the mapping table at least once.
   - Behavioural: `--help` exits 0, no-arg with no stdin exits non-zero,
     `-o` writes to file, multi-file concatenation inserts a form-feed,
     `--no-init`/`--no-ff` suppress prologue/epilogue.

9. **Documentation.**
   - `docs/escpmd.1` — man page in the same style as `docs/escp.1`
   - `docs/escpmd-mapping.md` — the full mapping table from stage 4,
     annotated with the exact byte sequences each Markdown element emits
   - update `README.md` with an `escpmd` quick-reference section and a
     piped example, e.g.
     ```sh
     escpmd README.md | lp -d pp404
     ```

10. **UTF-8 → ISO-8859-1 via iconv.**
    Replace the hand-coded transliteration table with **POSIX `iconv(3)`**
    (available in libc on macOS, glibc and the BSDs; no extra package or
    Makefile flag required on any of our supported platforms).  At
    startup `escpmd` opens one descriptor with
    `iconv_open("ISO-8859-1//TRANSLIT", "UTF-8")`, falling back to plain
    `"ISO-8859-1"` if the platform's iconv does not understand
    `//TRANSLIT`.  Every text chunk handed to the renderer is converted
    in place to ISO-8859-1 bytes before being written to the output
    stream; un-mappable codepoints become `?`.
    - new option: `--charset` lets callers pick a different target
      encoding (e.g. `ISO-8859-15`, `ASCII`, `CP437`).  Default is
      `ISO-8859-1`.
    - the existing `:emit_codepoint:` lookup table is removed; entities
      are decoded to a UTF-8 byte sequence first, then run through the
      same iconv pipeline so behaviour is consistent.
    - add `tests/md-cases/iso-8859-1.md` exercising accented Latin-1
      letters, smart quotes (TRANSLIT'd) and a deliberately unmappable
      codepoint (mapped to `?`).

13. **Margin-aware software layout.**
    Until now `--left-margin` / `--right-margin` only emit `ESC l`
    / `ESC Q` to the printer at startup; nothing inside escpmd knows
    about them.  Add a software-side layer that:
    - derives a `page_width` from `right - left` when both are set,
      or from a new explicit `--page-width N` option;
    - routes every byte (visible text *and* style ESC sequences) through
      a single line-buffered emitter that holds the in-progress word
      back, then either emits it on the current line, or breaks at the
      preceding word boundary and re-emits a continuation indent;
    - drops the space that would have triggered a wrap (it becomes the
      `LF`);
    - keeps style spans intact across wrap points by buffering ESC
      sequences with the word they precede;
    - scales the thematic-break rule to `page_width` instead of the
      hard-coded 72;
    - tracks a continuation-indent stack so wrapped list-item lines
      align under the bullet's content column.
    Hardware margins are still emitted in the prologue as a safety
    net so the printer firmware enforces them too.

    New test fixtures: `wrap-paragraph`, `wrap-hr`, `wrap-bullet`,
    `wrap-bold-span`, `wrap-from-margins`, `margins-hardware`.

12. **Link rendering: underlined text by default.**
    Per the updated requirement, plain Markdown links `[text](url)`
    render as the visible text only, in underline — the URL is not
    written by default. Add an opt-in `--link-urls` flag that restores
    the original behaviour of appending `" (url)"` after the underlined
    text.  Autolinks (`<https://x>`) have no separate visible text, so
    they always print as the underlined URL.
    - update `tests/md-cases/link.hex` to the new default
    - update `tests/md-cases/autolink.hex` to underline the URL
    - add `tests/md-cases/link-urls.md` covering the `--link-urls` flag

11. **Emoji shortcode expansion (`--smileys`).**
    GitHub's `gemoji` list is the de-facto standard for `:name:` style
    shortcodes (used by GitHub, Discord, Slack, Matrix, Mastodon …).
    There is no IETF/ISO standard, so we bake in a curated subset of the
    ~50 most common shortcodes that have a natural ASCII rendering
    (`:smile:` → `:)`, `:wink:` → `;)`, `:heart:` → `<3`, `:thumbsup:`
    → `(y)`, `:check:` → `[x]`, …) — full table in
    `docs/escpmd-mapping.md`.  Expansion is opt-in via `--smileys`.
    - implementation: scan each `MD_TEXT_NORMAL` chunk for
      `/:[A-Za-z0-9_+-]+:/`, look the name up in a sorted static table,
      and substitute the ASCII string.  Unknown shortcodes are left
      untouched, so e.g. `:not_a_real_shortcode:` survives intact.
    - happens before the iconv pass (ASCII is a strict subset of
      ISO-8859-1 so this is safe).
    - new `tests/md-cases/smileys.md` covering known shortcodes,
      unknown shortcodes and one occurring next to ordinary punctuation.
