/*
 * SAXCE - SAX chat for the TI-84 Plus CE
 * Akhilesh Gollapudi
 *
 * Needs the SAX-CE bridge on a PC (USB serial). Protocol is in the README.
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

#define VERSION "1.0"

#define SCREEN_W 320
#define SCREEN_H 240
#define CHAR_W   8
#define LINE_H   10
#define KB_ROWS  5

/* palette */
#define C_BG      1
#define C_BAR     2
#define C_TEXT    3
#define C_NICK    4
#define C_STATUS  5
#define C_SELF    6
#define C_DIM     7
#define C_OK      8
#define C_BAD     9
#define C_KEY     10
#define C_KEYSEL  11
#define C_KEYON   12
#define C_TRANSP  254

#define NICK_MAX 8   /* efnet allows 9, leave room for a digit if taken */
#define WHO_MAX  15
#define TEXT_MAX 200
#define LOG_MAX  48
#define RX_MAX   (WHO_MAX + TEXT_MAX + 8)

#define CFG_VAR "SAXCFG"

typedef struct {
    char kind;  /* M = chat, S = status, E = own message */
    char nick[WHO_MAX + 1];
    char text[TEXT_MAX + 1];
} msg_t;

static msg_t msgs[LOG_MAX];
static uint8_t msg_head, msg_count;
static int scroll;

static char nick[NICK_MAX + 1];
static char input[TEXT_MAX + 1];
static uint8_t input_len;
static uint8_t mode;  /* 0 abc, 1 ABC, 2 123 */

static bool kb_on;
static uint8_t kb_layer;
static uint8_t kb_row = 1, kb_col;

static srl_device_t srl;
static uint8_t srl_buf[512];
static bool linked;
static bool said_hello;
static char rx[RX_MAX + 1];
static uint16_t rx_len;
static bool redraw = true;

/* fonts: [0] is the graphx font, the rest come from font packs */

#define FONTS_MAX 24

typedef struct {
    char pack[9];
    uint8_t index;
    uint8_t height;
} fontref_t;

static fontref_t fonts[FONTS_MAX];
static uint8_t num_fonts = 1;
static uint8_t font;
static uint8_t line_h = LINE_H;

static bool custom_font(void) { return font != 0; }

static void find_fonts(void) {
    void *pos = NULL;
    char *name;

    num_fonts = 1;
    fonts[0].pack[0] = 0;
    fonts[0].height = 8;
    while ((name = ti_Detect(&pos, "FONTPACK")) && num_fonts < FONTS_MAX) {
        uint8_t h = ti_Open(name, "r");
        if (!h) continue;
        uint8_t n = ((const fontlib_font_pack_t *)ti_GetDataPtr(h))->fontCount;
        ti_Close(h);

        for (uint8_t i = 0; i < n && num_fonts < FONTS_MAX; i++) {
            fontlib_font_t *f = fontlib_GetFontByIndex(name, i);
            if (!f || f->height < 6 || f->height > 24) continue;
            fontref_t *r = &fonts[num_fonts++];
            strncpy(r->pack, name, 8);
            r->pack[8] = 0;
            r->index = i;
            r->height = f->height;
        }
    }
}

/* call again after writing any file, appvars can move */
static void load_font(void) {
    if (font >= num_fonts) font = 0;
    if (custom_font()) {
        fontlib_font_t *f = fontlib_GetFontByIndex(fonts[font].pack, fonts[font].index);
        if (f && fontlib_SetFont(f, 0)) {
            fontlib_SetWindowFullScreen();
            fontlib_SetTransparency(true);
            fontlib_SetNewlineOptions(0);
            line_h = fontlib_GetCurrentFontHeight();
            if (line_h < 8) line_h = 8;
            return;
        }
        font = 0;
    }
    line_h = LINE_H;
}

static uint8_t char_w(char c) {
    if (!custom_font()) return CHAR_W;
    if (!fontlib_ValidateCodePoint(c)) c = '?';
    return fontlib_GetGlyphWidth(c);
}

static void put_char(int x, int y, char c, uint8_t color) {
    if (custom_font()) {
        if (!fontlib_ValidateCodePoint(c)) c = '?';
        fontlib_SetForegroundColor(color);
        fontlib_SetCursorPosition(x, y);
        fontlib_DrawGlyph(c);
    } else {
        gfx_SetTextFGColor(color);
        gfx_SetTextXY(x, y);
        gfx_PrintChar(c);
    }
}

static int str_w(const char *s) {
    int w = 0;
    while (*s) w += char_w(*s++);
    return w;
}

static int put_str_max(const char *s, int x, int y, uint8_t color, int max_x) {
    for (; *s; s++) {
        uint8_t w = char_w(*s);
        if (x + w > max_x) break;
        put_char(x, y, *s, color);
        x += w;
    }
    return x;
}

static int put_str(const char *s, int x, int y, uint8_t color) {
    return put_str_max(s, x, y, color, SCREEN_W);
}

/* wraps at the screen edge, returns the next y */
static int put_wrapped(const char *s, int x, int y, uint8_t color) {
    int cx = x;
    for (; *s; s++) {
        uint8_t w = char_w(*s);
        if (cx + w > SCREEN_W - 2 && cx > x) {
            cx = x;
            y += line_h;
        }
        put_char(cx, y, *s, color);
        cx += w;
    }
    return y + line_h;
}

/* layout depends on the font height */
static int font_h(void)  { return custom_font() ? fonts[font].height : 8; }
static int bar_h(void)   { return font_h() + 4; }
static int log_top(void) { return bar_h() + 2; }
static int key_h(void)   { int h = font_h() + 5; return h < 17 ? 17 : h; }
static int kb_top(void)  { return SCREEN_H - KB_ROWS * key_h(); }
static int input_y(void) { return kb_on ? kb_top() - line_h - 3 : SCREEN_H - 2 * line_h - 6; }
static int log_rows(void) {
    int r = (input_y() - 4 - log_top()) / line_h;
    return r < 1 ? 1 : r;
}

/* message log */

static void copy_printable(char *dst, const char *src, size_t max) {
    size_t i;
    for (i = 0; src[i] && i < max; i++)
        dst[i] = (src[i] >= ' ' && src[i] <= '~') ? src[i] : '?';
    dst[i] = 0;
}

static void add_msg(char kind, const char *who, const char *text) {
    msg_t *m = &msgs[msg_head];
    m->kind = kind;
    copy_printable(m->nick, who ? who : "", WHO_MAX);
    copy_printable(m->text, text, TEXT_MAX);
    msg_head = (msg_head + 1) % LOG_MAX;
    if (msg_count < LOG_MAX) msg_count++;
    scroll = 0;
    redraw = true;
}

static void status(const char *text) { add_msg('S', NULL, text); }

/* "nick: text", status lines have no nick */
static size_t prefix_len(const msg_t *m) {
    return m->kind == 'S' ? 0 : strlen(m->nick) + 2;
}

static char msg_char(const msg_t *m, size_t i, uint8_t *color) {
    size_t pre = prefix_len(m);
    if (i < pre) {
        *color = m->kind == 'E' ? C_SELF : C_NICK;
        if (i < pre - 2) return m->nick[i];
        return i == pre - 2 ? ':' : ' ';
    }
    *color = m->kind == 'S' ? C_STATUS : C_TEXT;
    return m->text[i - pre];
}

/* word wrap by pixels; draws rows top..visible-1 if draw is set, returns row count */
static int layout_msg(const msg_t *m, int top, bool draw, int visible) {
    size_t len = prefix_len(m) + strlen(m->text);
    int row = 0, x = 1;
    for (size_t i = 0; i < len; i++) {
        uint8_t color;
        char c = msg_char(m, i, &color);
        uint8_t w = char_w(c);
        if (x + w > SCREEN_W - 2 && x > 1) {
            row++;
            x = 1;
        }
        int r = top + row;
        if (draw && r >= 0 && r < visible) put_char(x, log_top() + r * line_h, c, color);
        x += w;
    }
    return row + 1;
}

static int msg_rows(const msg_t *m) { return layout_msg(m, 0, false, 0); }

static int all_rows(void) {
    int rows = 0;
    for (uint8_t k = 0; k < msg_count; k++)
        rows += msg_rows(&msgs[(msg_head + LOG_MAX - 1 - k) % LOG_MAX]);
    return rows;
}

/* irc nick rules */

static bool nick_char_ok(char c, bool first) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    if (c && strchr("[]\\`_^{|}", c)) return true;
    if (first) return false;
    return (c >= '0' && c <= '9') || c == '-';
}

static bool nick_ok(const char *s) {
    if (!s[0] || strlen(s) > NICK_MAX) return false;
    for (size_t i = 0; s[i]; i++)
        if (!nick_char_ok(s[i], i == 0)) return false;
    return true;
}

/* on screen keyboard */

enum { K_SHIFT = 1, K_SYM, K_SPACE, K_DEL, K_SEND };

static const char *const kb_keys[3][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?" },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:!" },
    { "!@#$%^&*()", "-_=+[]{}\\|", "~`<>/;:\"'?", ".,0123456@" },
};

static const struct { uint8_t code, w; } kb_bottom[] = {
    { K_SHIFT, 64 }, { K_SYM, 48 }, { K_SPACE, 112 }, { K_DEL, 48 }, { K_SEND, 48 },
};

static uint8_t row_len(uint8_t row) { return row == 4 ? 5 : 10; }

static void key_pos(uint8_t row, uint8_t col, int *x, int *w) {
    if (row < 4) {
        *x = col * 32;
        *w = 32;
        return;
    }
    *x = 0;
    for (uint8_t i = 0; i < col; i++) *x += kb_bottom[i].w;
    *w = kb_bottom[col].w;
}

/* up/down picks whatever key is under the middle of the current one */
static void kb_move(int dir) {
    int x, w;
    key_pos(kb_row, kb_col, &x, &w);
    int mid = x + w / 2;
    kb_row = (kb_row + KB_ROWS + dir) % KB_ROWS;
    for (uint8_t c = 0; c < row_len(kb_row); c++) {
        key_pos(kb_row, c, &x, &w);
        if (mid >= x && mid < x + w) {
            kb_col = c;
            return;
        }
    }
    kb_col = row_len(kb_row) - 1;
}

static void draw_kb(bool nick_mode) {
    int kh = key_h(), top = kb_top(), ty = (kh - font_h()) / 2;

    for (uint8_t r = 0; r < KB_ROWS; r++) {
        int y = top + r * kh;
        for (uint8_t c = 0; c < row_len(r); c++) {
            int x, w;
            key_pos(r, c, &x, &w);
            bool sel = r == kb_row && c == kb_col;
            bool lit = r == 4 && ((kb_bottom[c].code == K_SHIFT && kb_layer == 1) ||
                                  (kb_bottom[c].code == K_SYM && kb_layer == 2));
            gfx_SetColor(sel ? C_KEYSEL : lit ? C_KEYON : C_KEY);
            gfx_FillRectangle(x + 1, y + 1, w - 2, kh - 2);
            uint8_t fg = sel ? C_BG : C_TEXT;

            if (r < 4) {
                char ch = kb_keys[kb_layer][r][c];
                put_char(x + (w - char_w(ch)) / 2, y + ty, ch, fg);
                continue;
            }
            const char *label, *small;
            switch (kb_bottom[c].code) {
                case K_SHIFT: label = "Shift"; small = "Sh"; break;
                case K_SYM:   label = small = kb_layer == 2 ? "abc" : "Sym"; break;
                case K_SPACE: label = "Space"; small = "Spc"; break;
                case K_DEL:   label = small = "Del"; break;
                default:      label = small = nick_mode ? "OK" : "Send"; break;
            }
            if (str_w(label) > w - 4) label = small;
            int lw = str_w(label);
            put_str_max(label, x + (w - lw > 4 ? (w - lw) / 2 : 2), y + ty, fg, x + w - 1);
        }
    }
}

/* drawing */

static void set_palette(void) {
    gfx_palette[C_BG]     = gfx_RGBTo1555(16, 18, 28);
    gfx_palette[C_BAR]    = gfx_RGBTo1555(32, 52, 96);
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

static void draw_bar(const char *title, const char *who, const char *right, uint8_t right_color) {
    gfx_SetColor(C_BAR);
    gfx_FillRectangle(0, 0, SCREEN_W, bar_h());
    int rx = right ? SCREEN_W - 3 - str_w(right) : SCREEN_W;
    int x = put_str_max(title, 3, 2, C_TEXT, rx - 4);
    if (who && who[0]) put_str_max(who, x + char_w(' '), 2, C_SELF, rx - 4);
    if (right) put_str(right, rx, 2, right_color);
}

static void draw_input(const char *label, bool nick_mode) {
    static const char *modes[] = { "abc", "ABC", "123" };
    int y = input_y();

    gfx_SetColor(C_BAR);
    gfx_HorizLine(0, y - 3, SCREEN_W);
    int x = put_str(label, 2, y, C_DIM) + 4;
    int mx = SCREEN_W - 2 - str_w(modes[mode]);
    put_str(modes[mode], mx, y, C_STATUS);

    /* scroll the text so the end (and cursor) stays visible */
    int cur_w = char_w('_');
    int room = mx - 6 - cur_w - x;
    const char *start = input + input_len;
    int w = 0;
    while (start > input && w + char_w(start[-1]) <= room) w += char_w(*--start);
    x = put_str(start, x, y, C_TEXT);
    gfx_SetColor(C_SELF);
    gfx_FillRectangle(x, y + font_h() - 1, cur_w, 2);

    if (kb_on)
        draw_kb(nick_mode);
    else
        put_str(nick_mode ? "2nd:keyboard alpha:abc/ABC/123" : "2nd:keys graph:send y=:font mode:nick",
                2, y + line_h + 2, C_DIM);
}

static void draw_chat(void) {
    gfx_FillScreen(C_BG);
    draw_bar("SAX", nick, linked ? "online" : "offline", linked ? C_OK : C_BAD);

    int visible = log_rows();
    int bottom = visible - 1 + scroll;
    for (uint8_t k = 0; k < msg_count && bottom >= 0; k++) {
        const msg_t *m = &msgs[(msg_head + LOG_MAX - 1 - k) % LOG_MAX];
        int first = bottom - msg_rows(m) + 1;
        layout_msg(m, first, true, visible);
        bottom = first - 1;
    }

    if (scroll > 0) {
        const char *more = "-- more --";
        int y = log_top() + (visible - 1) * line_h;
        int w = str_w(more);
        gfx_SetColor(C_BG);
        gfx_FillRectangle(SCREEN_W / 2 - w / 2 - 2, y, w + 4, line_h);
        put_str(more, SCREEN_W / 2 - w / 2, y, C_STATUS);
    }

    draw_input(">", false);
    gfx_SwapDraw();
}

/* settings appvar: nick\0, keyboard flag, font pack\0, font index */

static void save_settings(void) {
    uint8_t h = ti_Open(CFG_VAR, "w");
    if (h) {
        uint8_t flag = kb_on;
        ti_Write(nick, 1, strlen(nick) + 1, h);
        ti_Write(&flag, 1, 1, h);
        ti_Write(fonts[font].pack, 1, strlen(fonts[font].pack) + 1, h);
        ti_Write(&fonts[font].index, 1, 1, h);
        ti_Close(h);
    }
    load_font();
}

static bool nick_trimmed;

static void load_settings(void) {
    char buf[40];
    size_t n, len, i, k = 0;

    nick[0] = 0;
    font = 0;
    uint8_t h = ti_Open(CFG_VAR, "r");
    if (!h) return;
    n = ti_Read(buf, 1, sizeof buf - 1, h);
    ti_Close(h);
    buf[n] = 0;
    len = strlen(buf);

    /* old versions allowed 15 chars, keep the first 8 valid ones */
    for (i = 0; i < len && k < NICK_MAX; i++)
        if (nick_char_ok(buf[i], k == 0)) nick[k++] = buf[i];
    nick[k] = 0;
    nick_trimmed = k > 0 && strcmp(nick, buf);

    i = len + 1;
    if (i < n) kb_on = buf[i++] != 0;
    if (i < n) {
        const char *pack = buf + i;
        size_t at = i + strlen(pack) + 1;
        if (pack[0] && at < n)
            for (uint8_t f = 1; f < num_fonts; f++)
                if (!strcmp(fonts[f].pack, pack) && fonts[f].index == (uint8_t)buf[at]) font = f;
    }
}

static void next_font(void) {
    char msg[48];

    if (num_fonts == 1) {
        status("No font packs found");
        return;
    }
    font = (font + 1) % num_fonts;
    save_settings();

    if (!custom_font()) {
        status("Font: default");
        return;
    }
    const char *name = fontlib_GetFontPackName(fonts[font].pack);
    uint8_t ht = fonts[font].height;
    char num[3] = { '0' + ht / 10, '0' + ht % 10, 0 };
    strcpy(msg, "Font: ");
    strncat(msg, name ? name : fonts[font].pack, 24);
    strcat(msg, " ");
    strcat(msg, ht < 10 ? num + 1 : num);
    status(msg);
}

/* keys */

/* green letters on the keypad, and what they type in 123 mode */
static const struct { uint8_t key; char letter, num; } keymap[] = {
    { sk_Math,   'a', '!' }, { sk_Apps,   'b', '@' }, { sk_Prgm,   'c', '#' },
    { sk_Recip,  'd', '&' }, { sk_Sin,    'e', '=' }, { sk_Cos,    'f', '<' },
    { sk_Tan,    'g', '>' }, { sk_Power,  'h', '^' }, { sk_Square, 'i', '%' },
    { sk_Comma,  'j', ',' }, { sk_LParen, 'k', '(' }, { sk_RParen, 'l', ')' },
    { sk_Div,    'm', '/' }, { sk_Log,    'n', ';' }, { sk_7,      'o', '7' },
    { sk_8,      'p', '8' }, { sk_9,      'q', '9' }, { sk_Mul,    'r', '*' },
    { sk_Ln,     's', '_' }, { sk_4,      't', '4' }, { sk_5,      'u', '5' },
    { sk_6,      'v', '6' }, { sk_Sub,    'w', '-' }, { sk_Store,  'x', ':' },
    { sk_1,      'y', '1' }, { sk_2,      'z', '2' }, { sk_3,      '\'', '3' },
    { sk_0,      ' ', '0' }, { sk_DecPnt, '.', '.' }, { sk_Chs,    '?', '-' },
    { sk_Add,    '"', '+' }, { sk_Vars,   ',', '$' },
};

static char key_char(uint8_t key) {
    for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++) {
        if (keymap[i].key != key) continue;
        if (mode == 2) return keymap[i].num;
        char c = keymap[i].letter;
        if (mode == 1 && c >= 'a' && c <= 'z') c -= 'a' - 'A';
        return c;
    }
    return 0;
}

enum { ED_NONE, ED_CHANGED, ED_SUBMIT };

static void type_char(char c, size_t max, bool nick_mode) {
    if (input_len >= max) return;
    if (nick_mode && !nick_char_ok(c, input_len == 0)) return;
    input[input_len++] = c;
    input[input_len] = 0;
}

static void erase_char(void) {
    if (input_len) input[--input_len] = 0;
}

static void clear_input(void) {
    input[0] = 0;
    input_len = 0;
}

/* line editing, used by the chat box and the nick screen */
static int edit(uint8_t key, size_t max, bool nick_mode) {
    if (key == sk_2nd) {
        kb_on = !kb_on;
        save_settings();
        return ED_CHANGED;
    }
    if (key == sk_Graph) return ED_SUBMIT;

    if (kb_on) {
        switch (key) {
            case sk_Left:
                kb_col = kb_col ? kb_col - 1 : row_len(kb_row) - 1;
                return ED_CHANGED;
            case sk_Right:
                kb_col = (kb_col + 1) % row_len(kb_row);
                return ED_CHANGED;
            case sk_Up:
                kb_move(-1);
                return ED_CHANGED;
            case sk_Down:
                kb_move(1);
                return ED_CHANGED;
            case sk_Enter:
                if (kb_row < 4) {
                    type_char(kb_keys[kb_layer][kb_row][kb_col], max, nick_mode);
                    return ED_CHANGED;
                }
                switch (kb_bottom[kb_col].code) {
                    case K_SHIFT: kb_layer = kb_layer == 1 ? 0 : 1; break;
                    case K_SYM:   kb_layer = kb_layer == 2 ? 0 : 2; break;
                    case K_SPACE: type_char(' ', max, nick_mode); break;
                    case K_DEL:   erase_char(); break;
                    default:      return ED_SUBMIT;
                }
                return ED_CHANGED;
        }
    } else if (key == sk_Enter) {
        return ED_SUBMIT;
    }

    if (key == sk_Alpha) {
        mode = (mode + 1) % 3;
        return ED_CHANGED;
    }
    if (key == sk_Del) {
        erase_char();
        return ED_CHANGED;
    }
    char c = key_char(key);
    if (!c) return ED_NONE;
    type_char(c, max, nick_mode);
    return ED_CHANGED;
}

/* usb serial */

static void srl_send(const char *s, size_t len) {
    while (linked && len) {
        int n = srl_Write(&srl, s, len);
        if (n < 0) {
            linked = false;
            redraw = true;
            return;
        }
        s += n;
        len -= n;
        if (len) usb_HandleEvents();
    }
}

static void send_line(char kind, const char *text) {
    char buf[TEXT_MAX + 4];
    size_t n = strlen(text);
    if (n > TEXT_MAX) n = TEXT_MAX;
    buf[0] = kind;
    buf[1] = ' ';
    memcpy(buf + 2, text, n);
    buf[n + 2] = '\n';
    srl_send(buf, n + 3);
}

static void got_line(char *line) {
    if (line[0] == 'R' && (!line[1] || line[1] == ' ')) {
        /* bridge (re)opened the port */
        send_line('H', nick);
        return;
    }
    if (!line[0] || line[1] != ' ') return;

    char *body = line + 2;
    if (line[0] == 'M') {
        char *tab = strchr(body, '\t');
        if (tab) {
            *tab = 0;
            add_msg('M', body, tab + 1);
        } else {
            add_msg('M', "?", body);
        }
    } else if (line[0] == 'S') {
        status(body);
    }
}

static void read_serial(void) {
    char chunk[64];

    if (!linked) return;
    if (!said_hello) {
        send_line('H', nick);
        said_hello = true;
    }
    int n = srl_Read(&srl, chunk, sizeof chunk);
    if (n < 0) {
        linked = false;
        status("USB error");
        return;
    }
    for (int i = 0; i < n; i++) {
        char c = chunk[i];
        if (c == '\r') continue;
        if (c == '\n') {
            rx[rx_len] = 0;
            got_line(rx);
            rx_len = 0;
        } else if (rx_len < RX_MAX) {
            rx[rx_len++] = c;
        }
    }
}

/* same as the srldrvce echo example */
static usb_error_t usb_event(usb_event_t event, void *data, usb_callback_data_t *cb) {
    usb_error_t err = srl_UsbEventCallback(event, data, cb);
    if (err != USB_SUCCESS) return err;

    if (event == USB_DEVICE_CONNECTED_EVENT && !(usb_GetRole() & USB_ROLE_DEVICE))
        usb_ResetDevice(data);

    if (event == USB_HOST_CONFIGURE_EVENT ||
        (event == USB_DEVICE_ENABLED_EVENT && !(usb_GetRole() & USB_ROLE_DEVICE))) {
        if (linked) return USB_SUCCESS;
        usb_device_t dev = data;
        if (event == USB_HOST_CONFIGURE_EVENT) {
            dev = usb_FindDevice(NULL, NULL, USB_SKIP_HUBS);
            if (!dev) return USB_SUCCESS;
        }
        if (srl_Open(&srl, dev, srl_buf, sizeof srl_buf, SRL_INTERFACE_ANY, 115200) == SRL_SUCCESS) {
            linked = true;
            said_hello = false;
            rx_len = 0;
            status("USB connected");
        } else {
            status("USB error");
        }
    }

    if (event == USB_DEVICE_DISCONNECTED_EVENT && linked && data == srl.dev) {
        srl_Close(&srl);
        linked = false;
        status("USB disconnected");
    }
    return USB_SUCCESS;
}

/* nick screen, returns false if cancelled with no nick */
static bool ask_nick(void) {
    uint8_t old_mode = mode;
    const char *err = NULL;
    bool draw = true;

    strcpy(input, nick);
    input_len = strlen(input);
    mode = 0;

    for (;;) {
        usb_HandleEvents();
        if (draw) {
            gfx_FillScreen(C_BG);
            draw_bar("SAX - nickname", NULL, NULL, 0);
            int y = bar_h() + 8;
            y = put_wrapped("Enter a nickname (max 8)", 8, y, C_TEXT) + 2;
            y = put_wrapped("a-z 0-9 - _ [ ] { } | ^ `", 8, y, C_DIM);
            y = put_wrapped("can't start with 0-9 or -", 8, y, C_DIM) + 6;
            if (err) y = put_wrapped(err, 8, y, C_BAD) + 4;
            if (!kb_on) put_wrapped("enter:save clear:cancel", 8, y, C_DIM);
            draw_input("nick", true);
            gfx_SwapDraw();
            draw = false;
        }

        uint8_t key = os_GetCSC();
        if (!key) continue;
        draw = true;
        if (key == sk_Clear) {
            clear_input();
            mode = old_mode;
            return nick[0] != 0;
        }
        if (edit(key, NICK_MAX, true) != ED_SUBMIT) continue;
        if (!nick_ok(input)) {
            err = input_len ? "Invalid nickname" : "Enter a nickname";
            continue;
        }
        strcpy(nick, input);
        save_settings();
        clear_input();
        mode = old_mode;
        return true;
    }
}

static void send_input(void) {
    if (!input_len) {
        status("Nothing to send");
        return;
    }
    if (!linked) {
        status("Not connected");  /* text is kept */
        return;
    }
    send_line('M', input);
    add_msg('E', nick, input);
    clear_input();
}

int main(void) {
    gfx_Begin();
    gfx_SetDrawBuffer();
    set_palette();
    gfx_SetMonospaceFont(CHAR_W);
    gfx_SetTextBGColor(C_TRANSP);
    gfx_SetTextTransparentColor(C_TRANSP);

    if (usb_Init(usb_event, NULL, srl_GetCDCStandardDescriptors(), USB_DEFAULT_INIT_FLAGS) != USB_SUCCESS) {
        gfx_End();
        usb_Cleanup();
        os_ClrHome();
        os_PutStrFull("USB init failed");
        while (!os_GetCSC());
        return 1;
    }

    find_fonts();
    load_settings();
    load_font();
    if (!nick[0] && !ask_nick()) goto quit;

    status("SAXCE v" VERSION);
    status("Connect to the bridge to chat");
    if (nick_trimmed) {
        char msg[40] = "Nick is now ";
        strcat(msg, nick);
        save_settings();
        status(msg);
    }

    for (;;) {
        usb_HandleEvents();
        read_serial();

        uint8_t key = os_GetCSC();
        if (key) {
            redraw = true;
            if (key == sk_Clear) {
                if (!input_len) break;
                clear_input();
            } else if (!kb_on && key == sk_Up) {
                if (scroll < all_rows() - log_rows()) scroll++;
            } else if (!kb_on && key == sk_Down) {
                if (scroll > 0) scroll--;
            } else if (key == sk_Yequ) {
                next_font();
            } else if (key == sk_Mode) {
                if (ask_nick()) send_line('H', nick);
            } else if (edit(key, TEXT_MAX, false) == ED_SUBMIT) {
                send_input();
            }
        }

        if (redraw) {
            draw_chat();
            redraw = false;
        }
    }

quit:
    usb_Cleanup();
    gfx_End();
    return 0;
}
