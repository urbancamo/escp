/*
 * escp_codes - shared ESC/P and ESC/P 2 byte emitters.
 *
 * Both the escp(1) CLI and the escpmd(1) markdown filter call into
 * this module; nothing else in the project hard-codes ESC sequences.
 *
 * All functions write to a caller-supplied FILE *. On write failure
 * the module prints an error to stderr and calls exit(2); callers do
 * not need to check return values.
 */

#ifndef ESCP_CODES_H
#define ESCP_CODES_H

#include <stdio.h>
#include <stddef.h>

#define ESCP_ESC 0x1B
#define ESCP_NUL 0x00

/* Override the program name used in error messages. Default: "escp". */
void escp_set_progname(const char *name);

/* ----- byte primitives ----- */
void escp_emit(FILE *out, const unsigned char *bytes, size_t n);
void escp_emit1(FILE *out, unsigned char b);
void escp_emit2(FILE *out, unsigned char a, unsigned char b);
void escp_emit3(FILE *out, unsigned char a, unsigned char b, unsigned char c);

/* ESC c nL nH   (ESC $ for h-pos, ESC \ for h-rel) */
void escp_emit_word(FILE *out, unsigned char cmd, long value);
/* ESC ( cmd 02 00 nL nH   (ESC ( V for v-pos, ESC ( v for v-rel) */
void escp_emit_escp2_word(FILE *out, unsigned char cmd, long value);

/* Plain text passthrough. */
void escp_text(FILE *out, const char *s);
void escp_textn(FILE *out, const char *s, size_t n);

/* ----- single-byte control codes ----- */
void escp_bs(FILE *out);
void escp_ht(FILE *out);
void escp_lf(FILE *out);
void escp_vt(FILE *out);
void escp_ff(FILE *out);
void escp_cr(FILE *out);
void escp_double_width_line(FILE *out);          /* SO  */
void escp_cancel_double_width_line(FILE *out);   /* DC4 */
void escp_condensed(FILE *out);                  /* SI  */
void escp_cancel_condensed(FILE *out);           /* DC2 */
void escp_select(FILE *out);                     /* DC1 */
void escp_deselect(FILE *out);                   /* DC3 */
void escp_cancel(FILE *out);                     /* CAN */
void escp_del(FILE *out);                        /* DEL */

/* ----- printer state ----- */
void escp_init(FILE *out);          /* ESC @ */
void escp_msb_0(FILE *out);         /* ESC = */
void escp_msb_1(FILE *out);         /* ESC > */
void escp_msb_cancel(FILE *out);    /* ESC # */

/* ----- pitch and font ----- */
void escp_pica(FILE *out);                       /* ESC P */
void escp_elite(FILE *out);                      /* ESC M */
void escp_cpi(FILE *out, int n);                 /* 10|12|15 -> P|M|g  */
void escp_proportional(FILE *out, int on);       /* ESC p   */

typedef enum {
    ESCP_FONT_ROMAN       = 0,
    ESCP_FONT_SANS        = 1,
    ESCP_FONT_COURIER     = 2,
    ESCP_FONT_PRESTIGE    = 3,
    ESCP_FONT_SCRIPT      = 4,
    ESCP_FONT_OCR_B       = 5,
    ESCP_FONT_OCR_A       = 6,
    ESCP_FONT_ORATOR_C    = 7,
    ESCP_FONT_ORATOR      = 8,
    ESCP_FONT_DATA_BLOCK  = 10,
    ESCP_FONT_DATA_LARGE  = 11
} escp_font_t;
void escp_font(FILE *out, int n);                /* ESC k n */

typedef enum {
    ESCP_QUALITY_DRAFT = 0,
    ESCP_QUALITY_LQ    = 1
} escp_quality_t;
void escp_quality(FILE *out, int q);             /* ESC x q */

/* ----- style toggles (on != 0) ----- */
void escp_bold(FILE *out, int on);              /* ESC E / ESC F */
void escp_double_strike(FILE *out, int on);     /* ESC G / ESC H */
void escp_italic(FILE *out, int on);            /* ESC 4 / ESC 5 */
void escp_underline(FILE *out, int on);         /* ESC - n        */
void escp_double_width(FILE *out, int on);      /* ESC W n        */
void escp_double_height(FILE *out, int on);     /* ESC w n        */

typedef enum {
    ESCP_SCRIPT_SUPER = 0,
    ESCP_SCRIPT_SUB   = 1,
    ESCP_SCRIPT_NONE  = 2
} escp_script_t;
void escp_script(FILE *out, int s);             /* ESC S n / ESC T */

void escp_style(FILE *out, int n);              /* ESC q n (0..3)  */
void escp_master(FILE *out, int n);             /* ESC ! n         */

/* ----- layout ----- */
void escp_line_spacing_1_8(FILE *out);          /* ESC 0 */
void escp_line_spacing_1_6(FILE *out);          /* ESC 2 */
void escp_line_spacing_180(FILE *out, int n);   /* ESC 3 n */
void escp_line_spacing_360(FILE *out, int n);   /* ESC + n */
void escp_line_spacing_60(FILE *out, int n);    /* ESC A n */
void escp_feed_180(FILE *out, int n);           /* ESC J n */
void escp_reverse_feed_180(FILE *out, int n);   /* ESC j n */
void escp_page_lines(FILE *out, int n);         /* ESC C n      */
void escp_page_inches(FILE *out, int n);        /* ESC C 00 n   */
void escp_skip_perf(FILE *out, int n);          /* ESC N n */
void escp_no_skip_perf(FILE *out);              /* ESC O   */
void escp_left_margin(FILE *out, int n);        /* ESC l n */
void escp_right_margin(FILE *out, int n);       /* ESC Q n */
void escp_lpi(FILE *out, int n);                /* 6|8|other -> ESC 2 / ESC 0 / ESC 3 */

/* ----- print position ----- */
void escp_h_pos(FILE *out, int n);              /* ESC $ */
void escp_h_rel(FILE *out, int n);              /* ESC \ */
void escp_v_pos(FILE *out, int n);              /* ESC ( V */
void escp_v_rel(FILE *out, int n);              /* ESC ( v */

/* ----- character set ----- */
void escp_upper_print(FILE *out);               /* ESC 6 */
void escp_upper_control(FILE *out);             /* ESC 7 */
void escp_country(FILE *out, int n);            /* ESC R n */
void escp_charset(FILE *out, int n);            /* ESC t n */

/* ----- tabs and direction ----- */
void escp_clear_htabs(FILE *out);               /* ESC D NUL */
void escp_clear_vtabs(FILE *out);               /* ESC B NUL */
void escp_unidirectional(FILE *out, int on);    /* ESC U n */
void escp_uni_line(FILE *out);                  /* ESC < */

typedef enum {
    ESCP_JUSTIFY_LEFT   = 0,
    ESCP_JUSTIFY_CENTER = 1,
    ESCP_JUSTIFY_RIGHT  = 2,
    ESCP_JUSTIFY_FULL   = 3
} escp_justify_t;
void escp_justify(FILE *out, int j);            /* ESC a n */

/* ----- string mappers (return -1 on unknown name) ----- */
int escp_font_from_name(const char *name);
int escp_quality_from_name(const char *name);
int escp_script_from_name(const char *name);
int escp_style_from_name(const char *name);
int escp_justify_from_name(const char *name);

#endif /* ESCP_CODES_H */
