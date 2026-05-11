# Supported ESC/P Commands

This table lists the ESC/P / ESC/P2 commands implemented by the `escp` utility.
Every command is verified against Appendix F (EPSON LQ 2550 and ESC/P2 Quick
Reference) of the PSI PP-404 Operator's Manual, so the output is guaranteed to
be understood by a PP-404 in its EPSON emulation mode.  The utility is also
compatible with any other printer that speaks ESC/P or ESC/P2 (Epson LQ
series, FX series in the overlapping subset, etc.).

`P1`, `P2`, … denote numeric parameters that are emitted as **raw bytes**
(not as ASCII digits).  Bytes are shown in hex.

## 1. Single-byte control codes

| Hex | Mnemonic | Function                             | CLI option                   |
|-----|----------|--------------------------------------|------------------------------|
| 08  | BS       | Backspace                            | `--bs`                       |
| 09  | HT       | Horizontal tab                       | `--ht`                       |
| 0A  | LF       | Line feed                            | `--lf`                       |
| 0B  | VT       | Vertical tab                         | `--vt`                       |
| 0C  | FF       | Form feed                            | `--ff`                       |
| 0D  | CR       | Carriage return                      | `--cr`                       |
| 0E  | SO       | Select double-width (one line)       | `--double-width-line`        |
| 0F  | SI       | Select condensed printing            | `--condensed`                |
| 11  | DC1      | Select printer (on-line)             | `--select`                   |
| 12  | DC2      | Cancel condensed printing            | `--cancel-condensed`         |
| 13  | DC3      | Deselect printer (off-line)          | `--deselect`                 |
| 14  | DC4      | Cancel double-width (one line)       | `--cancel-double-width-line` |
| 18  | CAN      | Cancel buffered data on current line | `--cancel`                   |
| 7F  | DEL      | Delete last character in buffer      | `--del`                      |

## 2. Initialisation and printer state

| Sequence       | Function                            | CLI option        |
|----------------|-------------------------------------|-------------------|
| `1B 40`        | Initialise printer (`ESC @`)        | `--init`, `-i`    |
| `1B 3D`        | Set MSB to 0 (`ESC =`)              | `--msb-0`         |
| `1B 3E`        | Set MSB to 1 (`ESC >`)              | `--msb-1`         |
| `1B 23`        | Cancel MSB control (`ESC #`)        | `--msb-cancel`    |

## 3. Pitch and font

| Sequence  | Function                           | CLI option               |
|-----------|------------------------------------|--------------------------|
| `1B 50`   | Select 10 cpi (Pica)               | `--pica`, `--cpi 10`     |
| `1B 4D`   | Select 12 cpi (Elite)              | `--elite`, `--cpi 12`    |
| `1B 67`   | Select 15 cpi                      | `--cpi 15`               |
| `1B 0F`   | Condensed (squeezes current pitch) | `--condensed`            |
| `1B 70 n` | Cancel/select proportional         | `--proportional on\|off` |
| `1B 6B n` | Select font family                 | `--font NAME`            |
| `1B 78 n` | Select draft / LQ                  | `--quality draft\|lq`    |

`--font` accepts `roman`, `sans` (Letter Gothic), `courier`, `prestige`,
`script`, `ocr-b`, `ocr-a`, `orator-c`, `orator`, `data-block`, `data-large`.
The numeric `n` is taken from Appendix F Table 5.

## 4. Style attributes

| Sequence  | Function                                | CLI option                |
|-----------|-----------------------------------------|---------------------------|
| `1B 45`   | Bold (emphasised) on                    | `--bold on`               |
| `1B 46`   | Bold off                                | `--bold off`              |
| `1B 47`   | Double-strike on                        | `--double-strike on`      |
| `1B 48`   | Double-strike off                       | `--double-strike off`     |
| `1B 34`   | Italic on                               | `--italic on`             |
| `1B 35`   | Italic off                              | `--italic off`            |
| `1B 2D n` | Underline on (`n=1`) / off (`n=0`)      | `--underline on\|off`     |
| `1B 53 n` | Superscript (`n=0`) / Subscript (`n=1`) | `--script super\|sub`     |
| `1B 54`   | Cancel super/subscript                  | `--script none`           |
| `1B 57 n` | Double width (`n=1` on, `n=0` off)      | `--double-width on\|off`  |
| `1B 77 n` | Double height (`n=1` on, `n=0` off)     | `--double-height on\|off` |
| `1B 71 n` | Character style (normal/outline/shadow) | `--style NAME`            |
| `1B 21 n` | Master select (bit-mask print mode)     | `--master N`              |

`--style` accepts `normal`, `outline`, `shadow`, `outline-shadow`.
`--master` takes a decimal 0–255 that is sent verbatim as the bit-mask.

## 5. Page layout

| Sequence     | Function                                | CLI option             |
|--------------|-----------------------------------------|------------------------|
| `1B 30`      | Line spacing 1/8"                       | `--line-spacing 1/8`   |
| `1B 32`      | Line spacing 1/6"                       | `--line-spacing 1/6`   |
| `1B 33 n`    | Line spacing `n`/180"                   | `--line-spacing-180 N` |
| `1B 2B n`    | Line spacing `n`/360"                   | `--line-spacing-360 N` |
| `1B 41 n`    | Line spacing `n`/60"                    | `--line-spacing-60 N`  |
| `1B 4A n`    | Forward feed `n`/180"                   | `--feed-180 N`         |
| `1B 6A n`    | Reverse feed `n`/180"                   | `--reverse-feed-180 N` |
| `1B 43 n`    | Form length `n` lines (1–127)           | `--page-lines N`       |
| `1B 43 00 n` | Form length `n` inches (1–22)           | `--page-inches N`      |
| `1B 4E n`    | Skip-over-perforation `n` lines (1–127) | `--skip-perf N`        |
| `1B 4F`      | Cancel skip-over-perforation            | `--no-skip-perf`       |
| `1B 6C n`    | Left margin column                      | `--left-margin N`      |
| `1B 51 n`    | Right margin column                     | `--right-margin N`     |

## 6. Print position

| Sequence               | Function                              | CLI option  |
|------------------------|---------------------------------------|-------------|
| `1B 24 nL nH`          | Absolute horizontal position (1/60")  | `--h-pos N` |
| `1B 5C nL nH`          | Relative horizontal position (signed) | `--h-rel N` |
| `1B 28 56 02 00 nL nH` | Absolute vertical position (ESC/P2)   | `--v-pos N` |
| `1B 28 76 02 00 nL nH` | Relative vertical position (ESC/P2)   | `--v-rel N` |

## 7. Character set

| Sequence       | Function                                      | CLI option         |
|----------------|-----------------------------------------------|--------------------|
| `1B 36`        | Enable upper-area (128–159) printing          | `--upper-print`    |
| `1B 37`        | Enable upper-area (128–159) controls          | `--upper-control`  |
| `1B 52 n`      | National version (0–15)                       | `--country N`      |
| `1B 74 n`      | Select character table (0–3)                  | `--charset N`      |

## 8. Tabs and direction

| Sequence   | Function                               | CLI option                 |
|------------|----------------------------------------|----------------------------|
| `1B 44 00` | Clear all horizontal tabs              | `--clear-htabs`            |
| `1B 42 00` | Clear all vertical tabs                | `--clear-vtabs`            |
| `1B 55 n`  | Unidirectional on/off                  | `--unidirectional on\|off` |
| `1B 3C`    | Unidirectional, one line               | `--uni-line`               |
| `1B 61 n`  | Justification (left/centre/right/full) | `--justify NAME`           |

## 9. Convenience aggregates

These options expand to multiple commands for ergonomics:

| CLI option | Expands to                                            |
|------------|-------------------------------------------------------|
| `--reset`  | `ESC @`                                               |
| `--cpi N`  | `ESC P`, `ESC M`, `ESC g` per `N`                     |
| `--lpi N`  | Sets line spacing (6 or 8 directly; otherwise N/180") |

## Commands intentionally excluded

The following ESC/P commands are **not** mapped to options because they
require multi-byte raw binary data better suited to a programmatic API than
a CLI flag.  They can still be emitted by passing raw bytes via standard
input or with `--raw HEXSTRING`:

* User-defined characters (`ESC &`)
* Bit-image graphics (`ESC K/L/Y/Z`, `ESC *`, `ESC .`)
* Vertical and horizontal tab tables (`ESC B`, `ESC D` with parameters)
* Bar-code printing (`ESC [ ? 0 h` and friends)
* Macro definition (`ESC :`)
* Page format `ESC ( c`, set unit `ESC ( U`, font by pitch+point `ESC X`

If a future need arises, these can be added without breaking the existing
CLI surface.
