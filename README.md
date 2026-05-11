# escp — Epson ESC/P printer control utility

`escp` is a small Unix-style filter that emits **ESC/P** and **ESC/P 2**
control sequences for Epson-compatible dot-matrix printers.  It is
designed for the **PSI PP-404** but works with any printer that
understands the standard Epson command set (LQ-series 24-pin printers,
many label printers, RPI receipt printers, etc.).

The tool follows the classic Unix "do one thing well" philosophy: it
writes bytes to stdout (or to a file) and is intended to live in a
pipeline that turns plain text into a print-ready stream.

```sh
# Initialise the printer, switch to 12 cpi, bold, print a file,
# eject the page and send everything to /dev/lp0.
{
    escp --init --cpi 12 --bold on
    cat report.txt
    escp --bold off --ff
} > /dev/lp0
```

## Build and install

There are no third-party dependencies — just a C99 compiler and
`make`.

```sh
make                       # builds bin/escp
make test                  # runs the byte-level test suite
sudo make install          # installs to /usr/local/bin and man1
```

`PREFIX`, `DESTDIR`, `CC`, `CFLAGS`, `LDFLAGS` can all be overridden
on the make command line in the usual way.

Tested on macOS (clang), Linux (gcc/clang) and the BSDs.

## Quick reference

Run `escp --help` for the full option list, or read the man page
installed at `${PREFIX}/share/man/man1/escp.1`.

| Need                          | Option                           |
|-------------------------------|----------------------------------|
| Reset printer                 | `--init` / `-i` / `--reset`      |
| 10/12/15 cpi                  | `--pica`, `--elite`, `--cpi 15`  |
| Bold on/off                   | `--bold on` / `--bold off`       |
| Underline / italic            | `--underline on` / `--italic on` |
| Switch to NLQ draft           | `--quality draft`                |
| Pick a font                   | `--font courier`                 |
| Form-feed                     | `--ff`                           |
| Send arbitrary bytes          | `--raw 1B40` or `--raw "1B 40"`  |
| Inline literal text           | `--text "Hello"`                 |

Options are emitted in command-line order, so the byte stream reflects
exactly what you typed.

## Supported commands

See [docs/commands.md](docs/commands.md) for the full table of every
ESC/P command implemented, each mapped to its CLI option and verified
against Appendix F of the PSI PP-404 Operator's Manual.

## Examples

Plain-text printing with a bold header:

```sh
{
    escp --init --cpi 10
    escp --bold on --text "INVOICE 0042" --bold off --lf --lf
    cat invoice.txt
    escp --ff
} | lp -d pp404
```

Switch to compressed printing and 8 lpi for a long log dump:

```sh
{ escp --init --cpi 15 --lpi 8 ; cat /var/log/messages ; escp --ff; } > /dev/usb/lp0
```

Send a hand-coded escape sequence (here, the page-eject for the
PP-404's automatic sheet feeder):

```sh
escp --raw "1B 5B 3C 73"
```

## License

See [LICENSE](LICENSE).
