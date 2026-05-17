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
 *
 * All ESC/P knowledge lives in escp_codes.c; this file is only the
 * command-line front end.
 */

#define _POSIX_C_SOURCE 200809L

#include "escp_codes.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROG_NAME    "escp"
#define PROG_VERSION "1.1.0"

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
    if (strcmp(s, "on")  == 0 || strcmp(s, "1") == 0) return 1;
    if (strcmp(s, "off") == 0 || strcmp(s, "0") == 0) return 0;
    die("%s: expected 'on' or 'off', got '%s'", name, s);
    return 0;
}

static void cmd_raw(const char *hex)
{
    size_t len = strlen(hex);
    if (len == 0) return;
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
    escp_emit(out, buf, n);
    free(buf);
}

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
    escp_set_progname(PROG_NAME);

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
            case OPT_RESET:   escp_init(out); break;

            case OPT_BS:               escp_bs(out); break;
            case OPT_HT:               escp_ht(out); break;
            case OPT_LF:               escp_lf(out); break;
            case OPT_VT:               escp_vt(out); break;
            case OPT_FF:               escp_ff(out); break;
            case OPT_CR:               escp_cr(out); break;
            case OPT_DW_LINE:          escp_double_width_line(out); break;
            case OPT_CANCEL_DW_LINE:   escp_cancel_double_width_line(out); break;
            case OPT_CONDENSED:        escp_condensed(out); break;
            case OPT_CANCEL_CONDENSED: escp_cancel_condensed(out); break;
            case OPT_SELECT:           escp_select(out); break;
            case OPT_DESELECT:         escp_deselect(out); break;
            case OPT_CANCEL:           escp_cancel(out); break;
            case OPT_DEL:              escp_del(out); break;

            case OPT_MSB0:       escp_msb_0(out); break;
            case OPT_MSB1:       escp_msb_1(out); break;
            case OPT_MSB_CANCEL: escp_msb_cancel(out); break;

            case OPT_PICA:  escp_pica(out); break;
            case OPT_ELITE: escp_elite(out); break;
            case OPT_CPI:
                escp_cpi(out, (int)parse_long(optarg, 10, 20, "--cpi"));
                break;
            case OPT_PROPORTIONAL:
                escp_proportional(out, parse_on_off(optarg, "--proportional"));
                break;
            case OPT_FONT: {
                int n = escp_font_from_name(optarg);
                if (n < 0) die("--font: unknown family '%s'", optarg);
                escp_font(out, n);
                break;
            }
            case OPT_QUALITY: {
                int q = escp_quality_from_name(optarg);
                if (q < 0) die("--quality: expected 'draft' or 'lq', got '%s'", optarg);
                escp_quality(out, q);
                break;
            }

            case OPT_BOLD:
                escp_bold(out, parse_on_off(optarg, "--bold"));
                break;
            case OPT_DOUBLE_STRIKE:
                escp_double_strike(out, parse_on_off(optarg, "--double-strike"));
                break;
            case OPT_ITALIC:
                escp_italic(out, parse_on_off(optarg, "--italic"));
                break;
            case OPT_UNDERLINE:
                escp_underline(out, parse_on_off(optarg, "--underline"));
                break;
            case OPT_SCRIPT: {
                int s = escp_script_from_name(optarg);
                if (s < 0) die("--script: expected super|sub|none, got '%s'", optarg);
                escp_script(out, s);
                break;
            }
            case OPT_DW:
                escp_double_width(out, parse_on_off(optarg, "--double-width"));
                break;
            case OPT_DH:
                escp_double_height(out, parse_on_off(optarg, "--double-height"));
                break;
            case OPT_STYLE: {
                int n = escp_style_from_name(optarg);
                if (n < 0) die("--style: unknown style '%s'", optarg);
                escp_style(out, n);
                break;
            }
            case OPT_MASTER:
                escp_master(out, (int)parse_long(optarg, 0, 255, "--master"));
                break;

            case OPT_LINE_SPACING:
                if (strcmp(optarg, "1/8") == 0)      escp_line_spacing_1_8(out);
                else if (strcmp(optarg, "1/6") == 0) escp_line_spacing_1_6(out);
                else die("--line-spacing: expected '1/6' or '1/8' (got '%s')", optarg);
                break;
            case OPT_LS_180:
                escp_line_spacing_180(out, (int)parse_long(optarg, 0, 255, "--line-spacing-180"));
                break;
            case OPT_LS_360:
                escp_line_spacing_360(out, (int)parse_long(optarg, 0, 255, "--line-spacing-360"));
                break;
            case OPT_LS_60:
                escp_line_spacing_60(out, (int)parse_long(optarg, 0, 127, "--line-spacing-60"));
                break;
            case OPT_FEED_180:
                escp_feed_180(out, (int)parse_long(optarg, 0, 255, "--feed-180"));
                break;
            case OPT_REV_FEED_180:
                escp_reverse_feed_180(out, (int)parse_long(optarg, 0, 255, "--reverse-feed-180"));
                break;
            case OPT_PAGE_LINES:
                escp_page_lines(out, (int)parse_long(optarg, 1, 127, "--page-lines"));
                break;
            case OPT_PAGE_INCHES:
                escp_page_inches(out, (int)parse_long(optarg, 1, 22, "--page-inches"));
                break;
            case OPT_SKIP_PERF:
                escp_skip_perf(out, (int)parse_long(optarg, 1, 127, "--skip-perf"));
                break;
            case OPT_NO_SKIP_PERF: escp_no_skip_perf(out); break;
            case OPT_LEFT_MARGIN:
                escp_left_margin(out, (int)parse_long(optarg, 0, 255, "--left-margin"));
                break;
            case OPT_RIGHT_MARGIN:
                escp_right_margin(out, (int)parse_long(optarg, 1, 255, "--right-margin"));
                break;
            case OPT_LPI:
                escp_lpi(out, (int)parse_long(optarg, 1, 360, "--lpi"));
                break;

            case OPT_HPOS:
                escp_h_pos(out, (int)parse_long(optarg, 0, 65535, "--h-pos"));
                break;
            case OPT_HREL:
                escp_h_rel(out, (int)parse_long(optarg, -32768, 32767, "--h-rel"));
                break;
            case OPT_VPOS:
                escp_v_pos(out, (int)parse_long(optarg, 0, 65535, "--v-pos"));
                break;
            case OPT_VREL:
                escp_v_rel(out, (int)parse_long(optarg, -32768, 32767, "--v-rel"));
                break;

            case OPT_UPPER_PRINT: escp_upper_print(out); break;
            case OPT_UPPER_CTRL:  escp_upper_control(out); break;
            case OPT_COUNTRY:
                escp_country(out, (int)parse_long(optarg, 0, 15, "--country"));
                break;
            case OPT_CHARSET:
                escp_charset(out, (int)parse_long(optarg, 0, 3, "--charset"));
                break;

            case OPT_CLEAR_HTABS: escp_clear_htabs(out); break;
            case OPT_CLEAR_VTABS: escp_clear_vtabs(out); break;
            case OPT_UNIDIRECTIONAL:
                escp_unidirectional(out, parse_on_off(optarg, "--unidirectional"));
                break;
            case OPT_UNI_LINE: escp_uni_line(out); break;
            case OPT_JUSTIFY: {
                int j = escp_justify_from_name(optarg);
                if (j < 0) die("--justify: unknown mode '%s'", optarg);
                escp_justify(out, j);
                break;
            }

            case OPT_RAW:  cmd_raw(optarg); break;
            case OPT_TEXT: escp_text(out, optarg); break;

            case '?':
            default:
                fprintf(stderr, "Try '%s --help'\n", PROG_NAME);
                return 2;
        }
    }

    if (optind == 1 && argc == 1) {
        usage(stderr);
        return 2;
    }

    if (out != stdout) fclose(out);
    return 0;
}
