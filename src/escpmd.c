/*
 * escpmd - render Markdown to an ESC/P 2 byte stream.
 *
 *     escpmd README.md | lp -d pp404
 *     cat doc.md | escpmd --cpi 12 --quality lq > pp404.bin
 *
 * Input is CommonMark (with GFM tables, strikethrough and autolinks via
 * md4c). Output is the same kind of ESC/P that escp(1) emits, produced
 * via the shared escp_codes module.
 *
 * All ESC sequences and printable bytes pass through a single
 * line-buffered emitter (see "line layout" below) so the program can
 * word-wrap paragraphs when a printable width is known.  When no
 * --page-width or margin is given, the buffer is byte-equivalent to a
 * direct stream — existing test fixtures stay valid.
 *
 * The translation is documented in docs/escpmd-mapping.md.
 */

#define _POSIX_C_SOURCE 200809L

#include "escp_codes.h"
#include "md4c.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <iconv.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROG_NAME    "escpmd"
#define PROG_VERSION "1.1.0"

#define MAX_LIST_DEPTH 16
#define MAX_INDENT     16
#define HR_DEFAULT     72

/* ----- growable byte buffer ----- */

typedef struct {
    unsigned char *buf;
    size_t len;
    size_t cap;
} bytebuf_t;

static void bb_reset(bytebuf_t *b) { b->len = 0; }

static void bb_append(bytebuf_t *b, const unsigned char *src, size_t n)
{
    if (n == 0) return;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < b->len + n) nc *= 2;
        unsigned char *p = realloc(b->buf, nc);
        if (!p) { fprintf(stderr, PROG_NAME ": out of memory\n"); exit(2); }
        b->buf = p;
        b->cap = nc;
    }
    memcpy(b->buf + b->len, src, n);
    b->len += n;
}

static void bb_free(bytebuf_t *b) { free(b->buf); b->buf = NULL; b->len = b->cap = 0; }

typedef struct {
    int is_ordered;
    int counter;
} list_frame_t;

typedef struct {
    unsigned char bytes[MAX_INDENT];   /* visible bytes (spaces) for wrap continuation */
    int           visible;
} indent_t;

typedef struct {
    FILE *out;

    /* CLI options */
    int  no_init;
    int  no_ff;
    int  body_cpi;
    int  body_font;
    int  body_quality;
    int  body_lpi;
    int  left_margin;     /* -1 = unset */
    int  right_margin;    /* -1 = unset */
    int  page_width;      /* derived; 0 = no software wrap */
    int  smileys;
    int  link_urls;
    const char *charset;

    /* UTF-8 → target-charset conversion */
    iconv_t conv;

    /* Staging memstream: escp_codes writes here, we drain to the
     * line buffer immediately afterwards so it can route invisible
     * bytes through word-wrap state. */
    FILE   *staging;
    char   *staging_buf;
    size_t  staging_buf_size;

    /* nestable style toggles */
    int  bold_depth;
    int  italic_depth;
    int  underline_depth;
    int  courier_depth;
    int  condensed_depth;
    int  double_width_depth;
    int  double_height_depth;
    int  draft_depth;

    /* block state */
    int  in_code_block;
    int  heading_level;
    int  quote_depth;
    list_frame_t lists[MAX_LIST_DEPTH];
    int  list_depth;

    /* line layout. The model:
     *   column          = visible columns emitted on the current physical line
     *   pending_space   = a separator space is queued, to be flushed before
     *                     the next word (or dropped if we wrap)
     *   word_buf        = bytes of the current in-progress word, possibly
     *                     interleaved with zero-width ESC sequences
     *   word_visible    = visible-byte count of word_buf
     * A "word" is a maximal non-whitespace run. ESC sequences emitted
     * mid-word join the buffer; emitted between words they pass through. */
    int        column;
    int        pending_space;
    bytebuf_t  word_buf;
    int        word_visible;

    /* Continuation indent stack: each LI / quote frame pushes the
     * visible bytes that should re-introduce its indent after a wrap. */
    indent_t   indents[MAX_LIST_DEPTH + 4];
    int        indent_top;

    /* Block separation. */
    int  pending_blanks;
    int  at_line_start;

    /* Link rendering. */
    int    link_active;
    int    link_is_autolink;
    char   link_href[1024];
    size_t link_href_len;
} state_t;

/* ----- small helpers ----- */

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

/* ----- low-level emit primitives ------------------------------------
 *
 * raw_out_bytes    -- write bytes to s->out unconditionally; no state.
 * write_indent     -- emit the current continuation indent to s->out.
 * flush_word       -- emit pending_space + word_buf, wrapping if needed.
 * out_invisible    -- ESC sequence or other zero-column bytes.
 * out_visible_run  -- one or more printable bytes that form word content.
 * out_force_visible-- printable bytes that are structural (bullet, HR,
 *                     indent prefix) and should NOT participate in
 *                     word-wrap or be relocated.
 * add_space        -- request a separator space.
 * raw_lf           -- end-of-line: flush word, emit LF, column=0.
 */

static void raw_out_bytes(state_t *s, const unsigned char *bytes, size_t n)
{
    if (n == 0) return;
    if (fwrite(bytes, 1, n, s->out) != n)
        die("write failed: %s", strerror(errno));
}

static void write_indent(state_t *s)
{
    if (s->indent_top <= 0) return;
    const indent_t *ind = &s->indents[s->indent_top - 1];
    if (ind->visible > 0) {
        raw_out_bytes(s, ind->bytes, (size_t)ind->visible);
        s->column = ind->visible;
    }
}

static void flush_word(state_t *s)
{
    if (s->word_buf.len == 0 && !s->pending_space) return;

    int gap   = s->pending_space ? 1 : 0;
    int width = s->page_width;

    if (width > 0 && s->column > 0
            && s->column + gap + s->word_visible > width) {
        /* Wrap: drop the pending space, start a new line, replace it
         * with the continuation indent. */
        raw_out_bytes(s, (const unsigned char *)"\n", 1);
        s->column = 0;
        write_indent(s);
        s->pending_space = 0;
    } else if (s->pending_space) {
        raw_out_bytes(s, (const unsigned char *)" ", 1);
        s->column++;
        s->pending_space = 0;
    }

    if (s->word_buf.len > 0) {
        raw_out_bytes(s, s->word_buf.buf, s->word_buf.len);
        s->column += s->word_visible;
        bb_reset(&s->word_buf);
        s->word_visible = 0;
    }
}

static void out_invisible(state_t *s, const unsigned char *bytes, size_t n)
{
    if (n == 0) return;
    /* If a word is in progress (or a space is pending), the ESC bytes
     * belong with the upcoming visible content — buffer them so a wrap
     * keeps them attached to the right side. */
    if (s->word_visible > 0 || s->pending_space) {
        bb_append(&s->word_buf, bytes, n);
    } else {
        raw_out_bytes(s, bytes, n);
    }
}

static void out_visible_run(state_t *s, const unsigned char *bytes, size_t n)
{
    if (n == 0) return;
    bb_append(&s->word_buf, bytes, n);
    s->word_visible += (int)n;
    s->at_line_start = 0;
}

static void out_force_visible(state_t *s, const unsigned char *bytes, size_t n)
{
    if (n == 0) return;
    /* Structural prefix — flush any in-progress word, then emit and
     * count columns directly. */
    flush_word(s);
    raw_out_bytes(s, bytes, n);
    s->column += (int)n;
    s->at_line_start = 0;
}

static void add_space(state_t *s)
{
    /* A space coming AFTER content: flush the word first. */
    if (s->word_buf.len > 0) flush_word(s);

    /* Leading whitespace at column 0 (no content yet on this line) is
     * dropped — we never start a fresh line with bare spaces. */
    if (s->column == 0 && !s->at_line_start) {
        /* unusual; ignore */
        s->pending_space = 0;
        return;
    }
    if (s->column == 0) {
        s->pending_space = 0;
        return;
    }
    s->pending_space = 1;
}

static void raw_lf(state_t *s)
{
    flush_word(s);
    raw_out_bytes(s, (const unsigned char *)"\n", 1);
    s->column         = 0;
    s->pending_space  = 0;
    s->at_line_start  = 1;
}

/* Drain bytes accumulated in s->staging through out_invisible. Called
 * after every escp_codes call so style ESC sequences get routed into
 * the word buffer correctly. */
static void drain_staging(state_t *s)
{
    fflush(s->staging);
    if (s->staging_buf_size > 0) {
        out_invisible(s, (const unsigned char *)s->staging_buf,
                      s->staging_buf_size);
    }
    rewind(s->staging);
    /* After rewind, the next write resets the logical length; size
     * reported on next fflush will be only the new bytes. */
}

/* ----- style stacks ----- */

static void push_bold(state_t *s)        { if (s->bold_depth++ == 0)         { escp_bold(s->staging, 1); drain_staging(s); } }
static void pop_bold(state_t *s)         { if (--s->bold_depth == 0)         { escp_bold(s->staging, 0); drain_staging(s); } }
static void push_italic(state_t *s)      { if (s->italic_depth++ == 0)       { escp_italic(s->staging, 1); drain_staging(s); } }
static void pop_italic(state_t *s)       { if (--s->italic_depth == 0)       { escp_italic(s->staging, 0); drain_staging(s); } }
static void push_underline(state_t *s)   { if (s->underline_depth++ == 0)    { escp_underline(s->staging, 1); drain_staging(s); } }
static void pop_underline(state_t *s)    { if (--s->underline_depth == 0)    { escp_underline(s->staging, 0); drain_staging(s); } }
static void push_double_width(state_t *s)  { if (s->double_width_depth++  == 0) { escp_double_width(s->staging, 1);  drain_staging(s); } }
static void pop_double_width(state_t *s)   { if (--s->double_width_depth  == 0) { escp_double_width(s->staging, 0);  drain_staging(s); } }
static void push_double_height(state_t *s) { if (s->double_height_depth++ == 0) { escp_double_height(s->staging, 1); drain_staging(s); } }
static void pop_double_height(state_t *s)  { if (--s->double_height_depth == 0) { escp_double_height(s->staging, 0); drain_staging(s); } }

static void push_courier(state_t *s)
{
    if (s->courier_depth++ == 0 && s->body_font != ESCP_FONT_COURIER) {
        escp_font(s->staging, ESCP_FONT_COURIER);
        drain_staging(s);
    }
}
static void pop_courier(state_t *s)
{
    if (--s->courier_depth == 0 && s->body_font != ESCP_FONT_COURIER) {
        escp_font(s->staging, s->body_font);
        drain_staging(s);
    }
}
static void push_condensed(state_t *s) { if (s->condensed_depth++ == 0) { escp_condensed(s->staging);        drain_staging(s); } }
static void pop_condensed(state_t *s)  { if (--s->condensed_depth == 0) { escp_cancel_condensed(s->staging); drain_staging(s); } }
static void push_draft(state_t *s)
{
    if (s->draft_depth++ == 0 && s->body_quality != ESCP_QUALITY_DRAFT) {
        escp_quality(s->staging, ESCP_QUALITY_DRAFT);
        drain_staging(s);
    }
}
static void pop_draft(state_t *s)
{
    if (--s->draft_depth == 0 && s->body_quality != ESCP_QUALITY_DRAFT) {
        escp_quality(s->staging, s->body_quality);
        drain_staging(s);
    }
}

/* ----- block layout helpers ----- */

static void flush_blanks(state_t *s)
{
    while (s->pending_blanks > 0) {
        raw_lf(s);
        s->pending_blanks--;
    }
}

static void end_line(state_t *s)
{
    if (!s->at_line_start || s->word_buf.len > 0 || s->pending_space) {
        raw_lf(s);
    }
}

/* ----- indent stack ----- */

static void push_indent(state_t *s, int visible)
{
    if (s->indent_top >= (int)(sizeof s->indents / sizeof s->indents[0])) return;
    indent_t *ind = &s->indents[s->indent_top++];
    if (visible > MAX_INDENT) visible = MAX_INDENT;
    memset(ind->bytes, ' ', (size_t)visible);
    ind->visible = visible;
}
static void pop_indent(state_t *s)
{
    if (s->indent_top > 0) s->indent_top--;
}

/* ----- iconv conversion ----- */

static void open_iconv(state_t *s)
{
    if (s->conv != (iconv_t)-1) return;
    char with_translit[80];
    int n = snprintf(with_translit, sizeof with_translit, "%s//TRANSLIT", s->charset);
    if (n > 0 && (size_t)n < sizeof with_translit) {
        s->conv = iconv_open(with_translit, "UTF-8");
        if (s->conv != (iconv_t)-1) return;
    }
    s->conv = iconv_open(s->charset, "UTF-8");
    if (s->conv == (iconv_t)-1)
        die("iconv_open(%s, UTF-8): %s", s->charset, strerror(errno));
}

static size_t utf8_seq_len(unsigned char b, size_t available)
{
    size_t n;
    if      ((b & 0xE0) == 0xC0) n = 2;
    else if ((b & 0xF0) == 0xE0) n = 3;
    else if ((b & 0xF8) == 0xF0) n = 4;
    else                          n = 1;
    return n > available ? available : n;
}

/* Convert one UTF-8 input chunk to the output charset and split the
 * resulting bytes around spaces — routing space runs through add_space
 * (so wrap can drop them) and non-space runs through out_visible_run. */
static void route_bytes(state_t *s, const unsigned char *bytes, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (bytes[i] == ' ') {
            add_space(s);
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && bytes[j] != ' ') j++;
        out_visible_run(s, bytes + i, j - i);
        i = j;
    }
}

static void emit_converted(state_t *s, const char *in, size_t in_len)
{
    if (in_len == 0) return;
    unsigned char outbuf[4096];
    char *inp = (char *)in;
    size_t inb = in_len;
    while (inb > 0) {
        char *outp = (char *)outbuf;
        size_t outb = sizeof outbuf;
        size_t r = iconv(s->conv, &inp, &inb, &outp, &outb);
        size_t produced = (size_t)(outp - (char *)outbuf);
        if (produced > 0) route_bytes(s, outbuf, produced);
        if (r == (size_t)-1) {
            if (errno == E2BIG) continue;
            if (errno == EILSEQ || errno == EINVAL) {
                size_t skip = utf8_seq_len((unsigned char)*inp, inb);
                inp += skip;
                inb -= skip;
                route_bytes(s, (const unsigned char *)"?", 1);
                continue;
            }
            die("iconv: %s", strerror(errno));
        }
    }
}

/* ----- smiley shortcodes ----- */

typedef struct { const char *name; const char *ascii; } smiley_t;

static const smiley_t SMILEYS[] = {
    { "+1",                "(y)"  },
    { "-1",                "(n)"  },
    { "angry",             ">:("  },
    { "arrow_down",        "v"    },
    { "arrow_left",        "<-"   },
    { "arrow_right",       "->"   },
    { "arrow_up",          "^"    },
    { "ballot_box",        "[ ]"  },
    { "ballot_box_with_check", "[x]" },
    { "bulb",              "(*)"  },
    { "check",             "[x]"  },
    { "check_mark",        "v"    },
    { "clap",              "(clap)" },
    { "confused",          ":/"   },
    { "cross",             "[x]"  },
    { "cry",               ":'("  },
    { "disappointed",      ":("   },
    { "exclamation",       "!"    },
    { "eyes",              "O.O"  },
    { "fire",              "(fire)" },
    { "frown",             ":("   },
    { "grin",              ":D"   },
    { "grinning",          ":D"   },
    { "heart",             "<3"   },
    { "heart_eyes",        "<3.<3" },
    { "hooray",            "\\o/" },
    { "joy",               ":'D"  },
    { "kiss",              ":*"   },
    { "kissing_heart",     ":*"   },
    { "laughing",          "xD"   },
    { "lightning",         "(*)"  },
    { "minus",             "-"    },
    { "neutral_face",      ":|"   },
    { "no_entry",          "(no)" },
    { "ok",                "(ok)" },
    { "ok_hand",           "(ok)" },
    { "open_mouth",        ":O"   },
    { "party",             "\\o/" },
    { "pensive",           ":("   },
    { "plus",              "+"    },
    { "point_down",        "v"    },
    { "point_left",        "<-"   },
    { "point_right",       "->"   },
    { "point_up",          "^"    },
    { "question",          "?"    },
    { "rage",              ">:O"  },
    { "rocket",            "==>"  },
    { "sad",               ":("   },
    { "shrug",             "/shrug" },
    { "skull",             "X("   },
    { "slight_smile",      ":)"   },
    { "smile",             ":)"   },
    { "smiley",            ":D"   },
    { "smirk",             ";)"   },
    { "sob",               ":'("  },
    { "star",              "*"    },
    { "stuck_out_tongue",  ":P"   },
    { "stuck_out_tongue_winking_eye", ";P" },
    { "sunglasses",        "B)"   },
    { "tada",              "\\o/" },
    { "thinking",          ":?"   },
    { "thumbsdown",        "(n)"  },
    { "thumbsup",          "(y)"  },
    { "tongue",            ":P"   },
    { "unamused",          ":|"   },
    { "warning",           "[!]"  },
    { "wave",              "o/"   },
    { "wink",              ";)"   },
    { "x",                 "[x]"  },
    { "zzz",               "Zzz"  }
};

static int is_shortcode_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '+' || c == '-';
}

static const char *lookup_shortcode(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof SMILEYS / sizeof SMILEYS[0]; i++) {
        if (strlen(SMILEYS[i].name) == len
            && memcmp(SMILEYS[i].name, name, len) == 0)
            return SMILEYS[i].ascii;
    }
    return NULL;
}

static void emit_text(state_t *s, const char *t, size_t n)
{
    if (n == 0) return;
    if (!s->smileys) { emit_converted(s, t, n); return; }
    size_t i = 0, run_start = 0;
    while (i < n) {
        if (t[i] != ':') { i++; continue; }
        size_t name_start = i + 1;
        size_t end = name_start;
        while (end < n && is_shortcode_char(t[end])) end++;
        if (end < n && t[end] == ':' && end > name_start) {
            const char *ascii = lookup_shortcode(t + name_start, end - name_start);
            if (ascii) {
                if (i > run_start) emit_converted(s, t + run_start, i - run_start);
                emit_converted(s, ascii, strlen(ascii));
                i = end + 1;
                run_start = i;
                continue;
            }
        }
        i++;
    }
    if (run_start < n) emit_converted(s, t + run_start, n - run_start);
}

static void emit_codepoint(state_t *s, int cp)
{
    char buf[4];
    int n = 0;
    if (cp < 0)             { route_bytes(s, (const unsigned char *)"?", 1); return; }
    else if (cp < 0x80)     { buf[0] = (char)cp; n = 1; }
    else if (cp < 0x800)    { buf[0] = (char)(0xC0 | (cp >> 6));
                              buf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000)  { buf[0] = (char)(0xE0 | (cp >> 12));
                              buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                              buf[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else if (cp < 0x110000) { buf[0] = (char)(0xF0 | (cp >> 18));
                              buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
                              buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
                              buf[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    else                    { route_bytes(s, (const unsigned char *)"?", 1); return; }
    emit_converted(s, buf, (size_t)n);
}

/* Code-block / code-span text. md4c sends '\n' verbatim for code
 * blocks; everything else flows as visible-force bytes so word-wrap
 * does NOT touch the user's source code. */
static void emit_code_text(state_t *s, const char *t, size_t n)
{
    size_t i = 0, run_start = 0;
    while (i < n) {
        if (t[i] == '\n') {
            if (i > run_start) {
                /* Convert + force-emit the segment */
                unsigned char outbuf[4096];
                char *inp = (char *)(t + run_start);
                size_t inb = i - run_start;
                while (inb > 0) {
                    char *outp = (char *)outbuf;
                    size_t outb = sizeof outbuf;
                    size_t r = iconv(s->conv, &inp, &inb, &outp, &outb);
                    size_t produced = (size_t)(outp - (char *)outbuf);
                    if (produced > 0) out_force_visible(s, outbuf, produced);
                    if (r == (size_t)-1) {
                        if (errno == E2BIG) continue;
                        if (errno == EILSEQ || errno == EINVAL) {
                            size_t skip = utf8_seq_len((unsigned char)*inp, inb);
                            inp += skip; inb -= skip;
                            out_force_visible(s, (const unsigned char *)"?", 1);
                            continue;
                        }
                        die("iconv: %s", strerror(errno));
                    }
                }
            }
            raw_lf(s);
            run_start = i + 1;
        }
        i++;
    }
    if (run_start < n) {
        unsigned char outbuf[4096];
        char *inp = (char *)(t + run_start);
        size_t inb = n - run_start;
        while (inb > 0) {
            char *outp = (char *)outbuf;
            size_t outb = sizeof outbuf;
            size_t r = iconv(s->conv, &inp, &inb, &outp, &outb);
            size_t produced = (size_t)(outp - (char *)outbuf);
            if (produced > 0) out_force_visible(s, outbuf, produced);
            if (r == (size_t)-1) {
                if (errno == E2BIG) continue;
                if (errno == EILSEQ || errno == EINVAL) {
                    size_t skip = utf8_seq_len((unsigned char)*inp, inb);
                    inp += skip; inb -= skip;
                    out_force_visible(s, (const unsigned char *)"?", 1);
                    continue;
                }
                die("iconv: %s", strerror(errno));
            }
        }
    }
}

/* ----- entities ----- */

static int decode_entity(const char *t, size_t n)
{
    if (n < 2 || t[0] != '&' || t[n - 1] != ';') return -1;
    if (n > 3 && t[1] == '#') {
        int base = 10;
        size_t start = 2;
        if (t[2] == 'x' || t[2] == 'X') { base = 16; start = 3; }
        long cp = 0;
        for (size_t i = start; i < n - 1; i++) {
            char c = t[i];
            int d;
            if (c >= '0' && c <= '9')      d = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f') d = 10 + c - 'a';
            else if (base == 16 && c >= 'A' && c <= 'F') d = 10 + c - 'A';
            else return -1;
            cp = cp * base + d;
            if (cp > 0x10FFFF) return -1;
        }
        return (int)cp;
    }
    static const struct { const char *name; int cp; } map[] = {
        { "&amp;",   '&' }, { "&lt;",    '<' }, { "&gt;",    '>' },
        { "&quot;",  '"' }, { "&apos;",  '\'' },
        { "&nbsp;",  0x00A0 }, { "&copy;", 0x00A9 }, { "&reg;", 0x00AE },
        { "&trade;", 0x2122 }, { "&hellip;", 0x2026 },
        { "&mdash;", 0x2014 }, { "&ndash;", 0x2013 },
        { "&lsquo;", 0x2018 }, { "&rsquo;", 0x2019 },
        { "&ldquo;", 0x201C }, { "&rdquo;", 0x201D },
        { "&bull;",  0x2022 }
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        size_t L = strlen(map[i].name);
        if (L == n && memcmp(t, map[i].name, L) == 0) return map[i].cp;
    }
    return -1;
}

/* ----- md4c callbacks ----- */

static int cb_enter_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    state_t *s = userdata;
    switch (type) {
        case MD_BLOCK_DOC:
            if (!s->no_init) {
                escp_init(s->staging);          drain_staging(s);
                escp_quality(s->staging, s->body_quality); drain_staging(s);
                escp_cpi(s->staging, s->body_cpi);         drain_staging(s);
                escp_font(s->staging, s->body_font);       drain_staging(s);
                escp_lpi(s->staging, s->body_lpi);         drain_staging(s);
                if (s->left_margin  >= 0) { escp_left_margin (s->staging, s->left_margin);  drain_staging(s); }
                if (s->right_margin >= 0) { escp_right_margin(s->staging, s->right_margin); drain_staging(s); }
            }
            s->at_line_start = 1;
            break;

        case MD_BLOCK_QUOTE:
            flush_blanks(s);
            end_line(s);
            s->quote_depth++;
            push_italic(s);
            break;

        case MD_BLOCK_UL:
            if (s->list_depth < MAX_LIST_DEPTH) {
                s->lists[s->list_depth].is_ordered = 0;
                s->lists[s->list_depth].counter    = 0;
                s->list_depth++;
            }
            break;

        case MD_BLOCK_OL: {
            const MD_BLOCK_OL_DETAIL *d = detail;
            if (s->list_depth < MAX_LIST_DEPTH) {
                s->lists[s->list_depth].is_ordered = 1;
                s->lists[s->list_depth].counter    = (int)d->start - 1;
                s->list_depth++;
            }
            break;
        }

        case MD_BLOCK_LI: {
            flush_blanks(s);
            end_line(s);
            int depth = s->list_depth > 0 ? s->list_depth - 1 : 0;
            /* Indent for nesting (2 spaces per outer level). */
            for (int i = 0; i < depth; i++)
                out_force_visible(s, (const unsigned char *)"  ", 2);

            int bullet_len;
            char bullet[16];
            if (s->list_depth > 0 && s->lists[depth].is_ordered) {
                s->lists[depth].counter++;
                bullet_len = snprintf(bullet, sizeof bullet, "%d. ", s->lists[depth].counter);
                if (bullet_len < 0) bullet_len = 0;
            } else {
                bullet[0] = '*'; bullet[1] = ' '; bullet_len = 2;
            }
            out_force_visible(s, (const unsigned char *)bullet, (size_t)bullet_len);

            /* Continuation indent: enough to line up with the bullet's
             * content column. */
            push_indent(s, depth * 2 + bullet_len);
            break;
        }

        case MD_BLOCK_HR: {
            flush_blanks(s);
            end_line(s);
            int width = s->page_width > 0 ? s->page_width : HR_DEFAULT;
            for (int i = 0; i < width; i++)
                out_force_visible(s, (const unsigned char *)"-", 1);
            raw_lf(s);
            s->pending_blanks = 1;
            break;
        }

        case MD_BLOCK_H: {
            const MD_BLOCK_H_DETAIL *d = detail;
            flush_blanks(s);
            end_line(s);
            s->heading_level = (int)d->level;
            if (d->level == 1) {
                push_double_width(s);
                push_double_height(s);
                push_bold(s);
            } else if (d->level == 2) {
                push_double_width(s);
                push_bold(s);
            } else if (d->level == 3) {
                push_bold(s);
                push_underline(s);
            } else {
                push_bold(s);
            }
            break;
        }

        case MD_BLOCK_CODE:
            flush_blanks(s);
            end_line(s);
            s->in_code_block = 1;
            push_courier(s);
            push_condensed(s);
            push_draft(s);
            break;

        case MD_BLOCK_HTML:
            break;

        case MD_BLOCK_P:
            flush_blanks(s);
            break;

        case MD_BLOCK_TABLE:
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
            flush_blanks(s);
            end_line(s);
            break;
        case MD_BLOCK_TR:
            end_line(s);
            break;
        case MD_BLOCK_TH:
            push_bold(s);
            break;
        case MD_BLOCK_TD:
            break;

        default:
            break;
    }
    return 0;
}

static int cb_leave_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    state_t *s = userdata;
    (void)detail;
    switch (type) {
        case MD_BLOCK_DOC:
            end_line(s);
            if (!s->no_ff)   { escp_ff(s->staging);   drain_staging(s); }
            if (!s->no_init) { escp_init(s->staging); drain_staging(s); }
            break;

        case MD_BLOCK_QUOTE:
            end_line(s);
            pop_italic(s);
            s->quote_depth--;
            s->pending_blanks = 1;
            break;

        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            if (s->list_depth > 0) s->list_depth--;
            if (s->list_depth == 0) s->pending_blanks = 1;
            break;

        case MD_BLOCK_LI:
            end_line(s);
            pop_indent(s);
            break;

        case MD_BLOCK_HR:
            break;

        case MD_BLOCK_H:
            end_line(s);
            if (s->heading_level == 1) {
                pop_bold(s);
                pop_double_height(s);
                pop_double_width(s);
            } else if (s->heading_level == 2) {
                pop_bold(s);
                pop_double_width(s);
            } else if (s->heading_level == 3) {
                pop_underline(s);
                pop_bold(s);
            } else {
                pop_bold(s);
            }
            s->heading_level  = 0;
            s->pending_blanks = 1;
            break;

        case MD_BLOCK_CODE:
            end_line(s);
            pop_draft(s);
            pop_condensed(s);
            pop_courier(s);
            s->in_code_block  = 0;
            s->pending_blanks = 1;
            break;

        case MD_BLOCK_P:
            end_line(s);
            s->pending_blanks = 1;
            break;

        case MD_BLOCK_TABLE:
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
            end_line(s);
            s->pending_blanks = 1;
            break;
        case MD_BLOCK_TR:
            end_line(s);
            break;
        case MD_BLOCK_TH:
            pop_bold(s);
            /* FALLTHROUGH */
        case MD_BLOCK_TD:
            emit_text(s, " | ", 3);
            break;

        case MD_BLOCK_HTML:
            break;
    }
    return 0;
}

static int cb_enter_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    state_t *s = userdata;
    switch (type) {
        case MD_SPAN_EM:     push_italic(s); break;
        case MD_SPAN_STRONG: push_bold(s); break;
        case MD_SPAN_U:
        case MD_SPAN_DEL:    push_underline(s); break;
        case MD_SPAN_CODE:
            push_courier(s);
            push_condensed(s);
            break;
        case MD_SPAN_A: {
            const MD_SPAN_A_DETAIL *d = detail;
            s->link_active       = 1;
            s->link_is_autolink  = d->is_autolink;
            size_t L = d->href.size < sizeof s->link_href - 1
                     ? d->href.size : sizeof s->link_href - 1;
            memcpy(s->link_href, d->href.text, L);
            s->link_href[L] = 0;
            s->link_href_len = L;
            push_underline(s);
            break;
        }
        case MD_SPAN_IMG:
            push_italic(s);
            break;
        default:
            break;
    }
    return 0;
}

static int cb_leave_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    state_t *s = userdata;
    (void)detail;
    switch (type) {
        case MD_SPAN_EM:     pop_italic(s); break;
        case MD_SPAN_STRONG: pop_bold(s); break;
        case MD_SPAN_U:
        case MD_SPAN_DEL:    pop_underline(s); break;
        case MD_SPAN_CODE:
            pop_condensed(s);
            pop_courier(s);
            break;
        case MD_SPAN_A:
            pop_underline(s);
            if (s->link_urls && s->link_active && !s->link_is_autolink
                    && s->link_href_len > 0) {
                emit_text(s, " (", 2);
                emit_text(s, s->link_href, s->link_href_len);
                emit_text(s, ")", 1);
            }
            s->link_active       = 0;
            s->link_is_autolink  = 0;
            s->link_href_len     = 0;
            break;
        case MD_SPAN_IMG:
            pop_italic(s);
            break;
        default:
            break;
    }
    return 0;
}

static int cb_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata)
{
    state_t *s = userdata;
    switch (type) {
        case MD_TEXT_NULLCHAR:
            emit_text(s, "?", 1);
            break;
        case MD_TEXT_BR:
        case MD_TEXT_SOFTBR:
            end_line(s);
            break;
        case MD_TEXT_ENTITY: {
            int cp = decode_entity(text, size);
            if (cp >= 0) emit_codepoint(s, cp);
            else         emit_text(s, text, size);
            break;
        }
        case MD_TEXT_CODE:
            emit_code_text(s, text, size);
            break;
        case MD_TEXT_HTML:
            break;
        case MD_TEXT_NORMAL:
        default:
            emit_text(s, text, size);
            break;
    }
    return 0;
}

/* ----- input slurping ----- */

static char *read_all(FILE *fp, size_t *out_len)
{
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) die("out of memory");
    for (;;) {
        if (len == cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); die("out of memory"); }
            buf = nb;
        }
        size_t got = fread(buf + len, 1, cap - len, fp);
        len += got;
        if (got == 0) {
            if (ferror(fp)) { free(buf); die("read failed: %s", strerror(errno)); }
            break;
        }
    }
    *out_len = len;
    return buf;
}

static void render_buffer(state_t *s, const char *text, size_t len)
{
    s->pending_blanks = 0;
    s->at_line_start  = 1;
    s->column         = 0;
    s->pending_space  = 0;
    bb_reset(&s->word_buf);
    s->word_visible   = 0;
    s->indent_top     = 0;

    MD_PARSER parser;
    memset(&parser, 0, sizeof parser);
    parser.abi_version  = 0;
    parser.flags        = MD_DIALECT_GITHUB | MD_FLAG_COLLAPSEWHITESPACE;
    parser.enter_block  = cb_enter_block;
    parser.leave_block  = cb_leave_block;
    parser.enter_span   = cb_enter_span;
    parser.leave_span   = cb_leave_span;
    parser.text         = cb_text;

    if (md_parse(text, (MD_SIZE)len, &parser, s) != 0)
        die("markdown parser failed");
}

/* ----- CLI ----- */

static void usage(FILE *fp)
{
    fprintf(fp,
"Usage: %s [OPTIONS] [FILE...]\n"
"\n"
"Render Markdown to an ESC/P 2 byte stream suitable for piping to a\n"
"dot-matrix printer. With no FILE, reads from stdin.\n"
"\n"
"General:\n"
"  -o, --output FILE     write to FILE instead of stdout\n"
"  -i, --no-init         skip the ESC @ + body-style prologue\n"
"  -F, --no-ff           skip the trailing form feed\n"
"      --cpi N           body pitch: 10|12|15 (default 10)\n"
"      --font NAME       body font (default roman); see escp(1) --font\n"
"      --quality Q       draft|lq (default lq)\n"
"      --lpi N           lines per inch (default 6)\n"
"      --left-margin N   printer left margin (also used for software wrap)\n"
"      --right-margin N  printer right margin (also used for software wrap)\n"
"      --page-width N    override printable width used for word-wrap and the\n"
"                        thematic break. Implied by margins when both are set.\n"
"      --charset NAME    output character encoding (default ISO-8859-1).\n"
"                        Anything iconv(1) accepts: ISO-8859-15, ASCII,\n"
"                        CP437, US-ASCII, etc.\n"
"      --smileys         expand :shortcode: emoji to ASCII (:smile: -> :))\n"
"      --link-urls       after the underlined link text, also print\n"
"                        \" (url)\" for non-autolink references\n"
"  -h, --help            this help\n"
"  -V, --version         print version\n"
"\n"
"Examples:\n"
"  %s README.md | lp -d pp404\n"
"  cat doc.md | %s --cpi 12 --quality lq > pp404.bin\n",
        PROG_NAME, PROG_NAME, PROG_NAME);
}

enum {
    OPT_CPI = 256,
    OPT_FONT,
    OPT_QUALITY,
    OPT_LPI,
    OPT_LMARGIN,
    OPT_RMARGIN,
    OPT_PAGE_WIDTH,
    OPT_CHARSET,
    OPT_SMILEYS,
    OPT_LINK_URLS
};

static const struct option long_opts[] = {
    { "output",       required_argument, 0, 'o' },
    { "no-init",      no_argument,       0, 'i' },
    { "no-ff",        no_argument,       0, 'F' },
    { "cpi",          required_argument, 0, OPT_CPI },
    { "font",         required_argument, 0, OPT_FONT },
    { "quality",      required_argument, 0, OPT_QUALITY },
    { "lpi",          required_argument, 0, OPT_LPI },
    { "left-margin",  required_argument, 0, OPT_LMARGIN },
    { "right-margin", required_argument, 0, OPT_RMARGIN },
    { "page-width",   required_argument, 0, OPT_PAGE_WIDTH },
    { "charset",      required_argument, 0, OPT_CHARSET },
    { "smileys",      no_argument,       0, OPT_SMILEYS },
    { "link-urls",    no_argument,       0, OPT_LINK_URLS },
    { "help",         no_argument,       0, 'h' },
    { "version",      no_argument,       0, 'V' },
    { 0, 0, 0, 0 }
};

int main(int argc, char **argv)
{
    state_t s;
    memset(&s, 0, sizeof s);
    s.out          = stdout;
    s.body_cpi     = 10;
    s.body_font    = ESCP_FONT_ROMAN;
    s.body_quality = ESCP_QUALITY_LQ;
    s.body_lpi     = 6;
    s.left_margin  = -1;
    s.right_margin = -1;
    s.page_width   = 0;
    s.at_line_start = 1;
    s.charset      = "ISO-8859-1";
    s.conv         = (iconv_t)-1;

    s.staging = open_memstream(&s.staging_buf, &s.staging_buf_size);
    if (!s.staging) die("open_memstream: %s", strerror(errno));

    escp_set_progname(PROG_NAME);

    int explicit_page_width = 0;

    int c;
    while ((c = getopt_long(argc, argv, "ho:VFi", long_opts, NULL)) != -1) {
        switch (c) {
            case 'h': usage(stdout); return 0;
            case 'V': printf("%s %s\n", PROG_NAME, PROG_VERSION); return 0;
            case 'o':
                if (s.out != stdout) fclose(s.out);
                s.out = fopen(optarg, "wb");
                if (!s.out) die("cannot open '%s': %s", optarg, strerror(errno));
                break;
            case 'i': s.no_init = 1; break;
            case 'F': s.no_ff   = 1; break;
            case OPT_CPI:
                s.body_cpi = (int)parse_long(optarg, 10, 20, "--cpi");
                if (s.body_cpi != 10 && s.body_cpi != 12 && s.body_cpi != 15)
                    die("--cpi: only 10, 12 or 15 are supported (got %d)", s.body_cpi);
                break;
            case OPT_FONT: {
                int n = escp_font_from_name(optarg);
                if (n < 0) die("--font: unknown family '%s'", optarg);
                s.body_font = n;
                break;
            }
            case OPT_QUALITY: {
                int q = escp_quality_from_name(optarg);
                if (q < 0) die("--quality: expected 'draft' or 'lq', got '%s'", optarg);
                s.body_quality = q;
                break;
            }
            case OPT_LPI:
                s.body_lpi = (int)parse_long(optarg, 1, 360, "--lpi");
                break;
            case OPT_LMARGIN:
                s.left_margin = (int)parse_long(optarg, 0, 255, "--left-margin");
                break;
            case OPT_RMARGIN:
                s.right_margin = (int)parse_long(optarg, 1, 255, "--right-margin");
                break;
            case OPT_PAGE_WIDTH:
                s.page_width = (int)parse_long(optarg, 1, 1000, "--page-width");
                explicit_page_width = 1;
                break;
            case OPT_CHARSET:
                s.charset = optarg;
                break;
            case OPT_SMILEYS:
                s.smileys = 1;
                break;
            case OPT_LINK_URLS:
                s.link_urls = 1;
                break;
            case '?':
            default:
                fprintf(stderr, "Try '%s --help'\n", PROG_NAME);
                return 2;
        }
    }

    /* If the user gave margins but no explicit width, derive one. The
     * hardware margins still get emitted via the prologue as a safety
     * net; the software width drives word-wrap and the HR rule. */
    if (!explicit_page_width && s.left_margin >= 0 && s.right_margin > 0
            && s.right_margin > s.left_margin) {
        s.page_width = s.right_margin - s.left_margin;
    }

    open_iconv(&s);

    int files = argc - optind;
    if (files == 0) {
        size_t len = 0;
        char *buf = read_all(stdin, &len);
        render_buffer(&s, buf, len);
        free(buf);
    } else {
        for (int i = optind; i < argc; i++) {
            FILE *fp = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "rb");
            if (!fp) die("cannot open '%s': %s", argv[i], strerror(errno));
            size_t len = 0;
            char *buf = read_all(fp, &len);
            if (fp != stdin) fclose(fp);
            if (i > optind) {
                /* Form-feed between docs goes straight to the file. */
                flush_word(&s);
                escp_ff(s.out);
            }
            render_buffer(&s, buf, len);
        }
    }

    flush_word(&s);

    if (s.conv != (iconv_t)-1) iconv_close(s.conv);
    fclose(s.staging);
    free(s.staging_buf);
    bb_free(&s.word_buf);
    if (s.out != stdout) fclose(s.out);
    return 0;
}
