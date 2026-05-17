/*
 * escp_codes - shared ESC/P and ESC/P 2 byte emitters.
 *
 * See escp_codes.h for the public API.
 */

#define _POSIX_C_SOURCE 200809L

#include "escp_codes.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const char *progname = "escp";

void escp_set_progname(const char *name)
{
    if (name && *name) progname = name;
}

static void escp_die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

/* ----- byte primitives ----- */

void escp_emit(FILE *out, const unsigned char *bytes, size_t n)
{
    if (n == 0) return;
    if (fwrite(bytes, 1, n, out) != n)
        escp_die("write failed: %s", strerror(errno));
}

void escp_emit1(FILE *out, unsigned char b) { escp_emit(out, &b, 1); }

void escp_emit2(FILE *out, unsigned char a, unsigned char b)
{
    unsigned char buf[2] = { a, b };
    escp_emit(out, buf, 2);
}

void escp_emit3(FILE *out, unsigned char a, unsigned char b, unsigned char c)
{
    unsigned char buf[3] = { a, b, c };
    escp_emit(out, buf, 3);
}

void escp_emit_word(FILE *out, unsigned char cmd, long value)
{
    unsigned char buf[4];
    buf[0] = ESCP_ESC;
    buf[1] = cmd;
    buf[2] = (unsigned char)(value & 0xFF);
    buf[3] = (unsigned char)((value >> 8) & 0xFF);
    escp_emit(out, buf, sizeof buf);
}

void escp_emit_escp2_word(FILE *out, unsigned char cmd, long value)
{
    unsigned char buf[7];
    buf[0] = ESCP_ESC;
    buf[1] = '(';
    buf[2] = cmd;
    buf[3] = 0x02;
    buf[4] = 0x00;
    buf[5] = (unsigned char)(value & 0xFF);
    buf[6] = (unsigned char)((value >> 8) & 0xFF);
    escp_emit(out, buf, sizeof buf);
}

void escp_text(FILE *out, const char *s)
{
    if (s) escp_emit(out, (const unsigned char *)s, strlen(s));
}

void escp_textn(FILE *out, const char *s, size_t n)
{
    if (s && n) escp_emit(out, (const unsigned char *)s, n);
}

/* ----- single-byte control codes ----- */

void escp_bs(FILE *out) { escp_emit1(out, 0x08); }
void escp_ht(FILE *out) { escp_emit1(out, 0x09); }
void escp_lf(FILE *out) { escp_emit1(out, 0x0A); }
void escp_vt(FILE *out) { escp_emit1(out, 0x0B); }
void escp_ff(FILE *out) { escp_emit1(out, 0x0C); }
void escp_cr(FILE *out) { escp_emit1(out, 0x0D); }
void escp_double_width_line(FILE *out)        { escp_emit1(out, 0x0E); }
void escp_cancel_double_width_line(FILE *out) { escp_emit1(out, 0x14); }
void escp_condensed(FILE *out)        { escp_emit1(out, 0x0F); }
void escp_cancel_condensed(FILE *out) { escp_emit1(out, 0x12); }
void escp_select(FILE *out)   { escp_emit1(out, 0x11); }
void escp_deselect(FILE *out) { escp_emit1(out, 0x13); }
void escp_cancel(FILE *out)   { escp_emit1(out, 0x18); }
void escp_del(FILE *out)      { escp_emit1(out, 0x7F); }

/* ----- printer state ----- */

void escp_init(FILE *out)       { escp_emit2(out, ESCP_ESC, '@'); }
void escp_msb_0(FILE *out)      { escp_emit2(out, ESCP_ESC, '='); }
void escp_msb_1(FILE *out)      { escp_emit2(out, ESCP_ESC, '>'); }
void escp_msb_cancel(FILE *out) { escp_emit2(out, ESCP_ESC, '#'); }

/* ----- pitch and font ----- */

void escp_pica(FILE *out)  { escp_emit2(out, ESCP_ESC, 'P'); }
void escp_elite(FILE *out) { escp_emit2(out, ESCP_ESC, 'M'); }

void escp_cpi(FILE *out, int n)
{
    switch (n) {
        case 10: escp_pica(out);  break;
        case 12: escp_elite(out); break;
        case 15: escp_emit2(out, ESCP_ESC, 'g'); break;
        default: escp_die("cpi: only 10, 12 or 15 are supported (got %d)", n);
    }
}

void escp_proportional(FILE *out, int on)
{
    escp_emit3(out, ESCP_ESC, 'p', on ? 1 : 0);
}

void escp_font(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, 'k', (unsigned char)n);
}

void escp_quality(FILE *out, int q)
{
    escp_emit3(out, ESCP_ESC, 'x', q ? 1 : 0);
}

/* ----- style ----- */

void escp_bold(FILE *out, int on)
{
    escp_emit2(out, ESCP_ESC, on ? 'E' : 'F');
}

void escp_double_strike(FILE *out, int on)
{
    escp_emit2(out, ESCP_ESC, on ? 'G' : 'H');
}

void escp_italic(FILE *out, int on)
{
    escp_emit2(out, ESCP_ESC, on ? '4' : '5');
}

void escp_underline(FILE *out, int on)
{
    escp_emit3(out, ESCP_ESC, '-', on ? 1 : 0);
}

void escp_double_width(FILE *out, int on)
{
    escp_emit3(out, ESCP_ESC, 'W', on ? 1 : 0);
}

void escp_double_height(FILE *out, int on)
{
    escp_emit3(out, ESCP_ESC, 'w', on ? 1 : 0);
}

void escp_script(FILE *out, int s)
{
    switch (s) {
        case ESCP_SCRIPT_SUPER: escp_emit3(out, ESCP_ESC, 'S', 0); break;
        case ESCP_SCRIPT_SUB:   escp_emit3(out, ESCP_ESC, 'S', 1); break;
        default:                escp_emit2(out, ESCP_ESC, 'T');    break;
    }
}

void escp_style(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, 'q', (unsigned char)n);
}

void escp_master(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, '!', (unsigned char)n);
}

/* ----- layout ----- */

void escp_line_spacing_1_8(FILE *out) { escp_emit2(out, ESCP_ESC, '0'); }
void escp_line_spacing_1_6(FILE *out) { escp_emit2(out, ESCP_ESC, '2'); }

void escp_line_spacing_180(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, '3', (unsigned char)n);
}

void escp_line_spacing_360(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, '+', (unsigned char)n);
}

void escp_line_spacing_60(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, 'A', (unsigned char)n);
}

void escp_feed_180(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, 'J', (unsigned char)n);
}

void escp_reverse_feed_180(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, 'j', (unsigned char)n);
}

void escp_page_lines(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, 'C', (unsigned char)n);
}

void escp_page_inches(FILE *out, int n)
{
    unsigned char buf[4] = { ESCP_ESC, 'C', 0x00, (unsigned char)n };
    escp_emit(out, buf, 4);
}

void escp_skip_perf(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, 'N', (unsigned char)n);
}

void escp_no_skip_perf(FILE *out) { escp_emit2(out, ESCP_ESC, 'O'); }

void escp_left_margin(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, 'l', (unsigned char)n);
}

void escp_right_margin(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, 'Q', (unsigned char)n);
}

void escp_lpi(FILE *out, int n)
{
    if (n == 6)       escp_line_spacing_1_6(out);
    else if (n == 8)  escp_line_spacing_1_8(out);
    else {
        long units = 180 / n;
        if (units < 1 || units > 255)
            escp_die("lpi: %d lines per inch is not representable", n);
        escp_line_spacing_180(out, (int)units);
    }
}

/* ----- print position ----- */

void escp_h_pos(FILE *out, int n) { escp_emit_word(out, '$', n);       }
void escp_h_rel(FILE *out, int n) { escp_emit_word(out, '\\', n & 0xFFFF); }
void escp_v_pos(FILE *out, int n) { escp_emit_escp2_word(out, 'V', n);     }
void escp_v_rel(FILE *out, int n) { escp_emit_escp2_word(out, 'v', n & 0xFFFF); }

/* ----- character set ----- */

void escp_upper_print(FILE *out)   { escp_emit2(out, ESCP_ESC, '6'); }
void escp_upper_control(FILE *out) { escp_emit2(out, ESCP_ESC, '7'); }

void escp_country(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, 'R', (unsigned char)n);
}

void escp_charset(FILE *out, int n)
{
    escp_emit3(out, ESCP_ESC, 't', (unsigned char)n);
}

/* ----- tabs and direction ----- */

void escp_clear_htabs(FILE *out) { escp_emit3(out, ESCP_ESC, 'D', ESCP_NUL); }
void escp_clear_vtabs(FILE *out) { escp_emit3(out, ESCP_ESC, 'B', ESCP_NUL); }

void escp_unidirectional(FILE *out, int on)
{
    escp_emit3(out, ESCP_ESC, 'U', on ? 1 : 0);
}

void escp_uni_line(FILE *out) { escp_emit2(out, ESCP_ESC, '<'); }

void escp_justify(FILE *out, int j)
{
    escp_emit3(out, ESCP_ESC, 'a', (unsigned char)j);
}

/* ----- string mappers ----- */

int escp_font_from_name(const char *name)
{
    static const struct { const char *name; int n; } map[] = {
        { "roman",         ESCP_FONT_ROMAN       },
        { "sans",          ESCP_FONT_SANS        },
        { "letter-gothic", ESCP_FONT_SANS        },
        { "courier",       ESCP_FONT_COURIER     },
        { "prestige",      ESCP_FONT_PRESTIGE    },
        { "script",        ESCP_FONT_SCRIPT      },
        { "ocr-b",         ESCP_FONT_OCR_B       },
        { "ocr-a",         ESCP_FONT_OCR_A       },
        { "orator-c",      ESCP_FONT_ORATOR_C    },
        { "orator",        ESCP_FONT_ORATOR      },
        { "data-block",    ESCP_FONT_DATA_BLOCK  },
        { "data-large",    ESCP_FONT_DATA_LARGE  }
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strcmp(map[i].name, name) == 0) return map[i].n;
    return -1;
}

int escp_quality_from_name(const char *name)
{
    if (strcmp(name, "draft")  == 0) return ESCP_QUALITY_DRAFT;
    if (strcmp(name, "lq")     == 0) return ESCP_QUALITY_LQ;
    if (strcmp(name, "letter") == 0) return ESCP_QUALITY_LQ;
    return -1;
}

int escp_script_from_name(const char *name)
{
    if (strcmp(name, "super") == 0) return ESCP_SCRIPT_SUPER;
    if (strcmp(name, "sub")   == 0) return ESCP_SCRIPT_SUB;
    if (strcmp(name, "none")  == 0) return ESCP_SCRIPT_NONE;
    return -1;
}

int escp_style_from_name(const char *name)
{
    if (strcmp(name, "normal")         == 0) return 0;
    if (strcmp(name, "outline")        == 0) return 1;
    if (strcmp(name, "shadow")         == 0) return 2;
    if (strcmp(name, "outline-shadow") == 0) return 3;
    return -1;
}

int escp_justify_from_name(const char *name)
{
    if (strcmp(name, "left")   == 0) return ESCP_JUSTIFY_LEFT;
    if (strcmp(name, "center") == 0) return ESCP_JUSTIFY_CENTER;
    if (strcmp(name, "centre") == 0) return ESCP_JUSTIFY_CENTER;
    if (strcmp(name, "right")  == 0) return ESCP_JUSTIFY_RIGHT;
    if (strcmp(name, "full")   == 0) return ESCP_JUSTIFY_FULL;
    return -1;
}
