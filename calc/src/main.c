/*
 *--------------------------------------
 * Program Name: SAXCE
 * Description:  Cemetech SAX chat client for the TI-84 Plus CE.
 *               Talks to the SAX-CE PC bridge over USB (CDC serial).
 *
 * Wire protocol (ASCII, one message per '\n'-terminated line):
 *   calc -> PC : "H <nick>"            hello / nick change
 *                "M <text>"            say something
 *   PC -> calc : "M <nick>\t<text>"    chat message
 *                "S <text>"            status / system notice
 *                "R"                   request a hello (sent when the bridge opens the port)
 *
 * Typing: the green alpha letters work as usual ([alpha] cycles abc/ABC/123),
 * and [2nd] toggles an on-screen keyboard driven by the arrow keys.
 * Fonts: [y=] cycles the chat log through any fontlibc font packs
 * (AppVars starting with "FONTPACK") found on the calculator.
 *--------------------------------------
 */

#include <fileioc.h>
#include <fontlibc.h>
#include <graphx.h>
#include <srldrvce.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ti/getcsc.h>
#include <ti/screen.h>
#include <usbdrvce.h>

/* ---------- layout ---------- */
/* Everything is laid out from the current font's height (see the layout
 * helpers after the font code), so all text follows the chosen font. */
#define SCREEN_W     320
#define SCREEN_H     240
#define CHAR_W       8
#define LINE_H       10
#define OSK_ROWS     5

/* ---------- palette indices ---------- */
#define C_BG       1
#define C_HEADER   2
#define C_TEXT     3
#define C_NICK     4
#define C_STATUS   5
#define C_SELF     6
#define C_DIM      7
#define C_OK       8
#define C_BAD      9
#define C_KEY      10
#define C_KEYSEL   11
#define C_KEYON    12
#define C_TRANSP   254

/* ---------- limits ---------- */
#define NICK_MAX   8      /* under EFnet's 9, leaving room for a fallback digit */
#define WHO_MAX    15     /* longest nick we display for other people */
#define TEXT_MAX   200
#define LOG_MAX    48
#define RX_MAX     (WHO_MAX + TEXT_MAX + 8)

#define CFG_VAR    "SAXCFG"

typedef struct {
    char kind;                   /* 'M' chat, 'S' status, 'E' echo of own msg */
    char nick[WHO_MAX + 1];
    char text[TEXT_MAX + 1];
} entry_t;

static entry_t log_buf[LOG_MAX];
static uint8_t log_head;         /* index of next slot */
static uint8_t log_count;
static int     scroll;           /* rows scrolled up from bottom */

static char nick[NICK_MAX + 1];
static char input[TEXT_MAX + 1];
static uint8_t input_len;
static uint8_t mode;             /* alpha keys: 0 = abc, 1 = ABC, 2 = 123 */

static bool osk_on;
static uint8_t osk_layer;        /* 0 = lower, 1 = upper, 2 = symbols */
static uint8_t osk_row = 1, osk_col;

static srl_device_t srl;
static uint8_t srl_buf[512];
static bool has_srl;
static bool hello_sent;
static char rx_line[RX_MAX + 1];
static uint16_t rx_len;
static bool dirty = true;

/* ---------- fonts ---------- */

#define FONTS_MAX 24
typedef struct {
    char pack[9];     /* AppVar name; "" = built-in */
    uint8_t index;    /* font within the pack */
    uint8_t height;
} font_ref_t;

static font_ref_t fonts[FONTS_MAX];
static uint8_t font_count = 1;   /* fonts[0] is the built-in GraphX font */
static uint8_t cur_font;
static uint8_t line_h = LINE_H;

static bool using_pack(void) { return cur_font != 0; }

static void scan_fonts(void) {
    void *pos = NULL;
    char *name;
    font_count = 1;
    fonts[0].pack[0] = '\0';
    fonts[0].height = 8;
    while ((name = ti_Detect(&pos, "FONTPACK")) && font_count < FONTS_MAX) {
        uint8_t h = ti_Open(name, "r");
        if (!h) continue;
        const fontlib_font_pack_t *pack = ti_GetDataPtr(h);
        uint8_t n = pack->fontCount;
        ti_Close(h);
        for (uint8_t i = 0; i < n && font_count < FONTS_MAX; i++) {
            fontlib_font_t *f = fontlib_GetFontByIndex(name, i);
            if (!f || f->height < 6 || f->height > 24) continue;
            font_ref_t *r = &fonts[font_count++];
            strncpy(r->pack, name, 8);
            r->pack[8] = '\0';
            r->index = i;
            r->height = f->height;
        }
    }
}

/* (Re)load the current font. Must be redone after any file write, since
 * that can move AppVars and invalidate fontlib's pointers. */
static void apply_font(void) {
    if (cur_font >= font_count) cur_font = 0;
    if (using_pack()) {
        fontlib_font_t *f = fontlib_GetFontByIndex(fonts[cur_font].pack, fonts[cur_font].index);
        if (f && fontlib_SetFont(f, 0)) {
            fontlib_SetWindowFullScreen();
            fontlib_SetTransparency(true);
            fontlib_SetNewlineOptions(0);
            line_h = fontlib_GetCurrentFontHeight();
            if (line_h < 8) line_h = 8;
            return;
        }
        cur_font = 0;
    }
    line_h = LINE_H;
}

static uint8_t glyph_w(char c) {
    if (!using_pack()) return CHAR_W;
    if (!fontlib_ValidateCodePoint(c)) c = '?';
    return fontlib_GetGlyphWidth(c);
}

static void draw_glyph(int x, int y, char c, uint8_t color) {
    if (using_pack()) {
        if (!fontlib_ValidateCodePoint(c)) c = '?';
        fontlib_SetForegroundColor(color);
        fontlib_SetCursorPosition((unsigned int)x, (uint8_t)y);
        fontlib_DrawGlyph((uint8_t)c);
    } else {
        gfx_SetTextFGColor(color);
        gfx_SetTextXY(x, y);
        gfx_PrintChar(c);
    }
}

static int text_w(const char *s) {
    int w = 0;
    while (*s) w += glyph_w(*s++);
    return w;
}

/* Draw a string in the current font, stopping before max_x. Returns the end x. */
static int draw_text_clip(const char *s, int x, int y, uint8_t color, int max_x) {
    for (; *s; s++) {
        uint8_t w = glyph_w(*s);
        if (x + w > max_x) break;
        draw_glyph(x, y, *s, color);
        x += w;
    }
    return x;
}

static int draw_text(const char *s, int x, int y, uint8_t color) {
    return draw_text_clip(s, x, y, color, SCREEN_W);
}

/* Draw a paragraph wrapped to the screen width; returns the y below it. */
static int draw_para(const char *s, int x, int y, uint8_t color) {
    int cx = x;
    for (; *s; s++) {
        uint8_t w = glyph_w(*s);
        if (cx + w > SCREEN_W - 2 && cx > x) {
            cx = x;
            y += line_h;
        }
        draw_glyph(cx, y, *s, color);
        cx += w;
    }
    return y + line_h;
}

/* ---------- layout (from the font height) ---------- */

static int text_h(void)    { return using_pack() ? fonts[cur_font].height : 8; }
static int header_h(void)  { return text_h() + 4; }
static int log_y(void)     { return header_h() + 2; }
static int osk_row_h(void) { int h = text_h() + 5; return h < 17 ? 17 : h; }
static int osk_y(void)     { return SCREEN_H - OSK_ROWS * osk_row_h(); }
/* input line; with the keyboard hidden a hint line sits under it */
static int input_y(void)   { return osk_on ? osk_y() - line_h - 3 : SCREEN_H - 2 * line_h - 6; }
static int log_area_h(void) { return input_y() - 4 - log_y(); }
static int log_rows(void)  { int r = log_area_h() / line_h; return r < 1 ? 1 : r; }

/* ---------- log ---------- */

static void copy_str(char *dst, const char *src, size_t max) {
    size_t i = 0;
    for (; src[i] && i < max; i++) {
        char c = src[i];
        dst[i] = (c >= ' ' && c <= '~') ? c : '?';
    }
    dst[i] = '\0';
}

static void log_add(char kind, const char *who, const char *text) {
    entry_t *e = &log_buf[log_head];
    e->kind = kind;
    copy_str(e->nick, who ? who : "", WHO_MAX);
    copy_str(e->text, text, TEXT_MAX);
    log_head = (log_head + 1) % LOG_MAX;
    if (log_count < LOG_MAX) log_count++;
    scroll = 0;
    dirty = true;
}

/* Rendered line: "nick: text", or just text for status lines. */
static size_t entry_prefix(const entry_t *e) {
    return e->kind == 'S' ? 0 : strlen(e->nick) + 2;
}


static char entry_char(const entry_t *e, size_t i, uint8_t *color) {
    size_t pre = entry_prefix(e);
    if (i < pre) {
        *color = e->kind == 'E' ? C_SELF : C_NICK;
        size_t nl = pre - 2;
        if (i < nl) return e->nick[i];
        return i == nl ? ':' : ' ';
    }
    *color = e->kind == 'S' ? C_STATUS : C_TEXT;
    return e->text[i - pre];
}

#define WRAP_W (SCREEN_W - 2)

/* Lay out an entry, wrapping by pixel width. If draw, draws the rows that
 * fall inside the visible log area, with the entry's first row at log row
 * `top`. Returns the number of rows. */
static int entry_layout(const entry_t *e, int top, bool draw, int visible) {
    size_t len = entry_prefix(e) + strlen(e->text);
    int row = 0, x = 1;
    for (size_t i = 0; i < len; i++) {
        uint8_t color;
        char ch = entry_char(e, i, &color);
        uint8_t w = glyph_w(ch);
        if (x + w > WRAP_W && x > 1) {
            row++;
            x = 1;
        }
        int r = top + row;
        if (draw && r >= 0 && r < visible) draw_glyph(x, log_y() + r * line_h, ch, color);
        x += w;
    }
    return row + 1;
}

static int entry_rows(const entry_t *e) { return entry_layout(e, 0, false, 0); }

static int total_rows(void) {
    int rows = 0;
    for (uint8_t k = 0; k < log_count; k++) {
        uint8_t idx = (log_head + LOG_MAX - 1 - k) % LOG_MAX;
        rows += entry_rows(&log_buf[idx]);
    }
    return rows;
}

/* ---------- nick rules (IRC) ---------- */

static bool nick_char_ok(char c, bool first) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    if (strchr("[]\\`_^{|}", c) && c) return true;
    if (first) return false;
    return (c >= '0' && c <= '9') || c == '-';
}

static bool nick_valid(const char *s) {
    if (!s[0] || strlen(s) > NICK_MAX) return false;
    for (size_t i = 0; s[i]; i++)
        if (!nick_char_ok(s[i], i == 0)) return false;
    return true;
}

/* ---------- on-screen keyboard ---------- */

enum { K_SHIFT = 1, K_SYM, K_SPACE, K_DEL, K_SEND };

static const char *const osk_chars[3][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?" },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:!" },
    { "!@#$%^&*()", "-_=+[]{}\\|", "~`<>/;:\"'?", ".,0123456@" },
};

/* bottom row: special keys with their widths */
static const struct { uint8_t code; uint8_t w; } osk_special[] = {
    { K_SHIFT, 64 }, { K_SYM, 48 }, { K_SPACE, 112 }, { K_DEL, 48 }, { K_SEND, 48 },
};
#define OSK_SPECIALS 5

static uint8_t osk_row_len(uint8_t row) { return row == 4 ? OSK_SPECIALS : 10; }

static void osk_key_span(uint8_t row, uint8_t col, int *x, int *w) {
    if (row < 4) { *x = col * 32; *w = 32; return; }
    int xx = 0;
    for (uint8_t i = 0; i < col; i++) xx += osk_special[i].w;
    *x = xx;
    *w = osk_special[col].w;
}

/* Move vertically to the key under the current key's centre. */
static void osk_move_row(int dir) {
    int x, w;
    osk_key_span(osk_row, osk_col, &x, &w);
    int centre = x + w / 2;
    osk_row = (uint8_t)((osk_row + OSK_ROWS + dir) % OSK_ROWS);
    for (uint8_t c = 0; c < osk_row_len(osk_row); c++) {
        osk_key_span(osk_row, c, &x, &w);
        if (centre >= x && centre < x + w) { osk_col = c; return; }
    }
    osk_col = osk_row_len(osk_row) - 1;
}

static void draw_osk(bool nick_mode) {
    int rh = osk_row_h(), oy = osk_y(), ty = (rh - text_h()) / 2;
    for (uint8_t r = 0; r < OSK_ROWS; r++) {
        int y = oy + r * rh;
        for (uint8_t c = 0; c < osk_row_len(r); c++) {
            int x, w;
            osk_key_span(r, c, &x, &w);
            bool sel = r == osk_row && c == osk_col;
            bool on = r == 4 && ((osk_special[c].code == K_SHIFT && osk_layer == 1) ||
                                 (osk_special[c].code == K_SYM && osk_layer == 2));
            gfx_SetColor(sel ? C_KEYSEL : on ? C_KEYON : C_KEY);
            gfx_FillRectangle(x + 1, y + 1, w - 2, rh - 2);
            uint8_t fg = sel ? C_BG : C_TEXT;
            if (r < 4) {
                char ch = osk_chars[osk_layer][r][c];
                draw_glyph(x + (w - glyph_w(ch)) / 2, y + ty, ch, fg);
            } else {
                const char *label, *shrt;
                switch (osk_special[c].code) {
                    case K_SHIFT: label = "Shift"; shrt = "Sh"; break;
                    case K_SYM:   label = shrt = osk_layer == 2 ? "abc" : "Sym"; break;
                    case K_SPACE: label = "Space"; shrt = "Spc"; break;
                    case K_DEL:   label = shrt = "Del"; break;
                    default:      label = shrt = nick_mode ? "OK" : "Send"; break;
                }
                if (text_w(label) > w - 4) label = shrt;
                int lw = text_w(label);
                draw_text_clip(label, x + (w - lw > 4 ? (w - lw) / 2 : 2), y + ty, fg, x + w - 1);
            }
        }
    }
}

/* ---------- drawing ---------- */

static void setup_palette(void) {
    gfx_palette[C_BG]     = gfx_RGBTo1555(16, 18, 28);
    gfx_palette[C_HEADER] = gfx_RGBTo1555(32, 52, 96);
    gfx_palette[C_TEXT]   = gfx_RGBTo1555(230, 230, 230);
    gfx_palette[C_NICK]   = gfx_RGBTo1555(120, 190, 255);
    gfx_palette[C_STATUS] = gfx_RGBTo1555(240, 200, 90);
    gfx_palette[C_SELF]   = gfx_RGBTo1555(140, 230, 140);
    gfx_palette[C_DIM]    = gfx_RGBTo1555(120, 125, 140);
    gfx_palette[C_OK]     = gfx_RGBTo1555(90, 220, 110);
    gfx_palette[C_BAD]    = gfx_RGBTo1555(230, 80, 80);
    gfx_palette[C_KEY]    = gfx_RGBTo1555(44, 48, 64);
    gfx_palette[C_KEYSEL] = gfx_RGBTo1555(140, 230, 140);
    gfx_palette[C_KEYON]  = gfx_RGBTo1555(32, 70, 130);
}

static void draw_header(const char *title, const char *who, const char *right, uint8_t right_c) {
    gfx_SetColor(C_HEADER);
    gfx_FillRectangle(0, 0, SCREEN_W, header_h());
    int rx = right ? SCREEN_W - 3 - text_w(right) : SCREEN_W;
    int x = draw_text_clip(title, 3, 2, C_TEXT, rx - 4);
    if (who && who[0]) draw_text_clip(who, x + glyph_w(' '), 2, C_SELF, rx - 4);
    if (right) draw_text(right, rx, 2, right_c);
}

static void draw_input_line(const char *label, bool nick_mode) {
    static const char *mode_names[] = { "abc", "ABC", "123" };
    int y = input_y();
    gfx_SetColor(C_HEADER);
    gfx_HorizLine(0, y - 3, SCREEN_W);

    int x = draw_text(label, 2, y, C_DIM) + 4;
    int mx = SCREEN_W - 2 - text_w(mode_names[mode]);
    draw_text(mode_names[mode], mx, y, C_STATUS);

    /* Show the tail of the input so the cursor is always visible. */
    int cur_w = glyph_w('_');
    int avail = mx - 6 - cur_w - x;
    const char *shown = input + input_len;
    int w = 0;
    while (shown > input && w + glyph_w(shown[-1]) <= avail) w += glyph_w(*--shown);
    x = draw_text(shown, x, y, C_TEXT);
    gfx_SetColor(C_SELF);
    gfx_FillRectangle(x, y + text_h() - 1, cur_w, 2);

    if (osk_on) {
        draw_osk(nick_mode);
    } else {
        const char *hint = nick_mode ? "2nd:keyboard alpha:abc/ABC/123"
                                     : "2nd:keys graph:send y=:font mode:nick";
        draw_text(hint, 2, y + line_h + 2, C_DIM);
    }
}

static void draw_chat(void) {
    gfx_FillScreen(C_BG);
    draw_header("SAX for CE -", nick, has_srl ? "online" : "no link", has_srl ? C_OK : C_BAD);

    int rows_visible = log_rows();
    /* row index (from the top of the log area) of the newest entry's last line */
    int cursor = rows_visible - 1 + scroll;
    for (uint8_t k = 0; k < log_count; k++) {
        const entry_t *e = &log_buf[(log_head + LOG_MAX - 1 - k) % LOG_MAX];
        int rows = entry_rows(e);
        int first = cursor - rows + 1;
        entry_layout(e, first, true, rows_visible);
        cursor = first - 1;
        if (cursor < 0) break;
    }

    if (scroll > 0) {
        const char *more = "-- more below --";
        int y = log_y() + (rows_visible - 1) * line_h;
        int w = text_w(more);
        gfx_SetColor(C_BG);
        gfx_FillRectangle(SCREEN_W / 2 - w / 2 - 2, y, w + 4, line_h);
        draw_text(more, SCREEN_W / 2 - w / 2, y, C_STATUS);
    }

    draw_input_line(">", false);
    gfx_SwapDraw();
}

/* ---------- config ---------- */
/* SAXCFG: nick '\0', osk flag, font pack name '\0', font index.
 * Older versions stored less; missing fields keep their defaults. */

static void save_cfg(void) {
    uint8_t h = ti_Open(CFG_VAR, "w");
    if (h) {
        ti_Write(nick, 1, strlen(nick) + 1, h);
        uint8_t flag = osk_on;
        ti_Write(&flag, 1, 1, h);
        const font_ref_t *f = &fonts[cur_font];
        ti_Write(f->pack, 1, strlen(f->pack) + 1, h);
        ti_Write(&f->index, 1, 1, h);
        ti_Close(h);
    }
    apply_font();   /* the write may have moved the font pack */
}

static bool nick_changed;   /* old saved name was shortened on load */

static void load_cfg(void) {
    char buf[40];
    nick[0] = '\0';
    cur_font = 0;
    uint8_t h = ti_Open(CFG_VAR, "r");
    if (!h) return;
    size_t n = ti_Read(buf, 1, sizeof buf - 1, h);
    ti_Close(h);
    buf[n] = '\0';
    size_t len = strlen(buf);
    /* Reuse the name from older versions (up to 15 chars): keep the valid
     * characters and the first 8 of them, rather than asking again. */
    size_t k = 0;
    for (size_t j = 0; j < len && k < NICK_MAX; j++)
        if (nick_char_ok(buf[j], k == 0)) nick[k++] = buf[j];
    nick[k] = '\0';
    nick_changed = strcmp(nick, buf) != 0 && k > 0;
    size_t i = len + 1;
    if (i < n) osk_on = buf[i++] != 0;
    if (i < n) {
        const char *pack = buf + i;
        size_t plen = strlen(pack);
        size_t idx_at = i + plen + 1;
        if (plen && idx_at < n) {
            uint8_t idx = (uint8_t)buf[idx_at];
            for (uint8_t k = 1; k < font_count; k++)
                if (!strcmp(fonts[k].pack, pack) && fonts[k].index == idx) cur_font = k;
        }
    }
}

static void next_font(void) {
    if (font_count == 1) {
        log_add('S', NULL, "No font packs found. Send a fontlibc font pack AppVar to use one.");
        return;
    }
    cur_font = (uint8_t)((cur_font + 1) % font_count);
    save_cfg();   /* also applies it */
    char msg[48];
    if (using_pack()) {
        const char *name = fontlib_GetFontPackName(fonts[cur_font].pack);
        uint8_t hgt = fonts[cur_font].height;
        char size[4];
        size[0] = (char)('0' + hgt / 10);
        size[1] = (char)('0' + hgt % 10);
        size[2] = '\0';
        strcpy(msg, "Font: ");
        strncat(msg, name ? name : fonts[cur_font].pack, 24);
        strcat(msg, " ");
        strcat(msg, hgt < 10 ? size + 1 : size);
        strcat(msg, "px");
    } else {
        strcpy(msg, "Font: built-in");
    }
    log_add('S', NULL, msg);
}

/* ---------- keyboard ---------- */

static char key_char(uint8_t key) {
    /* Letters as printed in green above each key. */
    static const struct { uint8_t key; char lower; char num; } map[] = {
        { sk_Math,   'a', '!' }, { sk_Apps,  'b', '@' }, { sk_Prgm,   'c', '#' },
        { sk_Recip,  'd', '&' }, { sk_Sin,   'e', '=' }, { sk_Cos,    'f', '<' },
        { sk_Tan,    'g', '>' }, { sk_Power, 'h', '^' }, { sk_Square, 'i', '%' },
        { sk_Comma,  'j', ',' }, { sk_LParen,'k', '(' }, { sk_RParen, 'l', ')' },
        { sk_Div,    'm', '/' }, { sk_Log,   'n', ';' }, { sk_7,      'o', '7' },
        { sk_8,      'p', '8' }, { sk_9,     'q', '9' }, { sk_Mul,    'r', '*' },
        { sk_Ln,     's', '_' }, { sk_4,     't', '4' }, { sk_5,      'u', '5' },
        { sk_6,      'v', '6' }, { sk_Sub,   'w', '-' }, { sk_Store,  'x', ':' },
        { sk_1,      'y', '1' }, { sk_2,     'z', '2' }, { sk_3,      '\'','3' },
        { sk_0,      ' ', '0' }, { sk_DecPnt,'.', '.' }, { sk_Chs,    '?', '-' },
        { sk_Add,    '"', '+' }, { sk_Vars,  ',', '$' },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (map[i].key != key) continue;
        if (mode == 2) return map[i].num;
        char c = map[i].lower;
        if (mode == 1 && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        return c;
    }
    return 0;
}

enum { ED_NONE, ED_CHANGED, ED_SUBMIT, ED_LAYOUT };

static void insert_char(char c, size_t max, bool nick_mode) {
    if (input_len >= max) return;
    if (nick_mode && !nick_char_ok(c, input_len == 0)) return;
    input[input_len++] = c;
    input[input_len] = '\0';
}

static void backspace(void) {
    if (input_len) input[--input_len] = '\0';
}

/* Shared line editor for the chat box and the nick prompt. */
static int edit_key(uint8_t key, size_t max, bool nick_mode) {
    if (key == sk_2nd) {
        osk_on = !osk_on;
        save_cfg();
        return ED_LAYOUT;
    }
    if (key == sk_Graph) return ED_SUBMIT;   /* send / OK, keyboard shown or not */
    if (osk_on) {
        switch (key) {
            case sk_Left:
                osk_col = osk_col ? osk_col - 1 : osk_row_len(osk_row) - 1;
                return ED_CHANGED;
            case sk_Right:
                osk_col = (uint8_t)((osk_col + 1) % osk_row_len(osk_row));
                return ED_CHANGED;
            case sk_Up:   osk_move_row(-1); return ED_CHANGED;
            case sk_Down: osk_move_row(1);  return ED_CHANGED;
            case sk_Enter:
                if (osk_row < 4) {
                    insert_char(osk_chars[osk_layer][osk_row][osk_col], max, nick_mode);
                    return ED_CHANGED;
                }
                switch (osk_special[osk_col].code) {
                    case K_SHIFT: osk_layer = osk_layer == 1 ? 0 : 1; break;
                    case K_SYM:   osk_layer = osk_layer == 2 ? 0 : 2; break;
                    case K_SPACE: insert_char(' ', max, nick_mode); break;
                    case K_DEL:   backspace(); break;
                    default:      return ED_SUBMIT;
                }
                return ED_CHANGED;
            default:
                break;
        }
    } else if (key == sk_Enter) {
        return ED_SUBMIT;
    }
    if (key == sk_Alpha) {
        mode = (mode + 1) % 3;
        return ED_CHANGED;
    }
    if (key == sk_Del) {
        backspace();
        return ED_CHANGED;
    }
    char c = key_char(key);
    if (c) {
        insert_char(c, max, nick_mode);
        return ED_CHANGED;
    }
    return ED_NONE;
}

/* ---------- serial ---------- */

static void srl_send(const char *s, size_t len) {
    if (!has_srl) return;
    while (len) {
        int n = srl_Write(&srl, s, len);
        if (n < 0) { has_srl = false; dirty = true; return; }
        s += n;
        len -= (size_t)n;
        if (len) usb_HandleEvents();
    }
}

static void send_line(char kind, const char *payload) {
    char buf[TEXT_MAX + 4];
    buf[0] = kind;
    buf[1] = ' ';
    size_t n = strlen(payload);
    if (n > TEXT_MAX) n = TEXT_MAX;
    memcpy(buf + 2, payload, n);
    buf[2 + n] = '\n';
    srl_send(buf, n + 3);
}

static void handle_line(char *line) {
    /* "R": the bridge (re)opened the port and wants our hello. */
    if (line[0] == 'R' && (line[1] == '\0' || line[1] == ' ')) {
        send_line('H', nick);
        return;
    }
    if (line[0] == '\0' || line[1] != ' ') return;
    char *body = line + 2;
    if (line[0] == 'M') {
        char *tab = strchr(body, '\t');
        if (!tab) { log_add('M', "?", body); return; }
        *tab = '\0';
        log_add('M', body, tab + 1);
    } else if (line[0] == 'S') {
        log_add('S', NULL, body);
    }
}

static void poll_serial(void) {
    if (!has_srl) return;
    if (!hello_sent) {
        send_line('H', nick);
        hello_sent = true;
    }
    char chunk[64];
    int n = srl_Read(&srl, chunk, sizeof chunk);
    if (n < 0) {
        has_srl = false;
        log_add('S', NULL, "USB link error.");
        return;
    }
    for (int i = 0; i < n; i++) {
        char c = chunk[i];
        if (c == '\r') continue;
        if (c == '\n') {
            rx_line[rx_len] = '\0';
            handle_line(rx_line);
            rx_len = 0;
        } else if (rx_len < RX_MAX) {
            rx_line[rx_len++] = c;
        }
    }
}

static usb_error_t handle_usb_event(usb_event_t event, void *event_data,
                                    usb_callback_data_t *callback_data) {
    usb_error_t err;
    if ((err = srl_UsbEventCallback(event, event_data, callback_data)) != USB_SUCCESS)
        return err;

    if (event == USB_DEVICE_CONNECTED_EVENT && !(usb_GetRole() & USB_ROLE_DEVICE)) {
        usb_ResetDevice(event_data);
    }

    if (event == USB_HOST_CONFIGURE_EVENT ||
        (event == USB_DEVICE_ENABLED_EVENT && !(usb_GetRole() & USB_ROLE_DEVICE))) {
        if (has_srl) return USB_SUCCESS;
        usb_device_t device;
        if (event == USB_HOST_CONFIGURE_EVENT) {
            device = usb_FindDevice(NULL, NULL, USB_SKIP_HUBS);
            if (device == NULL) return USB_SUCCESS;
        } else {
            device = event_data;
        }
        if (srl_Open(&srl, device, srl_buf, sizeof srl_buf, SRL_INTERFACE_ANY, 115200) == SRL_SUCCESS) {
            has_srl = true;
            hello_sent = false;
            rx_len = 0;
            log_add('S', NULL, "USB linked. Waiting for bridge...");
        } else {
            log_add('S', NULL, "Could not open USB serial.");
        }
    }

    if (event == USB_DEVICE_DISCONNECTED_EVENT && has_srl && event_data == srl.dev) {
        srl_Close(&srl);
        has_srl = false;
        log_add('S', NULL, "USB disconnected.");
    }

    return USB_SUCCESS;
}

/* ---------- nickname prompt ---------- */

/* Modal nickname prompt. Returns false if the user cancelled with no nick set. */
static bool prompt_nick(void) {
    strcpy(input, nick);
    input_len = (uint8_t)strlen(input);
    uint8_t saved_mode = mode;
    mode = 0;
    const char *err = NULL;
    bool redraw = true;
    for (;;) {
        usb_HandleEvents();
        if (redraw) {
            gfx_FillScreen(C_BG);
            draw_header("SAX for CE - set nickname", NULL, NULL, 0);
            int y = header_h() + 8;
            y = draw_para("Your name on SAX. Up to 8 characters:", 8, y, C_TEXT) + 2;
            y = draw_para("letters, digits, - _ [ ] { } | ^ `", 8, y, C_DIM);
            y = draw_para("(it can't start with a digit or -)", 8, y, C_DIM) + 6;
            if (err) y = draw_para(err, 8, y, C_BAD) + 4;
            if (!osk_on) {
                y = draw_para("[enter] or [graph] save", 8, y, C_DIM);
                draw_para("[del] erase   [clear] cancel", 8, y, C_DIM);
            }
            draw_input_line("nick", true);
            gfx_SwapDraw();
            redraw = false;
        }
        uint8_t key = os_GetCSC();
        if (!key) continue;
        redraw = true;
        if (key == sk_Clear) {
            input[0] = '\0';
            input_len = 0;
            mode = saved_mode;
            return nick[0] != '\0';
        }
        int r = edit_key(key, NICK_MAX, true);
        if (r != ED_SUBMIT) continue;
        if (!nick_valid(input)) {
            err = input_len ? "That nickname isn't allowed." : "Type a nickname first.";
            continue;
        }
        strcpy(nick, input);
        save_cfg();
        input[0] = '\0';
        input_len = 0;
        mode = saved_mode;
        return true;
    }
}

/* ---------- main ---------- */

static void send_input(void) {
    if (!input_len) {
        log_add('S', NULL, "Type a message first.");
        return;
    }
    if (!has_srl) {
        /* keep the text so it can be sent once the link is up */
        log_add('S', NULL, "No USB link yet; message kept. Plug in and send again.");
        return;
    }
    send_line('M', input);
    log_add('E', nick, input);
    input[0] = '\0';
    input_len = 0;
}

int main(void) {
    gfx_Begin();
    gfx_SetDrawBuffer();
    setup_palette();
    gfx_SetMonospaceFont(CHAR_W);
    gfx_SetTextBGColor(C_TRANSP);
    gfx_SetTextTransparentColor(C_TRANSP);

    if (usb_Init(handle_usb_event, NULL, srl_GetCDCStandardDescriptors(), USB_DEFAULT_INIT_FLAGS) != USB_SUCCESS) {
        gfx_End();
        usb_Cleanup();
        os_ClrHome();
        os_PutStrFull("SAXCE: USB init failed.");
        while (!os_GetCSC());
        return 1;
    }

    scan_fonts();
    load_cfg();
    apply_font();
    if (nick[0] == '\0' && !prompt_nick()) goto done;

    log_add('S', NULL, "Welcome to SAX for CE.");
    log_add('S', NULL, "Plug into a PC running the SAX-CE Bridge.");
    if (nick_changed) {
        char msg[48];
        strcpy(msg, "Name shortened to ");
        strcat(msg, nick);
        strcat(msg, " (mode: change)");
        save_cfg();
        log_add('S', NULL, msg);
    }

    for (;;) {
        usb_HandleEvents();
        poll_serial();

        uint8_t key = os_GetCSC();
        if (key) {
            dirty = true;
            if (key == sk_Clear) {
                if (input_len == 0) break;
                input[0] = '\0';
                input_len = 0;
            } else if (!osk_on && key == sk_Up) {
                int max = total_rows() - log_rows();
                if (scroll < max) scroll++;
            } else if (!osk_on && key == sk_Down) {
                if (scroll > 0) scroll--;
            } else if (key == sk_Yequ) {
                next_font();
            } else if (key == sk_Mode) {
                if (prompt_nick() && has_srl) send_line('H', nick);
            } else if (edit_key(key, TEXT_MAX, false) == ED_SUBMIT) {
                send_input();
            }
        }

        if (dirty) {
            draw_chat();
            dirty = false;
        }
    }

done:
    usb_Cleanup();
    gfx_End();
    return 0;
}
