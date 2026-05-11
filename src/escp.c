/*
 * escp - emit Epson ESC/P and ESC/P2 control sequences.
 *
 * Designed for the PSI PP-404 24-wire dot-matrix printer in its
 * EPSON LQ 2550 / ESC/P2 emulation, but works with any printer that
 * understands the standard Epson command set.
 *
 * The utility writes the requested byte sequences to stdout (or to
 * the file given with -o), in the exact order they appear on the
 * command line, so that several invocations can be chained in a
 * shell pipeline:
 *
 *     escp --init --bold on --cpi 12 < text.txt | lp -d matrix
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROG_NAME    "escp"
#define PROG_VERSION "1.0.0"

#define ESC 0x1B
#define NUL 0x00

static FILE *out = NULL;

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", PROG_NAME);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static void emit(const unsigned char *bytes, size_t n)
{
    if (fwrite(bytes, 1, n, out) != n)
        die("write failed: %s", strerror(errno));
}

static void emit1(unsigned char b)               { emit(&b, 1); }
static void emit2(unsigned char a, unsigned char b)
{
    unsigned char buf[2] = { a, b };
    emit(buf, 2);
}
static void emit3(unsigned char a, unsigned char b, unsigned char c)
{
    unsigned char buf[3] = { a, b, c };
    emit(buf, 3);
}

static long parse_long(const char *s, long lo, long hi, const char *name)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        die("%s: not an integer: '%s'", name, s);
    if (v < lo || v > hi)
        die("%s: value %ld out of range [%ld..%ld]", name, v, lo, hi);
    return v;
}

static int parse_on_off(const char *s, const char *name)
{
    if (strcmp(s, "on") == 0 || strcmp(s, "1") == 0)  return 1;
    if (strcmp(s, "off") == 0 || strcmp(s, "0") == 0) return 0;
    die("%s: expected 'on' or 'off', got '%s'", name, s);
    return 0;
}

/* Emit an ESC/P2 4-byte parameter prefix: ESC ( cmd 02 00, then nL nH */
static void emit_escp2_word(unsigned char cmd, long value)
{
    unsigned char buf[7];
    buf[0] = ESC;
    buf[1] = '(';
    buf[2] = cmd;
    buf[3] = 0x02;
    buf[4] = 0x00;
    buf[5] = (unsigned char)(value & 0xFF);
    buf[6] = (unsigned char)((value >> 8) & 0xFF);
    emit(buf, sizeof buf);
}

/* Emit ESC c nL nH for ESC $ and ESC \ */
static void emit_word(unsigned char cmd, long value)
{
    unsigned char buf[4];
    buf[0] = ESC;
    buf[1] = cmd;
    buf[2] = (unsigned char)(value & 0xFF);
    buf[3] = (unsigned char)((value >> 8) & 0xFF);
    emit(buf, sizeof buf);
}

/* --- per-option handlers ------------------------------------------------- */

static void cmd_font(const char *name)
{
    /* Mapping from Appendix F Table 5 of the PP-404 manual. */
    static const struct { const char *name; unsigned char n; } map[] = {
        { "roman",        0 },
        { "sans",         1 },     /* Letter Gothic */
        { "letter-gothic",1 },
        { "courier",      2 },
        { "prestige",     3 },
        { "script",       4 },
        { "ocr-b",        5 },
        { "ocr-a",        6 },
        { "orator-c",     7 },
        { "orator",       8 },
        { "data-block",  10 },
        { "data-large",  11 },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (strcmp(map[i].name, name) == 0) {
            emit3(ESC, 'k', map[i].n);
            return;
        }
    }
    die("--font: unknown family '%s'", name);
}

static void cmd_quality(const char *name)
{
    if (strcmp(name, "draft") == 0)            emit3(ESC, 'x', 0);
    else if (strcmp(name, "lq") == 0
          || strcmp(name, "letter") == 0)      emit3(ESC, 'x', 1);
    else die("--quality: expected 'draft' or 'lq', got '%s'", name);
}

static void cmd_cpi(const char *s)
{
    long n = parse_long(s, 10, 20, "--cpi");
    switch (n) {
        case 10: emit2(ESC, 'P'); break;
        case 12: emit2(ESC, 'M'); break;
        case 15: emit2(ESC, 'g'); break;
        default: die("--cpi: only 10, 12 or 15 are supported (got %ld)", n);
    }
}

static void cmd_script(const char *name)
{
    if (strcmp(name, "super") == 0)        emit3(ESC, 'S', 0);
    else if (strcmp(name, "sub")   == 0)   emit3(ESC, 'S', 1);
    else if (strcmp(name, "none")  == 0)   emit2(ESC, 'T');
    else die("--script: expected super|sub|none, got '%s'", name);
}

static void cmd_style(const char *name)
{
    unsigned char n;
    if      (strcmp(name, "normal")          == 0) n = 0;
    else if (strcmp(name, "outline")         == 0) n = 1;
    else if (strcmp(name, "shadow")          == 0) n = 2;
    else if (strcmp(name, "outline-shadow")  == 0) n = 3;
    else { die("--style: unknown style '%s'", name); return; }
    emit3(ESC, 'q', n);
}

static void cmd_justify(const char *name)
{
    unsigned char n;
    if      (strcmp(name, "left")   == 0) n = 0;
    else if (strcmp(name, "center") == 0) n = 1;
    else if (strcmp(name, "centre") == 0) n = 1;
    else if (strcmp(name, "right")  == 0) n = 2;
    else if (strcmp(name, "full")   == 0) n = 3;
    else { die("--justify: unknown mode '%s'", name); return; }
    emit3(ESC, 'a', n);
}

static void cmd_line_spacing(const char *s)
{
    if (strcmp(s, "1/8") == 0)      emit2(ESC, '0');
    else if (strcmp(s, "1/6") == 0) emit2(ESC, '2');
    else die("--line-spacing: expected '1/6' or '1/8' (got '%s')", s);
}

static void cmd_lpi(const char *s)
{
    long n = parse_long(s, 1, 360, "--lpi");
    if (n == 6)      emit2(ESC, '2');
    else if (n == 8) emit2(ESC, '0');
    else {
        /* generic: set line spacing to (180/n) in 1/180" units */
        long units = 180 / n;
        if (units < 1 || units > 255)
            die("--lpi: %ld lines per inch is not representable", n);
        emit3(ESC, '3', (unsigned char)units);
    }
}

static void cmd_raw(const char *hex)
{
    size_t len = strlen(hex);
    if (len == 0) return;
    /* Allow spaces and 0x prefixes between bytes for readability. */
    unsigned char *buf = malloc(len);
    if (!buf) die("out of memory");
    size_t n = 0;
    const char *p = hex;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ':'))
            p++;
        if (!*p) break;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) {
            free(buf);
            die("--raw: expected pairs of hex digits near '%s'", p);
        }
        char pair[3] = { p[0], p[1], 0 };
        buf[n++] = (unsigned char)strtol(pair, NULL, 16);
        p += 2;
    }
    emit(buf, n);
    free(buf);
}

/* --- option ids ---------------------------------------------------------- */

enum {
    OPT_BS = 256, OPT_HT, OPT_LF, OPT_VT, OPT_FF, OPT_CR,
    OPT_DW_LINE, OPT_CANCEL_DW_LINE,
    OPT_CONDENSED, OPT_CANCEL_CONDENSED,
    OPT_SELECT, OPT_DESELECT, OPT_CANCEL, OPT_DEL,
    OPT_INIT, OPT_RESET,
    OPT_MSB0, OPT_MSB1, OPT_MSB_CANCEL,
    OPT_PICA, OPT_ELITE, OPT_CPI, OPT_PROPORTIONAL,
    OPT_FONT, OPT_QUALITY,
    OPT_BOLD, OPT_DOUBLE_STRIKE, OPT_ITALIC, OPT_UNDERLINE,
    OPT_SCRIPT, OPT_DW, OPT_DH, OPT_STYLE, OPT_MASTER,
    OPT_LINE_SPACING, OPT_LS_180, OPT_LS_360, OPT_LS_60,
    OPT_FEED_180, OPT_REV_FEED_180,
    OPT_PAGE_LINES, OPT_PAGE_INCHES,
    OPT_SKIP_PERF, OPT_NO_SKIP_PERF,
    OPT_LEFT_MARGIN, OPT_RIGHT_MARGIN, OPT_LPI,
    OPT_HPOS, OPT_HREL, OPT_VPOS, OPT_VREL,
    OPT_UPPER_PRINT, OPT_UPPER_CTRL, OPT_COUNTRY, OPT_CHARSET,
    OPT_CLEAR_HTABS, OPT_CLEAR_VTABS,
    OPT_UNIDIRECTIONAL, OPT_UNI_LINE, OPT_JUSTIFY,
    OPT_RAW, OPT_TEXT
};

static const struct option long_opts[] = {
    /* control codes */
    { "bs",                       no_argument,       0, OPT_BS },
    { "ht",                       no_argument,       0, OPT_HT },
    { "lf",                       no_argument,       0, OPT_LF },
    { "vt",                       no_argument,       0, OPT_VT },
    { "ff",                       no_argument,       0, OPT_FF },
    { "cr",                       no_argument,       0, OPT_CR },
    { "double-width-line",        no_argument,       0, OPT_DW_LINE },
    { "cancel-double-width-line", no_argument,       0, OPT_CANCEL_DW_LINE },
    { "condensed",                no_argument,       0, OPT_CONDENSED },
    { "cancel-condensed",         no_argument,       0, OPT_CANCEL_CONDENSED },
    { "select",                   no_argument,       0, OPT_SELECT },
    { "deselect",                 no_argument,       0, OPT_DESELECT },
    { "cancel",                   no_argument,       0, OPT_CANCEL },
    { "del",                      no_argument,       0, OPT_DEL },

    { "init",                     no_argument,       0, OPT_INIT },
    { "reset",                    no_argument,       0, OPT_RESET },
    { "msb-0",                    no_argument,       0, OPT_MSB0 },
    { "msb-1",                    no_argument,       0, OPT_MSB1 },
    { "msb-cancel",               no_argument,       0, OPT_MSB_CANCEL },

    { "pica",                     no_argument,       0, OPT_PICA },
    { "elite",                    no_argument,       0, OPT_ELITE },
    { "cpi",                      required_argument, 0, OPT_CPI },
    { "proportional",             required_argument, 0, OPT_PROPORTIONAL },
    { "font",                     required_argument, 0, OPT_FONT },
    { "quality",                  required_argument, 0, OPT_QUALITY },

    { "bold",                     required_argument, 0, OPT_BOLD },
    { "double-strike",            required_argument, 0, OPT_DOUBLE_STRIKE },
    { "italic",                   required_argument, 0, OPT_ITALIC },
    { "underline",                required_argument, 0, OPT_UNDERLINE },
    { "script",                   required_argument, 0, OPT_SCRIPT },
    { "double-width",             required_argument, 0, OPT_DW },
    { "double-height",            required_argument, 0, OPT_DH },
    { "style",                    required_argument, 0, OPT_STYLE },
    { "master",                   required_argument, 0, OPT_MASTER },

    { "line-spacing",             required_argument, 0, OPT_LINE_SPACING },
    { "line-spacing-180",         required_argument, 0, OPT_LS_180 },
    { "line-spacing-360",         required_argument, 0, OPT_LS_360 },
    { "line-spacing-60",          required_argument, 0, OPT_LS_60 },
    { "feed-180",                 required_argument, 0, OPT_FEED_180 },
    { "reverse-feed-180",         required_argument, 0, OPT_REV_FEED_180 },
    { "page-lines",               required_argument, 0, OPT_PAGE_LINES },
    { "page-inches",              required_argument, 0, OPT_PAGE_INCHES },
    { "skip-perf",                required_argument, 0, OPT_SKIP_PERF },
    { "no-skip-perf",             no_argument,       0, OPT_NO_SKIP_PERF },
    { "left-margin",              required_argument, 0, OPT_LEFT_MARGIN },
    { "right-margin",             required_argument, 0, OPT_RIGHT_MARGIN },
    { "lpi",                      required_argument, 0, OPT_LPI },

    { "h-pos",                    required_argument, 0, OPT_HPOS },
    { "h-rel",                    required_argument, 0, OPT_HREL },
    { "v-pos",                    required_argument, 0, OPT_VPOS },
    { "v-rel",                    required_argument, 0, OPT_VREL },

    { "upper-print",              no_argument,       0, OPT_UPPER_PRINT },
    { "upper-control",            no_argument,       0, OPT_UPPER_CTRL },
    { "country",                  required_argument, 0, OPT_COUNTRY },
    { "charset",                  required_argument, 0, OPT_CHARSET },

    { "clear-htabs",              no_argument,       0, OPT_CLEAR_HTABS },
    { "clear-vtabs",              no_argument,       0, OPT_CLEAR_VTABS },
    { "unidirectional",           required_argument, 0, OPT_UNIDIRECTIONAL },
    { "uni-line",                 no_argument,       0, OPT_UNI_LINE },
    { "justify",                  required_argument, 0, OPT_JUSTIFY },

    { "raw",                      required_argument, 0, OPT_RAW },
    { "text",                     required_argument, 0, OPT_TEXT },

    { "output",                   required_argument, 0, 'o' },
    { "help",                     no_argument,       0, 'h' },
    { "version",                  no_argument,       0, 'V' },
    { 0, 0, 0, 0 }
};

static void usage(FILE *fp)
{
    fprintf(fp,
"Usage: %s [OPTIONS...]\n"
"\n"
"Emit Epson ESC/P and ESC/P2 control sequences to stdout (or to FILE).\n"
"Each option is processed in command-line order, so the byte sequence\n"
"reflects the order you specify.\n"
"\n"
"General:\n"
"  -o, --output FILE        write output to FILE instead of stdout\n"
"  -i, --init  / --reset    ESC @  (initialise printer)\n"
"  -h, --help               this help\n"
"  -V, --version            print version\n"
"\n"
"Control bytes:\n"
"  --bs --ht --lf --vt --ff --cr --del --cancel --select --deselect\n"
"\n"
"Pitch and font:\n"
"  --pica | --elite | --cpi {10|12|15}\n"
"  --condensed | --cancel-condensed | --double-width-line | --cancel-double-width-line\n"
"  --proportional on|off\n"
"  --font NAME              roman,sans,courier,prestige,script,ocr-a,ocr-b,\n"
"                           orator,orator-c,data-block,data-large\n"
"  --quality draft|lq\n"
"\n"
"Style:\n"
"  --bold on|off            --double-strike on|off\n"
"  --italic on|off          --underline on|off\n"
"  --script super|sub|none  --style normal|outline|shadow|outline-shadow\n"
"  --double-width on|off    --double-height on|off\n"
"  --master N (0..255)      bit-mask master-select\n"
"\n"
"Layout:\n"
"  --line-spacing 1/6|1/8                  --lpi N\n"
"  --line-spacing-60 N    (0..127)         --line-spacing-180 N (0..255)\n"
"  --line-spacing-360 N   (0..255)\n"
"  --feed-180 N           --reverse-feed-180 N\n"
"  --page-lines N (1..127) --page-inches N (1..22)\n"
"  --skip-perf N | --no-skip-perf\n"
"  --left-margin N         --right-margin N\n"
"\n"
"Print position:\n"
"  --h-pos N (0..65535)   1/60\"          --h-rel N (-32768..32767)\n"
"  --v-pos N (0..65535)   ESC/P2 only      --v-rel N\n"
"\n"
"Character set:\n"
"  --upper-print | --upper-control\n"
"  --country N (0..15)     --charset N (0..3)\n"
"\n"
"Tabs and direction:\n"
"  --clear-htabs | --clear-vtabs\n"
"  --unidirectional on|off | --uni-line\n"
"  --justify left|center|right|full\n"
"\n"
"Misc:\n"
"  --text \"STRING\"          emit STRING as-is\n"
"  --raw HEX                emit raw bytes (e.g. --raw 1b40 or '1B 40')\n"
"\n"
"Examples:\n"
"  %s --init --cpi 12 --bold on --text Hello --bold off --lf\n"
"  echo Hello | %s --init --cpi 12 --bold on > out.bin\n",
        PROG_NAME, PROG_NAME, PROG_NAME);
}

int main(int argc, char **argv)
{
    out = stdout;
    int c;
    while ((c = getopt_long(argc, argv, "ho:Vi", long_opts, NULL)) != -1) {
        switch (c) {
            case 'h': usage(stdout); return 0;
            case 'V': printf("%s %s\n", PROG_NAME, PROG_VERSION); return 0;
            case 'o':
                if (out != stdout) fclose(out);
                out = fopen(optarg, "wb");
                if (!out) die("cannot open '%s': %s", optarg, strerror(errno));
                break;
            case 'i':
            case OPT_INIT:
            case OPT_RESET:   emit2(ESC, '@'); break;

            case OPT_BS:      emit1(0x08); break;
            case OPT_HT:      emit1(0x09); break;
            case OPT_LF:      emit1(0x0A); break;
            case OPT_VT:      emit1(0x0B); break;
            case OPT_FF:      emit1(0x0C); break;
            case OPT_CR:      emit1(0x0D); break;
            case OPT_DW_LINE:        emit1(0x0E); break;
            case OPT_CANCEL_DW_LINE: emit1(0x14); break;
            case OPT_CONDENSED:        emit1(0x0F); break;
            case OPT_CANCEL_CONDENSED: emit1(0x12); break;
            case OPT_SELECT:   emit1(0x11); break;
            case OPT_DESELECT: emit1(0x13); break;
            case OPT_CANCEL:   emit1(0x18); break;
            case OPT_DEL:      emit1(0x7F); break;

            case OPT_MSB0:       emit2(ESC, '='); break;
            case OPT_MSB1:       emit2(ESC, '>'); break;
            case OPT_MSB_CANCEL: emit2(ESC, '#'); break;

            case OPT_PICA:  emit2(ESC, 'P'); break;
            case OPT_ELITE: emit2(ESC, 'M'); break;
            case OPT_CPI:   cmd_cpi(optarg); break;
            case OPT_PROPORTIONAL:
                emit3(ESC, 'p', parse_on_off(optarg, "--proportional"));
                break;
            case OPT_FONT:    cmd_font(optarg); break;
            case OPT_QUALITY: cmd_quality(optarg); break;

            case OPT_BOLD:
                emit2(ESC, parse_on_off(optarg, "--bold") ? 'E' : 'F');
                break;
            case OPT_DOUBLE_STRIKE:
                emit2(ESC, parse_on_off(optarg, "--double-strike") ? 'G' : 'H');
                break;
            case OPT_ITALIC:
                emit2(ESC, parse_on_off(optarg, "--italic") ? '4' : '5');
                break;
            case OPT_UNDERLINE:
                emit3(ESC, '-', parse_on_off(optarg, "--underline"));
                break;
            case OPT_SCRIPT: cmd_script(optarg); break;
            case OPT_DW:
                emit3(ESC, 'W', parse_on_off(optarg, "--double-width"));
                break;
            case OPT_DH:
                emit3(ESC, 'w', parse_on_off(optarg, "--double-height"));
                break;
            case OPT_STYLE:  cmd_style(optarg); break;
            case OPT_MASTER:
                emit3(ESC, '!', (unsigned char)parse_long(optarg, 0, 255, "--master"));
                break;

            case OPT_LINE_SPACING: cmd_line_spacing(optarg); break;
            case OPT_LS_180:
                emit3(ESC, '3', (unsigned char)parse_long(optarg, 0, 255, "--line-spacing-180"));
                break;
            case OPT_LS_360:
                emit3(ESC, '+', (unsigned char)parse_long(optarg, 0, 255, "--line-spacing-360"));
                break;
            case OPT_LS_60:
                emit3(ESC, 'A', (unsigned char)parse_long(optarg, 0, 127, "--line-spacing-60"));
                break;
            case OPT_FEED_180:
                emit3(ESC, 'J', (unsigned char)parse_long(optarg, 0, 255, "--feed-180"));
                break;
            case OPT_REV_FEED_180:
                emit3(ESC, 'j', (unsigned char)parse_long(optarg, 0, 255, "--reverse-feed-180"));
                break;
            case OPT_PAGE_LINES:
                emit3(ESC, 'C', (unsigned char)parse_long(optarg, 1, 127, "--page-lines"));
                break;
            case OPT_PAGE_INCHES: {
                long n = parse_long(optarg, 1, 22, "--page-inches");
                unsigned char buf[4] = { ESC, 'C', 0x00, (unsigned char)n };
                emit(buf, 4);
                break;
            }
            case OPT_SKIP_PERF:
                emit3(ESC, 'N', (unsigned char)parse_long(optarg, 1, 127, "--skip-perf"));
                break;
            case OPT_NO_SKIP_PERF: emit2(ESC, 'O'); break;
            case OPT_LEFT_MARGIN:
                emit3(ESC, 'l', (unsigned char)parse_long(optarg, 0, 255, "--left-margin"));
                break;
            case OPT_RIGHT_MARGIN:
                emit3(ESC, 'Q', (unsigned char)parse_long(optarg, 1, 255, "--right-margin"));
                break;
            case OPT_LPI: cmd_lpi(optarg); break;

            case OPT_HPOS:
                emit_word('$', parse_long(optarg, 0, 65535, "--h-pos"));
                break;
            case OPT_HREL: {
                long v = parse_long(optarg, -32768, 32767, "--h-rel");
                emit_word('\\', v & 0xFFFF);
                break;
            }
            case OPT_VPOS:
                emit_escp2_word('V', parse_long(optarg, 0, 65535, "--v-pos"));
                break;
            case OPT_VREL: {
                long v = parse_long(optarg, -32768, 32767, "--v-rel");
                emit_escp2_word('v', v & 0xFFFF);
                break;
            }

            case OPT_UPPER_PRINT: emit2(ESC, '6'); break;
            case OPT_UPPER_CTRL:  emit2(ESC, '7'); break;
            case OPT_COUNTRY:
                emit3(ESC, 'R', (unsigned char)parse_long(optarg, 0, 15, "--country"));
                break;
            case OPT_CHARSET:
                emit3(ESC, 't', (unsigned char)parse_long(optarg, 0, 3, "--charset"));
                break;

            case OPT_CLEAR_HTABS: emit3(ESC, 'D', NUL); break;
            case OPT_CLEAR_VTABS: emit3(ESC, 'B', NUL); break;
            case OPT_UNIDIRECTIONAL:
                emit3(ESC, 'U', parse_on_off(optarg, "--unidirectional"));
                break;
            case OPT_UNI_LINE: emit2(ESC, '<'); break;
            case OPT_JUSTIFY:  cmd_justify(optarg); break;

            case OPT_RAW:  cmd_raw(optarg); break;
            case OPT_TEXT: emit((const unsigned char *)optarg, strlen(optarg)); break;

            case '?':
            default:
                fprintf(stderr, "Try '%s --help'\n", PROG_NAME);
                return 2;
        }
    }

    /* If the user gave no options at all, show help. */
    if (optind == 1 && argc == 1) {
        usage(stderr);
        return 2;
    }

    if (out != stdout) fclose(out);
    return 0;
}
