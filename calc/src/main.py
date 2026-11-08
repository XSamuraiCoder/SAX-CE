# SAXCE - SAX chat for the TI-84 Plus CE
# Akhilesh Gollapudi
#
# Build with tipy-asm. Needs the SAX-CE bridge on a PC (USB serial);
# the protocol is in the README. Fixed text lives in tables.py.

from ti84 import file, font, graphics, keyboard, serial

from tables import (FONT_BUILTIN, FONT_N, FONT_PACK, FONT_PX, FONT_UNNAMED, HINT_CHAT, HINT_NICK, KEPT,
                    KEY_DEL, KEY_LOWER, KEY_NUMBER, KEY_SEND, KEY_SHIFT, KEY_SPACE, KEY_SYM, LINKED,
                    MODE_ABC, MSG_LEN, MSG_START, MSG_TEXT, MORE, NICK_BAD, NICK_CANCEL, NICK_EMPTY,
                    NICK_HELP1, NICK_HELP2, NICK_HELP3, NICK_LABEL, NICK_SAVE, NICK_TITLE, NO_FONTS,
                    NO_LINK, ONLINE, OSK_CHARS, PLUG_IN, PROMPT, SHORTENED, TITLE, TYPE_FIRST, UNKNOWN,
                    UNLINKED, WELCOME)

# Layout
SCREEN_W = 320
SCREEN_H = 240
OSK_ROWS = 5
WRAP_W = SCREEN_W - 2

# Colors (RGB565)
C_BG = 0x1083
C_HEADER = 0x21AC
C_TEXT = 0xE73C
C_NICK = 0x7DFF
C_STATUS = 0xF64B
C_SELF = 0x8F31
C_DIM = 0x7BF1
C_OK = 0x5EED
C_BAD = 0xE28A
C_KEY = 0x2988
C_KEYON = 0x2230

# Limits
NICK_MAX = 8
WHO_MAX = 15
TEXT_MAX = 200
LOG_MAX = 16
RX_MAX = WHO_MAX + TEXT_MAX + 8
CFG_MAX = 40

# log entry kinds: chat, status, own message
K_CHAT = 77             # M
K_STATUS = 83           # S
K_ECHO = 69             # E

# on-screen keyboard bottom row
OSK_SPECIAL_W = (64, 48, 112, 48, 48)
OSK_SPECIAL_MSG = (KEY_SHIFT, KEY_SYM, KEY_SPACE, KEY_DEL, KEY_SEND)
S_SHIFT = 0
S_SYM = 1
S_SPACE = 2
S_DEL = 3

ED_NONE = 0
ED_CHANGED = 1
ED_SUBMIT = 2

# what needs redrawing (bit flags in `dirty`); only the marked parts are repainted
D_HEADER = 1            # header bar (title, nick, online status)
D_LOG = 2               # chat log area
D_BOTTOM = 4            # separator, input row, and hint line or keyboard
D_INPUT = 8             # input row only (typing, mode label, cursor)
D_OSK = 16              # on-screen keyboard: just the old and new selected key
D_OSK_ALL = 32          # on-screen keyboard: every key (layer or highlight changed)
D_LAYOUT = 6            # log + bottom: the layout moved (keyboard shown or hidden)
D_ALL = 7               # everything
D_LOG_NEW = 64          # chat log: only new entries at the bottom (scroll up, draw them)

nick = [0] * (NICK_MAX + 1)
text_in = [0] * (TEXT_MAX + 1)
log_kind = [0] * LOG_MAX
log_who = [0] * (LOG_MAX * WHO_MAX)
log_who_len = [0] * LOG_MAX
log_text = [0] * (LOG_MAX * TEXT_MAX)
log_text_len = [0] * LOG_MAX
log_nrows = [0] * LOG_MAX     # rows each entry wraps to; 0 = not worked out yet
widths = [0] * 128            # glyph widths in the current font
rx = [0] * (RX_MAX + 1)
cfg = [0] * CFG_MAX

nick_len = 0
text_len = 0
mode = 0                # alpha keys: 0 abc, 1 ABC, 2 123
osk_on = 0
osk_layer = 0           # 0 lower, 1 upper, 2 symbols
osk_row = 1
osk_col = 0
log_head = 0
log_count = 0
scroll = 0
rx_len = 0
linked = 0
hello_sent = 0
dirty = D_ALL
osk_drawn_row = 1       # key drawn as selected last time
osk_drawn_col = 0
nick_changed = 0
font_pn = 99            # chosen font: its pack (PN_*, or n for font.find(n)) ...
font_sub = 0            # ... and its number in that pack
font_handle = 0
line_h = 10
log_new = 0             # entries added since the log was last drawn

# fonts y= cycles through, filled by find_fonts()
FONTS_MAX = 24
PN_BUILTIN = -1
PN_SAXFONT = 99
fl_pn = [0] * FONTS_MAX
fl_sub = [0] * FONTS_MAX
fl_count = 0


def mark(flags):
    global dirty
    dirty = dirty | flags


# text

def font_at(pn, sub):
    # font `sub` of a pack: SAXFONT, the built-in font, or font.find(pn); 0 if missing
    if pn == PN_BUILTIN:
        return font.default()
    if pn == PN_SAXFONT:
        return font.load("SAXFONT", sub)
    pack = font.find(pn)
    return font.load(pack, sub) if pack else 0


def add_font(pn, sub):
    global fl_count
    h = font_at(pn, sub)
    if h == 0 or fl_count >= FONTS_MAX - 1:
        return
    height = font.height(h)
    if 6 <= height <= 24:           # skip fonts too small or big for the layout
        fl_pn[fl_count] = pn
        fl_sub[fl_count] = sub
        fl_count += 1


def find_fonts():
    # every font y= can pick: SAXFONT's, then every other font pack's
    # (any fontlibc pack on the calc), then the built-in one
    global fl_count
    fl_count = 0
    for i in range(font.count("SAXFONT")):
        add_font(PN_SAXFONT, i)
    sax = font.load("SAXFONT", 0)
    n = 0
    while n < 40:
        pack = font.find(n)
        if pack == 0:
            break
        if sax == 0 or font.load(pack, 0) != sax:
            for i in range(font.count(pack)):
                add_font(n, i)
        n += 1
    fl_pn[fl_count] = PN_BUILTIN
    fl_sub[fl_count] = 0
    fl_count += 1


def apply_font():
    # a font that's gone (pack deleted) falls back to the built-in 8px font.
    # Called again after file writes, since font packs in RAM can move.
    global font_handle, line_h
    font_handle = font_at(font_pn, font_sub)
    if font_handle == 0:
        font_handle = font.default()
    line_h = font.height(font_handle) + 1
    # look widths up once instead of asking the font for every character
    for code in range(128):
        width = font.width(font_handle, code)
        widths[code] = width if width else font.width(font_handle, 63)
    # rows depend on the font: work them out again when next needed
    for slot in range(LOG_MAX):
        log_nrows[slot] = 0


def glyph_w(code):
    if code < 128:
        return widths[code]
    return widths[63]


def draw_code(code, x, y, fg, bg):
    # returns the width; draws over the background that's already there
    width = font.char(font_handle, code, x, y, fg)
    return width if width else font.char(font_handle, 63, x, y, fg)


def text_h():
    return line_h - 1


def header_h():
    return text_h() + 4


def log_y():
    return header_h() + 2


def osk_row_h():
    h = text_h() + 5
    return h if h > 17 else 17


def osk_y():
    return SCREEN_H - OSK_ROWS * osk_row_h()


def input_y():
    return osk_y() - line_h - 3 if osk_on else SCREEN_H - 2 * line_h - 6


def log_rows():
    rows = (input_y() - 4 - log_y()) // line_h
    return rows if rows > 0 else 1


def msg_w(msg):
    width = 0
    for i in range(MSG_LEN[msg]):
        width += glyph_w(MSG_TEXT[MSG_START[msg] + i])
    return width


def draw_msg(msg, x, y, fg, bg):
    for i in range(MSG_LEN[msg]):
        x += draw_code(MSG_TEXT[MSG_START[msg] + i], x, y, fg, bg)
    return x


def draw_para(msg, x, y, fg, bg):
    # wraps at the screen edge, returns the next y
    left = x
    for i in range(MSG_LEN[msg]):
        code = MSG_TEXT[MSG_START[msg] + i]
        width = glyph_w(code)
        if x + width > SCREEN_W - 4 and x > left:
            x = left
            y += line_h
        x += draw_code(code, x, y, fg, bg)
    return y + line_h


def draw_nick(x, y, fg, bg):
    for i in range(nick_len):
        x += draw_code(nick[i], x, y, fg, bg)
    return x


# chat log

def clean(code):
    return code if 32 <= code <= 126 else 63


def log_start(kind):
    global log_head, log_count, scroll, log_new
    slot = log_head
    log_kind[slot] = kind
    log_who_len[slot] = 0
    log_text_len[slot] = 0
    log_nrows[slot] = 0
    log_head = (log_head + 1) % LOG_MAX
    if log_count < LOG_MAX:
        log_count += 1
    if scroll:
        mark(D_LOG)             # was scrolled back: jump to the bottom
    else:
        mark(D_LOG_NEW)
    scroll = 0
    log_new += 1
    return slot


def log_who_add(slot, code):
    n = log_who_len[slot]
    if n < WHO_MAX:
        log_who[slot * WHO_MAX + n] = clean(code)
        log_who_len[slot] = n + 1


def log_text_add(slot, code):
    n = log_text_len[slot]
    if n < TEXT_MAX:
        log_text[slot * TEXT_MAX + n] = clean(code)
        log_text_len[slot] = n + 1


def log_status(msg):
    slot = log_start(K_STATUS)
    for i in range(MSG_LEN[msg]):
        log_text_add(slot, MSG_TEXT[MSG_START[msg] + i])


def entry_layout(slot, top, draw, visible):
    # "nick: text" (status lines have no nick), wrapped by pixels.
    # Draws the rows from `top` that fall in 0..visible-1 when draw is set;
    # returns the row count.
    row = 0
    x = 1
    y0 = log_y()
    kind = log_kind[slot]
    if kind != K_STATUS:
        color = C_SELF if kind == K_ECHO else C_NICK
        n = log_who_len[slot]
        base = slot * WHO_MAX
        for i in range(n + 2):
            code = log_who[base + i] if i < n else (58 if i == n else 32)
            width = widths[code]
            if x + width > WRAP_W and x > 1:
                row += 1
                x = 1
            if draw:
                r = top + row
                if 0 <= r < visible:
                    font.char(font_handle, code, x, y0 + r * line_h, color)
            x += width
    # the text: each wrapped row is drawn in one font.codes call
    color = C_STATUS if kind == K_STATUS else C_TEXT
    base = slot * TEXT_MAX
    n = log_text_len[slot]
    run = 0                     # index where the current row's text starts
    run_x = x
    end = n
    for i in range(n):
        width = widths[log_text[base + i]]
        if x + width > WRAP_W and x > 1:
            r = top + row
            if draw and 0 <= r < visible and i > run:
                font.codes(font_handle, log_text, base + run, i - run, run_x, y0 + r * line_h, color)
            row += 1
            x = 1
            run = i
            run_x = 1
            if draw and top + row >= visible:
                end = i         # the rest is below the log area
                break
        x += width
    r = top + row
    if draw and 0 <= r < visible and end > run:
        font.codes(font_handle, log_text, base + run, end - run, run_x, y0 + r * line_h, color)
    return row + 1


def entry_rows(slot):
    # cached: laid out once per entry (and again after a font change)
    if log_nrows[slot] == 0:
        log_nrows[slot] = entry_layout(slot, 0, 0, 0)
    return log_nrows[slot]


def newest(k):
    return (log_head + LOG_MAX - 1 - k) % LOG_MAX


def total_rows():
    rows = 0
    for k in range(log_count):
        rows += entry_rows(newest(k))
    return rows


# irc nick rules

def nick_char_ok(code, first):
    if 97 <= code <= 122 or 65 <= code <= 90:
        return 1
    # [ ] \ ` _ ^ { | }
    if code == 91 or code == 93 or code == 92 or code == 96 or code == 95 or code == 94:
        return 1
    if code == 123 or code == 124 or code == 125:
        return 1
    if first:
        return 0
    return 1 if 48 <= code <= 57 or code == 45 else 0


def input_is_valid_nick():
    if text_len == 0 or text_len > NICK_MAX:
        return 0
    for i in range(text_len):
        if not nick_char_ok(text_in[i], i == 0):
            return 0
    return 1


# on-screen keyboard

def osk_row_len(row):
    return 5 if row == 4 else 10


def osk_key_x(row, col):
    if row < 4:
        return col * 32
    x = 0
    for i in range(col):
        x += OSK_SPECIAL_W[i]
    return x


def osk_key_w(row, col):
    return 32 if row < 4 else OSK_SPECIAL_W[col]


def osk_move_row(step):
    global osk_row, osk_col
    old_row = osk_row
    center = osk_key_x(old_row, osk_col) + osk_key_w(old_row, osk_col) // 2
    osk_row = (osk_row + step + OSK_ROWS) % OSK_ROWS
    osk_col = 0
    for col in range(osk_row_len(osk_row)):
        left = osk_key_x(osk_row, col)
        right = left + osk_key_w(osk_row, col)
        if left <= center < right:
            osk_col = col


def draw_osk_key(row, col):
    row_h = osk_row_h()
    x = osk_key_x(row, col)
    w = osk_key_w(row, col)
    y = osk_y() + row * row_h
    selected = row == osk_row and col == osk_col
    on = row == 4 and (col == S_SHIFT and osk_layer == 1 or col == S_SYM and osk_layer == 2)
    back = C_SELF if selected else (C_KEYON if on else C_KEY)
    fore = C_BG if selected else C_TEXT
    graphics.fill_rect(x + 1, y + 1, w - 2, row_h - 2, back)
    if row < 4:
        code = OSK_CHARS[osk_layer * 40 + row * 10 + col]
        draw_code(code, x + (w - glyph_w(code)) // 2, y + (row_h - text_h()) // 2, fore, back)
    else:
        msg = OSK_SPECIAL_MSG[col]
        draw_msg(msg, x + (w - msg_w(msg)) // 2, y + (row_h - text_h()) // 2, fore, back)


def draw_osk():
    global osk_drawn_row, osk_drawn_col
    for row in range(OSK_ROWS):
        for col in range(osk_row_len(row)):
            draw_osk_key(row, col)
    osk_drawn_row = osk_row
    osk_drawn_col = osk_col


def draw_osk_sel():
    # the selection moved: repaint only the key it left and the key it reached
    global osk_drawn_row, osk_drawn_col
    draw_osk_key(osk_drawn_row, osk_drawn_col)
    draw_osk_key(osk_row, osk_col)
    osk_drawn_row = osk_row
    osk_drawn_col = osk_col


# screens

def draw_header(title, show_nick):
    graphics.fill_rect(0, 0, SCREEN_W, header_h(), C_HEADER)
    x = draw_msg(title, 3, 2, C_TEXT, C_HEADER)
    if show_nick:
        draw_nick(x + glyph_w(32), 2, C_SELF, C_HEADER)
        status = ONLINE if linked else NO_LINK
        draw_msg(status, SCREEN_W - 3 - msg_w(status), 2, C_OK if linked else C_BAD, C_HEADER)


def draw_input_row(label):
    # clears just this row, then draws the label, mode, text and cursor
    y = input_y()
    graphics.fill_rect(0, y - 2, SCREEN_W, line_h + 2, C_BG)
    x = draw_msg(label, 2, y, C_DIM, C_BG) + 4
    mode_msg = MODE_ABC + mode
    mode_x = SCREEN_W - 2 - msg_w(mode_msg)
    draw_msg(mode_msg, mode_x, y, C_STATUS, C_BG)
    # scroll so the end of the line stays visible
    cursor_w = glyph_w(95)
    room = mode_x - 6 - cursor_w - x
    first = text_len
    width = 0
    while first > 0 and width + glyph_w(text_in[first - 1]) <= room:
        first -= 1
        width += glyph_w(text_in[first])
    for i in range(first, text_len):
        x += draw_code(text_in[i], x, y, C_TEXT, C_BG)
    graphics.fill_rect(x, y + text_h() - 1, cursor_w, 2, C_SELF)


def draw_bottom(label, nick_mode):
    # everything under the log: separator, input row, and the hint line or keyboard
    y = input_y()
    graphics.fill_rect(0, y - 3, SCREEN_W, SCREEN_H - y + 3, C_BG)
    graphics.fill_rect(0, y - 3, SCREEN_W, 1, C_HEADER)
    draw_input_row(label)
    if osk_on:
        draw_osk()
    else:
        draw_msg(HINT_NICK if nick_mode else HINT_CHAT, 2, y + line_h + 2, C_DIM, C_BG)


def draw_log_new():
    # new entries at the bottom: move the log up and draw only them,
    # instead of clearing and repainting everything
    global log_new
    count = min(log_new, log_count)
    log_new = 0
    visible = log_rows()
    rows = 0
    for k in range(count):
        rows += entry_rows(newest(k))
    # also repaint when the log doesn't fill the area: an entry pushed out of
    # the 16 kept would otherwise stay on screen above the rest
    if rows >= visible or total_rows() < visible:
        draw_log()
        return
    graphics.scroll(0, log_y(), SCREEN_W, visible * line_h, rows * line_h, C_BG)
    cursor = visible - 1
    for k in range(count):
        slot = newest(k)
        first = cursor - entry_rows(slot) + 1
        entry_layout(slot, first, 1, visible)
        cursor = first - 1


def draw_log():
    global log_new
    log_new = 0
    top = header_h()
    graphics.fill_rect(0, top, SCREEN_W, input_y() - 3 - top, C_BG)
    visible = log_rows()
    cursor = visible - 1 + scroll
    for k in range(log_count):
        slot = newest(k)
        first = cursor - entry_rows(slot) + 1
        entry_layout(slot, first, 1, visible)
        cursor = first - 1
        if cursor < 0:
            break
    if scroll > 0:
        y = log_y() + (visible - 1) * line_h
        w = msg_w(MORE)
        graphics.fill_rect(SCREEN_W // 2 - w // 2 - 2, y, w + 4, line_h, C_BG)
        draw_msg(MORE, SCREEN_W // 2 - w // 2, y, C_STATUS, C_BG)


def draw_chat():
    # repaints only what was marked since the last call
    global dirty
    f = dirty
    dirty = 0
    if f & D_HEADER:
        draw_header(TITLE, 1)
    if f & D_LOG:
        draw_log()
    elif f & D_LOG_NEW:
        draw_log_new()
    if f & D_BOTTOM:
        draw_bottom(PROMPT, 0)
    else:
        if f & D_INPUT:
            draw_input_row(PROMPT)
        if osk_on:
            if f & D_OSK_ALL:
                draw_osk()
            elif f & D_OSK:
                draw_osk_sel()


# settings appvar
# nick, 0, keyboard flag, font pack name, 0, font index
# the pack name is "SAXFONT", "-" for the built-in font, or "#" and a
# character '0' + n for the n-th font pack found

def save_cfg():
    file.delete("SAXCFG")
    if font_pn == PN_SAXFONT:
        extra = MSG_LEN[FONT_PACK] + 2
    elif font_pn == PN_BUILTIN:
        extra = 2
    else:
        extra = 4
    file.create("SAXCFG", nick_len + 2 + extra)
    at = 0
    for i in range(nick_len):
        file.write_byte("SAXCFG", at, nick[i])
        at += 1
    file.write_byte("SAXCFG", at, 0)
    file.write_byte("SAXCFG", at + 1, osk_on)
    at += 2
    if font_pn == PN_SAXFONT:
        for i in range(MSG_LEN[FONT_PACK]):
            file.write_byte("SAXCFG", at, MSG_TEXT[MSG_START[FONT_PACK] + i])
            at += 1
        file.write_byte("SAXCFG", at, 0)
        file.write_byte("SAXCFG", at + 1, font_sub)
    elif font_pn == PN_BUILTIN:
        file.write_byte("SAXCFG", at, 45)
        file.write_byte("SAXCFG", at + 1, 0)
    else:
        file.write_byte("SAXCFG", at, 35)
        file.write_byte("SAXCFG", at + 1, 48 + font_pn)
        file.write_byte("SAXCFG", at + 2, 0)
        file.write_byte("SAXCFG", at + 3, font_sub)
    apply_font()                # the write may have moved a font pack


def load_cfg():
    global nick_len, nick_changed, osk_on, font_pn, font_sub
    nick_len = 0
    font_pn = PN_SAXFONT
    font_sub = 0
    if not file.exists("SAXCFG"):
        return
    size = min(file.size("SAXCFG"), CFG_MAX)
    for i in range(size):
        cfg[i] = file.read_byte("SAXCFG", i)
    # old versions allowed 15 chars, keep the first 8 valid ones
    end = 0
    while end < size and cfg[end] != 0:
        end += 1
    for i in range(end):
        if nick_len < NICK_MAX and nick_char_ok(cfg[i], nick_len == 0):
            nick[nick_len] = cfg[i]
            nick_len += 1
    nick_changed = 1 if nick_len > 0 and nick_len != end else 0
    at = end + 1
    if at < size:
        osk_on = 1 if cfg[at] else 0
        at += 1
    if at + 1 < size and cfg[at] == 45 and cfg[at + 1] == 0:
        font_pn = PN_BUILTIN
    elif at + 3 < size and cfg[at] == 35 and cfg[at + 2] == 0 and cfg[at + 1] >= 48:
        font_pn = cfg[at + 1] - 48
        font_sub = cfg[at + 3]
    elif at + MSG_LEN[FONT_PACK] + 1 < size and cfg[at + MSG_LEN[FONT_PACK]] == 0:
        same = 1
        for i in range(MSG_LEN[FONT_PACK]):
            if cfg[at + i] != MSG_TEXT[MSG_START[FONT_PACK] + i]:
                same = 0
        if same:
            font_sub = cfg[at + MSG_LEN[FONT_PACK] + 1]


def log_number(slot, n):
    if n >= 10:
        log_text_add(slot, 48 + n // 10)
    log_text_add(slot, 48 + n % 10)


def log_msg(slot, msg):
    for i in range(MSG_LEN[msg]):
        log_text_add(slot, MSG_TEXT[MSG_START[msg] + i])


def next_font():
    # y=: the next font of any font pack on the calc, then the built-in one
    global font_pn, font_sub
    find_fonts()
    if fl_count <= 1:
        log_status(NO_FONTS)
        return
    pos = fl_count - 1          # not in the list (pack gone): start from the top
    for i in range(fl_count):
        if fl_pn[i] == font_pn and fl_sub[i] == font_sub:
            pos = i
    pos = (pos + 1) % fl_count
    font_pn = fl_pn[pos]
    font_sub = fl_sub[pos]
    save_cfg()                  # also applies it
    # "Font: DrSans 9px": the pack's family name and the font's height
    slot = log_start(K_STATUS)
    log_msg(slot, FONT_N)
    if font_pn == PN_BUILTIN:
        log_msg(slot, FONT_BUILTIN)
    else:
        at = slot * TEXT_MAX + log_text_len[slot]
        got = font.family_codes(pack_of(font_pn), log_text, at, 24)
        for i in range(got):
            log_text[at + i] = clean(log_text[at + i])
        log_text_len[slot] += got
        if got == 0:
            log_msg(slot, FONT_UNNAMED)
    log_text_add(slot, 32)
    log_number(slot, font.height(font_handle))
    log_msg(slot, FONT_PX)


def pack_of(pn):
    # the font pack behind a font list entry, for font.family_codes
    if pn != PN_SAXFONT:
        return font.find(pn)
    sax = font.load("SAXFONT", 0)
    n = 0
    while n < 40:
        pack = font.find(n)
        if pack == 0 or font.load(pack, 0) == sax:
            return pack
        n += 1
    return 0


# keys

def key_char(key):
    if key > 56:
        return 0
    if mode == 2:
        return KEY_NUMBER[key]
    code = KEY_LOWER[key]
    if mode == 1 and 97 <= code <= 122:
        code -= 32
    return code


def insert_char(code, limit, nick_mode):
    global text_len
    if text_len >= limit:
        return
    if nick_mode and not nick_char_ok(code, text_len == 0):
        return
    text_in[text_len] = code
    text_len += 1


def backspace():
    global text_len
    if text_len:
        text_len -= 1


def edit_key(key, limit, nick_mode):
    # used by the chat box and the nick screen; marks what needs redrawing
    global osk_on, osk_col, osk_layer, mode
    if key == keyboard.SECOND:
        osk_on = 1 - osk_on
        save_cfg()
        mark(D_LAYOUT)
        return ED_CHANGED
    if key == keyboard.GRAPH:
        return ED_SUBMIT
    if osk_on:
        if key == keyboard.LEFT:
            osk_col = osk_col - 1 if osk_col else osk_row_len(osk_row) - 1
            mark(D_OSK)
            return ED_CHANGED
        if key == keyboard.RIGHT:
            osk_col = (osk_col + 1) % osk_row_len(osk_row)
            mark(D_OSK)
            return ED_CHANGED
        if key == keyboard.UP:
            osk_move_row(-1)
            mark(D_OSK)
            return ED_CHANGED
        if key == keyboard.DOWN:
            osk_move_row(1)
            mark(D_OSK)
            return ED_CHANGED
        if key == keyboard.ENTER:
            if osk_row < 4:
                insert_char(OSK_CHARS[osk_layer * 40 + osk_row * 10 + osk_col], limit, nick_mode)
                mark(D_INPUT)
            elif osk_col == S_SHIFT:
                osk_layer = 0 if osk_layer == 1 else 1
                mark(D_OSK_ALL)
            elif osk_col == S_SYM:
                osk_layer = 0 if osk_layer == 2 else 2
                mark(D_OSK_ALL)
            elif osk_col == S_SPACE:
                insert_char(32, limit, nick_mode)
                mark(D_INPUT)
            elif osk_col == S_DEL:
                backspace()
                mark(D_INPUT)
            else:
                return ED_SUBMIT
            return ED_CHANGED
    elif key == keyboard.ENTER:
        return ED_SUBMIT
    if key == keyboard.ALPHA:
        mode = (mode + 1) % 3
        mark(D_INPUT)
        return ED_CHANGED
    if key == keyboard.DEL:
        backspace()
        mark(D_INPUT)
        return ED_CHANGED
    code = key_char(key)
    if code:
        insert_char(code, limit, nick_mode)
        mark(D_INPUT)
        return ED_CHANGED
    return ED_NONE


# serial

def send_nick_line():
    serial.write("H ")
    for i in range(nick_len):
        serial.write_byte(nick[i])
    serial.write_byte(10)


def send_text_line():
    serial.write("M ")
    for i in range(text_len):
        serial.write_byte(text_in[i])
    serial.write_byte(10)


def handle_line():
    # R: bridge (re)opened the port
    if rx_len >= 1 and rx[0] == 82 and (rx_len == 1 or rx[1] == 32):
        if nick_len:
            send_nick_line()
        return
    if rx_len < 2 or rx[1] != 32:
        return
    if rx[0] == 77:
        tab = 2
        while tab < rx_len and rx[tab] != 9:
            tab += 1
        slot = log_start(K_CHAT)
        if tab == rx_len:
            log_who_add(slot, 63)
            for i in range(2, rx_len):
                log_text_add(slot, rx[i])
        else:
            for i in range(2, tab):
                log_who_add(slot, rx[i])
            for i in range(tab + 1, rx_len):
                log_text_add(slot, rx[i])
    elif rx[0] == 83:
        slot = log_start(K_STATUS)
        for i in range(2, rx_len):
            log_text_add(slot, rx[i])


def poll_serial():
    global linked, hello_sent, rx_len
    now = serial.connected()
    if now and not linked:
        linked = 1
        hello_sent = 0
        rx_len = 0
        mark(D_HEADER)
        log_status(LINKED)
    elif linked and not now:
        linked = 0
        mark(D_HEADER)
        log_status(UNLINKED)
    if not linked:
        return
    if not hello_sent and nick_len:
        send_nick_line()
        hello_sent = 1
    # take everything that's waiting, so a burst of messages is drawn once;
    # capped so keys still get read during a flood
    count = 0
    while count < 1024:
        code = serial.read_byte()
        if code < 0:
            break
        count += 1
        if code == 10:
            handle_line()
            rx_len = 0
        elif code != 13 and rx_len < RX_MAX:
            rx[rx_len] = code
            rx_len += 1


# nick screen

def draw_nick_screen(error):
    graphics.fill_rect(0, 0, SCREEN_W, SCREEN_H, C_BG)
    draw_header(NICK_TITLE, 0)
    y = header_h() + 8
    y = draw_para(NICK_HELP1, 8, y, C_TEXT, C_BG) + 2
    y = draw_para(NICK_HELP2, 8, y, C_DIM, C_BG)
    y = draw_para(NICK_HELP3, 8, y, C_DIM, C_BG) + 6
    if error >= 0:
        y = draw_para(error, 8, y, C_BAD, C_BG) + 4
    if not osk_on:
        y = draw_para(NICK_SAVE, 8, y, C_DIM, C_BG)
        draw_para(NICK_CANCEL, 8, y, C_DIM, C_BG)
    draw_bottom(NICK_LABEL, 1)


def prompt_nick():
    # returns 0 if cancelled with no nick
    global text_len, mode, nick_len, dirty
    for i in range(nick_len):
        text_in[i] = nick[i]
    text_len = nick_len
    saved_mode = mode
    mode = 0
    error = -1
    dirty = D_ALL
    while True:
        poll_serial()
        if dirty:
            f = dirty
            dirty = 0
            if f & D_BOTTOM:
                draw_nick_screen(error)    # the layout changed: repaint the screen
            else:
                if f & D_INPUT:
                    draw_input_row(NICK_LABEL)
                if osk_on:
                    if f & D_OSK_ALL:
                        draw_osk()
                    elif f & D_OSK:
                        draw_osk_sel()
        key = keyboard.get()
        if not key:
            continue
        if key == keyboard.CLEAR:
            text_len = 0
            mode = saved_mode
            return 1 if nick_len else 0
        if edit_key(key, NICK_MAX, 1) != ED_SUBMIT:
            continue
        if not input_is_valid_nick():
            error = NICK_BAD if text_len else NICK_EMPTY
            mark(D_BOTTOM)
            continue
        for i in range(text_len):
            nick[i] = text_in[i]
        nick_len = text_len
        save_cfg()
        text_len = 0
        mode = saved_mode
        return 1


# main

def send_input():
    global text_len
    if text_len == 0:
        log_status(TYPE_FIRST)
        return
    if not linked:
        log_status(KEPT)  # text is kept
        return
    send_text_line()
    slot = log_start(K_ECHO)
    for i in range(nick_len):
        log_who_add(slot, nick[i])
    for i in range(text_len):
        log_text_add(slot, text_in[i])
    text_len = 0
    mark(D_INPUT)


def main():
    global text_len, scroll
    graphics.begin()
    serial.begin(115200)
    load_cfg()
    apply_font()
    if nick_len == 0 and not prompt_nick():
        serial.end()
        graphics.end()
        return
    log_status(WELCOME)
    log_status(PLUG_IN)
    if nick_changed:
        save_cfg()
        log_status(SHORTENED)
    mark(D_ALL)
    while True:
        poll_serial()
        key = keyboard.get()
        if key:
            if key == keyboard.CLEAR:
                if text_len == 0:
                    break
                text_len = 0
                mark(D_INPUT)
            elif not osk_on and key == keyboard.UP:
                if scroll < total_rows() - log_rows():
                    scroll += 1
                    mark(D_LOG)
            elif not osk_on and key == keyboard.DOWN:
                if scroll > 0:
                    scroll -= 1
                    mark(D_LOG)
            elif key == keyboard.YEQU:
                next_font()
                mark(D_ALL)
            elif key == keyboard.MODE:
                if prompt_nick() and linked:
                    send_nick_line()
                mark(D_ALL)
            elif edit_key(key, TEXT_MAX, 0) == ED_SUBMIT:
                send_input()
        if dirty:
            draw_chat()
    serial.end()
    graphics.end()


main()