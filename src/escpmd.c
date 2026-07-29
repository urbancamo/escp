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
    char *content;        /* UTF-8 cell content */
    size_t len;
    size_t cap;
    int is_header;        /* bold header cell */
} table_cell_t;

typedef struct {
    table_cell_t *cells;
    int num_cells;
    int capacity;
} table_row_t;

typedef struct {
    table_row_t *rows;
    int num_rows;
    int num_cols;         /* max columns across all rows */
    int capacity;
    int in_table;         /* currently buffering a table */
    int in_header;        /* in THEAD section */
    int current_row;      /* index of row being built */
    int current_col;      /* index of cell being built */
} table_buffer_t;

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

    /* Table buffering. */
    table_buffer_t table;
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

/* ----- table buffer management ----- */

static void cell_append(table_cell_t *cell, const char *text, size_t len)
{
    if (len == 0) return;
    if (cell->len + len > cell->cap) {
        size_t nc = cell->cap ? cell->cap : 64;
        while (nc < cell->len + len) nc *= 2;
        char *p = realloc(cell->content, nc);
        if (!p) die("out of memory");
        cell->content = p;
        cell->cap = nc;
    }
    memcpy(cell->content + cell->len, text, len);
    cell->len += len;
}

static void table_init_row(table_buffer_t *tbl)
{
    if (tbl->num_rows >= tbl->capacity) {
        int nc = tbl->capacity ? tbl->capacity * 2 : 4;
        table_row_t *p = realloc(tbl->rows, (size_t)nc * sizeof(table_row_t));
        if (!p) die("out of memory");
        tbl->rows = p;
        tbl->capacity = nc;
    }
    table_row_t *row = &tbl->rows[tbl->num_rows];
    memset(row, 0, sizeof *row);
}

static void table_finish_cell(table_buffer_t *tbl, int is_header)
{
    if (tbl->num_rows == 0) return;
    table_row_t *row = &tbl->rows[tbl->num_rows - 1];

    if (row->num_cells >= row->capacity) {
        int nc = row->capacity ? row->capacity * 2 : 4;
        table_cell_t *p = realloc(row->cells, (size_t)nc * sizeof(table_cell_t));
        if (!p) die("out of memory");
        row->cells = p;
        row->capacity = nc;
    }

    table_cell_t *cell = &row->cells[row->num_cells];
    memset(cell, 0, sizeof *cell);
    cell->is_header = is_header;
    row->num_cells++;

    if (row->num_cells > tbl->num_cols)
        tbl->num_cols = row->num_cells;
}

static void table_free(table_buffer_t *tbl)
{
    for (int i = 0; i < tbl->num_rows; i++) {
        table_row_t *row = &tbl->rows[i];
        for (int j = 0; j < row->num_cells; j++) {
            free(row->cells[j].content);
        }
        free(row->cells);
    }
    free(tbl->rows);
    memset(tbl, 0, sizeof *tbl);
}

static table_cell_t *table_get_cell(table_buffer_t *tbl, int row, int col)
{
    if (row < 0 || row >= tbl->num_rows) return NULL;
    table_row_t *r = &tbl->rows[row];
    if (col < 0 || col >= r->num_cells) return NULL;
    return &r->cells[col];
}

/* Forward declaration for utf8_seq_len (defined later in iconv section). */
static size_t utf8_seq_len(unsigned char b, size_t available);

/* Measure visible width of UTF-8 text after conversion to target charset. */
static int measure_text_width(state_t *s, const char *utf8, size_t utf8_len)
{
    if (utf8_len == 0) return 0;

    unsigned char outbuf[4096];
    char *inp = (char *)utf8;
    size_t inb = utf8_len;
    int total = 0;

    while (inb > 0) {
        char *outp = (char *)outbuf;
        size_t outb = sizeof outbuf;
        size_t r = iconv(s->conv, &inp, &inb, &outp, &outb);
        size_t produced = (size_t)(outp - (char *)outbuf);
        total += (int)produced;

        if (r == (size_t)-1) {
            if (errno == E2BIG) continue;
            if (errno == EILSEQ || errno == EINVAL) {
                size_t skip = utf8_seq_len((unsigned char)*inp, inb);
                inp += skip;
                inb -= skip;
                total++;  /* replacement '?' */
                continue;
            }
            break;
        }
    }
    return total;
}

/* Measure width of each column across all rows. */
static void measure_table_columns(state_t *s, table_buffer_t *tbl, int *col_widths)
{
    for (int col = 0; col < tbl->num_cols; col++) {
        col_widths[col] = 0;
        for (int row = 0; row < tbl->num_rows; row++) {
            table_cell_t *cell = table_get_cell(tbl, row, col);
            if (cell && cell->content) {
                int w = measure_text_width(s, cell->content, cell->len);
                if (w > col_widths[col])
                    col_widths[col] = w;
            }
        }
    }
}

/* Calculate total table width: sum of column widths + separators " | ". */
static int calculate_table_width(int num_cols, const int *col_widths)
{
    if (num_cols == 0) return 0;
    int total = 0;
    for (int i = 0; i < num_cols; i++)
        total += col_widths[i];
    /* Add separator width: " | " between each column (3 chars each). */
    if (num_cols > 1)
        total += (num_cols - 1) * 3;
    return total;
}

/* Maximum column width when table doesn't fit (characters). */
#define MAX_COL_WIDTH_CONSTRAINED 30
/* Maximum column width when space is available (characters). */
#define MAX_COL_WIDTH_UNCONSTRAINED 100

/* Structure to hold wrapped lines for a cell. */
typedef struct {
    char **lines;
    int num_lines;
    int capacity;
} wrapped_cell_t;

/* Split text into lines at word boundaries, each line ≤ max_width. */
static void wrap_text_to_lines(state_t *s, const char *utf8, size_t utf8_len,
                                int max_width, wrapped_cell_t *out)
{
    if (utf8_len == 0) {
        out->lines = NULL;
        out->num_lines = 0;
        out->capacity = 0;
        return;
    }

    /* Convert to target charset first. */
    unsigned char converted[4096];
    char *inp = (char *)utf8;
    size_t inb = utf8_len;
    size_t converted_len = 0;

    while (inb > 0 && converted_len < sizeof converted - 1) {
        char *outp = (char *)(converted + converted_len);
        size_t outb = sizeof converted - converted_len;
        size_t r = iconv(s->conv, &inp, &inb, &outp, &outb);
        converted_len = (size_t)(outp - (char *)converted);
        if (r == (size_t)-1) {
            if (errno == E2BIG) break;
            if (errno == EILSEQ || errno == EINVAL) {
                size_t skip = utf8_seq_len((unsigned char)*inp, inb);
                inp += skip;
                inb -= skip;
                if (converted_len < sizeof converted - 1) {
                    converted[converted_len++] = '?';
                }
                continue;
            }
            break;
        }
    }
    converted[converted_len] = '\0';

    /* Wrap the converted text into lines. */
    out->lines = NULL;
    out->num_lines = 0;
    out->capacity = 0;

    size_t pos = 0;
    while (pos < converted_len) {
        /* Find end of line (up to max_width or word boundary). */
        size_t line_end = pos;
        size_t last_space = pos;
        int found_space = 0;

        while (line_end < converted_len && (int)(line_end - pos) < max_width) {
            if (converted[line_end] == ' ') {
                last_space = line_end;
                found_space = 1;
            }
            line_end++;
        }

        /* If we hit max_width mid-word, break at last space. */
        if (line_end < converted_len && found_space) {
            line_end = last_space;
        }

        /* Skip leading spaces on continuation lines. */
        while (pos < line_end && converted[pos] == ' ' && out->num_lines > 0) {
            pos++;
        }

        /* Extract line. */
        size_t line_len = line_end - pos;
        if (line_len > 0 || out->num_lines == 0) {
            if (out->num_lines >= out->capacity) {
                int nc = out->capacity ? out->capacity * 2 : 2;
                char **p = realloc(out->lines, (size_t)nc * sizeof(char *));
                if (!p) die("out of memory");
                out->lines = p;
                out->capacity = nc;
            }

            char *line = malloc(line_len + 1);
            if (!line) die("out of memory");
            memcpy(line, converted + pos, line_len);
            line[line_len] = '\0';
            out->lines[out->num_lines++] = line;
        }

        pos = line_end;
        /* Skip the space that caused the break. */
        if (pos < converted_len && converted[pos] == ' ') {
            pos++;
        }
    }

    /* Ensure at least one line. */
    if (out->num_lines == 0) {
        out->lines = malloc(sizeof(char *));
        if (!out->lines) die("out of memory");
        out->lines[0] = malloc(1);
        if (!out->lines[0]) die("out of memory");
        out->lines[0][0] = '\0';
        out->num_lines = 1;
        out->capacity = 1;
    }
}

static void free_wrapped_cell(wrapped_cell_t *wc)
{
    for (int i = 0; i < wc->num_lines; i++) {
        free(wc->lines[i]);
    }
    free(wc->lines);
}

/* Determine effective page width for a given CPI. */
static int get_effective_page_width(state_t *s, int cpi)
{
    /* If user set page_width explicitly, use it as-is (assumes it matches their CPI). */
    if (s->page_width > 0) return s->page_width;

    /* Otherwise assume wide-carriage width (~13.2" printable width). */
    if (cpi == 10) return 132;
    if (cpi == 12) return 158;
    if (cpi == 15) return 198;
    return 132;  /* fallback */
}

/* Determine table rendering strategy: which CPI and whether to use condensed.
 * Returns 1 if condensed should be used, and sets *cpi_out to the CPI to use. */
static int determine_table_style(state_t *s, int table_width, int *cpi_out)
{
    int cpi = s->body_cpi;

    /* Try current CPI without condensed. */
    int avail = get_effective_page_width(s, cpi);
    if (table_width <= avail) {
        *cpi_out = cpi;
        return 0;
    }

    /* Try condensed mode (approx 2x compression). */
    if (table_width <= avail * 2) {
        *cpi_out = cpi;
        return 1;
    }

    /* Try 12 CPI (if not already). */
    if (cpi != 12) {
        avail = get_effective_page_width(s, 12);
        if (table_width <= avail) {
            *cpi_out = 12;
            return 0;
        }
        if (table_width <= avail * 2) {
            *cpi_out = 12;
            return 1;
        }
    }

    /* Try 15 CPI (if not already). */
    if (cpi != 15) {
        avail = get_effective_page_width(s, 15);
        if (table_width <= avail) {
            *cpi_out = 15;
            return 0;
        }
        if (table_width <= avail * 2) {
            *cpi_out = 15;
            return 1;
        }
    }

    /* Give up: use 15 CPI + condensed and let it overflow. */
    *cpi_out = 15;
    return 1;
}

/* Flush the buffered table with proper alignment and auto-sizing. */
static void flush_table(state_t *s)
{
    table_buffer_t *tbl = &s->table;
    if (tbl->num_rows == 0 || tbl->num_cols == 0) {
        table_free(tbl);
        return;
    }

    /* Measure natural column widths. */
    int *col_widths = calloc((size_t)tbl->num_cols, sizeof(int));
    if (!col_widths) die("out of memory");
    measure_table_columns(s, tbl, col_widths);

    /* Calculate available width and distribute space intelligently. */
    int available_width = get_effective_page_width(s, s->body_cpi);

    /* First cap at unconstrained max and check if it fits. */
    for (int col = 0; col < tbl->num_cols; col++) {
        if (col_widths[col] > MAX_COL_WIDTH_UNCONSTRAINED)
            col_widths[col] = MAX_COL_WIDTH_UNCONSTRAINED;
    }

    int table_width = calculate_table_width(tbl->num_cols, col_widths);

    /* If table doesn't fit, distribute available space intelligently. */
    if (table_width > available_width) {
        /* Calculate separator overhead. */
        int separator_width = (tbl->num_cols - 1) * 3;
        int space_for_content = available_width - separator_width;

        /* Count columns wider than constrained limit. */
        int wide_cols = 0;
        int narrow_total = 0;
        for (int col = 0; col < tbl->num_cols; col++) {
            if (col_widths[col] > MAX_COL_WIDTH_CONSTRAINED) {
                wide_cols++;
            } else {
                narrow_total += col_widths[col];
            }
        }

        /* Distribute remaining space to wide columns. */
        if (wide_cols > 0 && space_for_content > narrow_total) {
            int space_for_wide = space_for_content - narrow_total;
            int width_per_wide = space_for_wide / wide_cols;

            /* Ensure each wide column gets at least CONSTRAINED limit. */
            if (width_per_wide < MAX_COL_WIDTH_CONSTRAINED)
                width_per_wide = MAX_COL_WIDTH_CONSTRAINED;

            for (int col = 0; col < tbl->num_cols; col++) {
                if (col_widths[col] > MAX_COL_WIDTH_CONSTRAINED) {
                    col_widths[col] = width_per_wide;
                }
            }
        } else {
            /* Fallback: apply blanket constraint. */
            for (int col = 0; col < tbl->num_cols; col++) {
                if (col_widths[col] > MAX_COL_WIDTH_CONSTRAINED)
                    col_widths[col] = MAX_COL_WIDTH_CONSTRAINED;
            }
        }

        table_width = calculate_table_width(tbl->num_cols, col_widths);
    }

    /* Determine style adjustments (CPI, condensed). */
    int temp_cpi;
    int use_condensed = determine_table_style(s, table_width, &temp_cpi);

    /* Apply temporary styles. */
    int changed_cpi = (temp_cpi != s->body_cpi);
    if (changed_cpi) {
        escp_cpi(s->staging, temp_cpi);
        drain_staging(s);
    }
    if (use_condensed && s->condensed_depth == 0) {
        escp_condensed(s->staging);
        drain_staging(s);
    }
    /* Use Courier font for tables. */
    int changed_font = (s->body_font != ESCP_FONT_COURIER && s->courier_depth == 0);
    if (changed_font) {
        escp_font(s->staging, ESCP_FONT_COURIER);
        drain_staging(s);
    }

    /* Emit each row with word wrapping. */
    for (int row = 0; row < tbl->num_rows; row++) {
        table_row_t *r = &tbl->rows[row];

        /* Wrap all cells in this row. */
        wrapped_cell_t *wrapped = calloc((size_t)tbl->num_cols, sizeof(wrapped_cell_t));
        if (!wrapped) die("out of memory");

        int max_lines = 0;
        for (int col = 0; col < tbl->num_cols; col++) {
            table_cell_t *cell = (col < r->num_cells) ? &r->cells[col] : NULL;
            const char *content = (cell && cell->content) ? cell->content : "";
            size_t content_len = (cell && cell->content) ? cell->len : 0;

            wrap_text_to_lines(s, content, content_len, col_widths[col], &wrapped[col]);
            if (wrapped[col].num_lines > max_lines)
                max_lines = wrapped[col].num_lines;
        }

        /* Emit each line of this row. */
        for (int line = 0; line < max_lines; line++) {
            for (int col = 0; col < tbl->num_cols; col++) {
                table_cell_t *cell = (col < r->num_cells) ? &r->cells[col] : NULL;
                int is_header = (cell && cell->is_header);

                /* Apply bold for header cells. */
                if (is_header && line == 0 && s->bold_depth == 0) {
                    escp_bold(s->staging, 1);
                    drain_staging(s);
                }

                /* Get the text for this line (or empty string). */
                const char *line_text = "";
                int line_len = 0;
                if (line < wrapped[col].num_lines) {
                    line_text = wrapped[col].lines[line];
                    line_len = (int)strlen(line_text);
                }

                /* Emit cell content. */
                if (line_len > 0) {
                    out_force_visible(s, (const unsigned char *)line_text, (size_t)line_len);
                }

                /* Pad to column width (except last column). */
                int is_last = (col == tbl->num_cols - 1);
                if (!is_last) {
                    int padding = col_widths[col] - line_len;
                    for (int i = 0; i < padding; i++)
                        out_force_visible(s, (const unsigned char *)" ", 1);
                }

                /* Remove bold for header cells. */
                if (is_header && line == max_lines - 1 && s->bold_depth == 1) {
                    escp_bold(s->staging, 0);
                    drain_staging(s);
                }

                /* Emit separator " | " except after last column. */
                if (!is_last) {
                    out_force_visible(s, (const unsigned char *)" | ", 3);
                }
            }
            raw_lf(s);
        }

        /* Free wrapped cells. */
        for (int col = 0; col < tbl->num_cols; col++) {
            free_wrapped_cell(&wrapped[col]);
        }
        free(wrapped);
    }

    /* Restore original styles. */
    if (changed_font) {
        escp_font(s->staging, s->body_font);
        drain_staging(s);
    }
    if (use_condensed && s->condensed_depth == 0) {
        escp_cancel_condensed(s->staging);
        drain_staging(s);
    }
    if (changed_cpi) {
        escp_cpi(s->staging, s->body_cpi);
        drain_staging(s);
    }

    free(col_widths);
    table_free(tbl);
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

/* Route UTF-8 text to either the current table cell or normal output. */
static void route_text(state_t *s, const char *t, size_t n)
{
    if (n == 0) return;

    /* If buffering a table, append UTF-8 text to the current cell. */
    if (s->table.in_table && s->table.num_rows > 0) {
        int row_idx = s->table.num_rows - 1;
        if (row_idx >= 0 && row_idx < s->table.num_rows) {
            table_row_t *row = &s->table.rows[row_idx];
            if (row->num_cells > 0) {
                int cell_idx = row->num_cells - 1;
                if (cell_idx >= 0 && cell_idx < row->num_cells) {
                    cell_append(&row->cells[cell_idx], t, n);
                    return;
                }
            }
        }
    }

    /* Otherwise emit normally (with charset conversion). */
    emit_converted(s, t, n);
}

static void emit_text(state_t *s, const char *t, size_t n)
{
    if (n == 0) return;
    if (!s->smileys) { route_text(s, t, n); return; }
    size_t i = 0, run_start = 0;
    while (i < n) {
        if (t[i] != ':') { i++; continue; }
        size_t name_start = i + 1;
        size_t end = name_start;
        while (end < n && is_shortcode_char(t[end])) end++;
        if (end < n && t[end] == ':' && end > name_start) {
            const char *ascii = lookup_shortcode(t + name_start, end - name_start);
            if (ascii) {
                if (i > run_start) route_text(s, t + run_start, i - run_start);
                route_text(s, ascii, strlen(ascii));
                i = end + 1;
                run_start = i;
                continue;
            }
        }
        i++;
    }
    if (run_start < n) route_text(s, t + run_start, n - run_start);
}

static void emit_codepoint(state_t *s, int cp)
{
    char buf[4];
    int n = 0;
    if (cp < 0)             { route_text(s, "?", 1); return; }
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
    else                    { route_text(s, "?", 1); return; }
    route_text(s, buf, (size_t)n);
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
            flush_blanks(s);
            end_line(s);
            memset(&s->table, 0, sizeof s->table);
            s->table.in_table = 1;
            break;

        case MD_BLOCK_THEAD:
            s->table.in_header = 1;
            break;

        case MD_BLOCK_TBODY:
            s->table.in_header = 0;
            break;

        case MD_BLOCK_TR:
            table_init_row(&s->table);
            s->table.num_rows++;
            s->table.current_col = 0;
            break;

        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            table_finish_cell(&s->table, type == MD_BLOCK_TH || s->table.in_header);
            s->table.current_col++;
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
            flush_table(s);
            s->pending_blanks = 1;
            break;

        case MD_BLOCK_THEAD:
            s->table.in_header = 0;
            break;

        case MD_BLOCK_TBODY:
            break;

        case MD_BLOCK_TR:
            break;

        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
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
