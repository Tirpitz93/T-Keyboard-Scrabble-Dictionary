//
// Created by loenja on 14/09/2026.
//
// Scrabble word judge firmware. This is the only translation unit that includes
// word_radix_trie.h, whose `static const` arrays are the 2 MB dictionary.
//

#include "scrabble_dict_main.h"

#include <Arduino.h>
#include <SPI.h>
#include <stdio.h>

#include <TFT_GC9D01N.h>

#include "word_radix_trie.h"
#include "radix_trie_complete.h"

#define KB_BACKLIGHT_PIN 9

// The 27th alphabet code, O with diaeresis. The word list spells four words with
// it (AARDWOLF, AARDWOLVES, AASVOGEL, AASVOGELS), which is why the plain ASCII
// AARDWOLF is a dead end in this dictionary rather than a bug.
#define SCRABBLE_CODE_ODIAERESIS 26

#define SCREEN_BACKLIGHT 50
#define BACKLIGHT_OFF_TIME 20000UL

// A glyph cell is FONT_W (8) px along the 160 axis and FONT_H (16) px along the
// 40 axis, so the 40 px axis holds two text rows plus an 8 px band:
//   Ystart 23 -> 40-axis rows  0..15   (DispOneChar maps Ystart to |Ystart-23|)
//   Ystart  7 -> 40-axis rows 16..31
//   band      -> 40-axis rows 32..39
// Grey for the autocomplete tail, so it reads as a suggestion rather than as
// something the user typed.
#define GHOST_COLOR GRAY50

#define ROW_WORD_Y 23
#define ROW_STATUS_Y 7
#define BAND_A0 32
#define BAND_A1 39
#define TEXT_COLS (TFT_HEIGHT / FONT_W)   // 160 / 8 = 20

// 40-axis offset of the umlaut dots inside the word row's 0..15 cell. 0 puts
// them at the low edge; if they come out under the glyph instead of over it on
// real hardware, this is the one constant to change (14 is the other edge).
#define UMLAUT_DOT_A0 0
#define UMLAUT_DOT_SIZE 2
#define TFT_HIGH  40
#define TFT_WIDE 160
#define GAP 8
#define TFT_BL_PIN  8
#define TFT_DC 2
#define TFT_MOSI 10
#define TFT_SCLK 20
#define TFT_CS -1
#define TFT_MISO -1
#define KB_BACKLIGHT_PIN  9
#define CORE_DEBUG_LEVEL 5
#define LANDSCAPE 1   //Horizontal screen
//#define PORTRAIT 2
#define CHAR_FONT_W8_H16  //typeface

#define TFT_WIDTH  40
#define TFT_HEIGHT 160

#define TFT_MISO  -1
#define TFT_MOSI  10//21
#define TFT_SCLK  20//22
#define TFT_CS    -1//18  // Chip select control pin
#define TFT_DC    2  // Data Command control pin
#define TFT_RST   -1//27
#define TFT_BL    8//4

/*
#define TFT_MISO  -1
#define TFT_MOSI  21
#define TFT_SCLK  22
#define TFT_CS    18  // Chip select control pin
#define TFT_DC    2  // Data Command control pin
#define TFT_RST   27
#define TFT_BL    23//4
*/
//RGB565
#define RED    0xF800
#define GREEN  0x07E0
#define BLUE   0x001F
#define WHITE  0xFFFF
#define BLACK  0x0000
#define GRAY   0xEF5D
#define GRAY75 0x39E7
#define GRAY50 0x7BEF
#define GRAY25 0xADB5



#ifdef  CHAR_FONT_W8_H16
#define  FONT_W  8
#define  FONT_H  16
#endif


#define SPI_FREQUENCY  40000000
#define DC_C digitalWrite(TFT_DC, LOW)
#define DC_D digitalWrite(TFT_DC, HIGH)
#if (TFT_CS==-1)
#define CS_L ;//digitalWrite(TFT_CS, LOW)
#define CS_H ;//digitalWrite(TFT_CS, HIGH)
#else
#define CS_L digitalWrite(TFT_CS, LOW)
#define CS_H digitalWrite(TFT_CS, HIGH)
#endif

#if (TFT_RST==-1)
#define RST_L ;//digitalWrite(TFT_RST, LOW)
#define RST_H ;//digitalWrite(TFT_RST, HIGH)
#else
#define RST_L digitalWrite(TFT_RST, LOW)
#define RST_H digitalWrite(TFT_RST, HIGH)
#endif
static TFT_GC9D01N_Class TFT_099;

// ---------------------------------------------------------------- dictionary

// Standard English tile values, indexed by alphabet code. The 27th letter (O
// with diaeresis) has no real tile; it takes O's value of 1.
static const uint8_t TILE_VALUE[RADIX_ALPHABET_SIZE] = {
    1,  3,  3,  2,  1,  4,  2,  4,  1,  8,  // A B C D E F G H I J
    5,  1,  3,  1,  1,  3, 10,  1,  1,  1,  // K L M N O P Q R S T
    1,  4,  4,  8,  4, 10,                  // U V W X Y Z
    // 1,                                      // O-diaeresis
};

// UTF-8 rendering of a code buffer, for the matcher and for the serial log.
static char query[SCRABBLE_MAX_WORD_LEN * 4 + 1];

static void codes_to_utf8(const uint8_t *codes, uint8_t len, char *buf, size_t cap) {
    size_t n = 0;
    for (uint8_t i = 0; i < len; i++) {
        if (codes[i] >= RADIX_ALPHABET_SIZE) continue;
        const char *glyph = RADIX_ALPHABET_TABLE[codes[i]];
        while (*glyph && n + 1 < cap) buf[n++] = *glyph++;
    }
    buf[n] = '\0';
}

word_verdict_t scrabble_judge(const uint8_t *codes, uint8_t len) {
    if (len == 0) return WORD_EMPTY;
    codes_to_utf8(codes, len, query, sizeof query);

    if (radix_trie_contains_word(WORD_RADIX_TRIE_DATA, WORD_RADIX_TRIE_DATA_COUNT,
                                 WORD_RADIX_LABEL_POOL, query)) {
        return WORD_VALID;
    }
    if (radix_trie_has_prefix(WORD_RADIX_TRIE_DATA, WORD_RADIX_TRIE_DATA_COUNT,
                              WORD_RADIX_LABEL_POOL, query)) {
        return WORD_PREFIX;
    }
    return WORD_DEAD_END;
}

uint16_t scrabble_score(const uint8_t *codes, uint8_t len) {
    uint16_t score = 0;
    for (uint8_t i = 0; i < len; i++) {
        if (codes[i] < RADIX_ALPHABET_SIZE) score += TILE_VALUE[codes[i]];
    }
    return score;
}

uint8_t scrabble_complete(const uint8_t *codes, uint8_t len,
                          uint8_t *out, uint8_t out_max) {
    if (len == 0 || out_max == 0) return 0;
    return radix_trie_complete(WORD_RADIX_TRIE_DATA, WORD_RADIX_TRIE_DATA_COUNT,
                               WORD_RADIX_LABEL_POOL, codes, len, out, out_max,
                               /*strict=*/true);
}

// -------------------------------------------------------------------- matrix

// Pin map and letter layout mirror src/main.cpp; the scan below differs in that
// it reports genuine press edges rather than "held".
static const uint8_t rows[] = {0, 3, 18, 12, 11, 6, 7};
static const uint8_t cols[] = {1, 4, 5, 19, 13};
static const int rowCount = sizeof(rows) / sizeof(rows[0]);
static const int colCount = sizeof(cols) / sizeof(cols[0]);

static const char keyboard[5][7] = {
    /* col 0 */ {'q', 'w',   0, 'a',   0, ' ',   0},  // (0,2) symbol, (0,4) alt, (0,5) space, (0,6) mic
    /* col 1 */ {'e', 's', 'd', 'p', 'x', 'z',   0},  // (1,6) left shift
    /* col 2 */ {'r', 'g', 't',   0, 'v', 'c', 'f'},  // (2,3) right shift
    /* col 3 */ {'u', 'h', 'y',   0, 'b', 'n', 'j'},  // (3,3) enter
    /* col 4 */ {'o', 'l', 'i',   0, '$', 'm', 'k'},  // (4,3) backspace
};

#define KEY_SYMBOL_C    0
#define KEY_SYMBOL_R    2
#define KEY_ALT_C       0
#define KEY_ALT_R       4
#define KEY_SPACE_C     0
#define KEY_SPACE_R     5
#define KEY_ENTER_C     3
#define KEY_ENTER_R     3
#define KEY_B_C         3
#define KEY_B_R         4
#define KEY_BACKSPACE_C 4
#define KEY_BACKSPACE_R 3
#define KEY_O_C         4
#define KEY_O_R         0

static bool key_held[5][7];
static bool key_prev[5][7];
static bool key_edge[5][7];

// One pass costs colCount * rowCount ms because the driver needs a settling
// delay after every INPUT/OUTPUT flip; ~35 ms doubles as debounce.
static void scan_matrix() {
    for (int c = 0; c < colCount; c++) {
        pinMode(cols[c], OUTPUT);
        digitalWrite(cols[c], LOW);

        for (int r = 0; r < rowCount; r++) {
            pinMode(rows[r], INPUT_PULLUP);
            delay(1);

            bool now = (digitalRead(rows[r]) == LOW);
            key_edge[c][r] = now && !key_prev[c][r];
            key_prev[c][r] = now;
            key_held[c][r] = now;

            pinMode(rows[r], INPUT);
        }
        pinMode(cols[c], INPUT);
    }
}

static inline bool key_pressed(int c, int r) { return key_edge[c][r]; }
static inline bool key_active(int c, int r) { return key_held[c][r]; }

// --------------------------------------------------------------------- state

static uint8_t word_codes[SCRABBLE_MAX_WORD_LEN];
static uint8_t word_len = 0;

// Autocomplete tail, drawn after the typed word in grey.
static uint8_t word_suggest[SCRABBLE_MAX_WORD_LEN];
static uint8_t suggest_len = 0;

// What each glyph cell of the word row currently holds, so a redraw only
// touches the cells that actually changed.
#define CELL_EMPTY 0xFF
static uint8_t drawn_code[TEXT_COLS];
static bool drawn_ghost[TEXT_COLS];

static word_verdict_t verdict = WORD_EMPTY;
static bool symbol_latched = false;
static bool kb_backlight_on = true;

static char status_line[TEXT_COLS + 1];
static char status_drawn[TEXT_COLS + 1];
static uint16_t band_drawn = 0xFFFE;   // impossible RGB565, forces a first paint

static bool screen_on = true;
static unsigned long last_key_ms = 0;

static uint32_t pick_random_sibling(uint32_t node_idx) {
    uint32_t chosen = node_idx;
    uint32_t seen = 0;

    while (node_idx < WORD_RADIX_TRIE_DATA_COUNT) {
        seen++;
        if (random((long)seen) == 0) chosen = node_idx;

        uint32_t header = WORD_RADIX_TRIE_DATA[node_idx];
        if (header & RADIX_FLAG_LAST_SIBLING) break;
        node_idx = radix_next_sibling(node_idx, header);
    }
    return chosen;
}

static uint8_t random_startup_word(uint8_t *out, uint8_t out_max) {
    if (!out || out_max == 0 || WORD_RADIX_TRIE_DATA_COUNT == 0) return 0;

    uint32_t node_idx = pick_random_sibling(0);
    uint8_t n = 0;

    while (node_idx < WORD_RADIX_TRIE_DATA_COUNT) {
        uint32_t header = WORD_RADIX_TRIE_DATA[node_idx];
        uint32_t label_len = header & RADIX_LEN_MASK;

        for (uint32_t k = 0; k < label_len; k++) {
            if (n >= out_max) return n;
            out[n++] = (uint8_t)radix_label_char(WORD_RADIX_LABEL_POOL, header, k);
        }

        if ((header & RADIX_FLAG_END_OF_WORD) && random(2) == 0) return n;
        if (!(header & RADIX_FLAG_HAS_CHILD)) return n;

        node_idx = pick_random_sibling(WORD_RADIX_TRIE_DATA[node_idx + 1]);
    }
    return n;
}

// -------------------------------------------------------------------- render

// Precise rectangle fill. DispColor always pushes a full screen's worth of
// pixels regardless of the window it set, which is far too slow for the small
// regions below.  a* is the 40 px axis, b* the 160 px axis, matching the
// argument order DispOneChar uses for BlockWrite.
static void fill_rect(unsigned int a0, unsigned int a1,
                      unsigned int b0, unsigned int b1, uint16_t color) {
    TFT_099.BlockWrite(a0, a1, b0, b1);
    unsigned long n = (unsigned long)(a1 - a0 + 1) * (b1 - b0 + 1);
    while (n--) TFT_099.WriteOneDot(color);
}

static void draw_glyph(char c, uint8_t col, unsigned int y,
                       uint16_t fg, uint16_t bg) {
    char buf[2] = {c, '\0'};
    TFT_099.DispStr(buf, col * FONT_W, y, fg, bg);
}

// Draws one letter cell of the word row. The font has no diaeresis, so the 27th
// letter is drawn as O plus two dots.
static void draw_letter_cell(uint8_t col, uint8_t code, uint16_t fg) {
    if (code >= RADIX_ALPHABET_SIZE) return;

    const char *glyph = RADIX_ALPHABET_TABLE[code];
    bool umlaut = (glyph[1] != '\0');   // the only multi-byte entry is O-diaeresis
    char c = umlaut ? 'O' : glyph[0];
    draw_glyph(c, col, ROW_WORD_Y, fg, BLACK);

    if (umlaut) {
        unsigned int x = col * FONT_W;
        unsigned int a1 = UMLAUT_DOT_A0 + UMLAUT_DOT_SIZE - 1;
        fill_rect(UMLAUT_DOT_A0, a1, x + 2, x + 2 + UMLAUT_DOT_SIZE - 1, fg);
        fill_rect(UMLAUT_DOT_A0, a1, x + 5, x + 5 + UMLAUT_DOT_SIZE - 1, fg);
    }
}

static void clear_cell(uint8_t col) {
    draw_glyph(' ', col, ROW_WORD_Y, WHITE, BLACK);
}

// Typed letters in white, the autocomplete tail behind them in grey, the way
// shell and editor ghost text reads: what you typed, then what it would become.
static void render_word() {
    for (uint8_t i = 0; i < TEXT_COLS; i++) {
        uint8_t code;
        bool ghost;

        if (i < word_len) {
            code = word_codes[i];
            ghost = false;
        } else if (i < word_len + suggest_len) {
            code = word_suggest[i - word_len];
            ghost = true;
        } else {
            code = CELL_EMPTY;
            ghost = false;
        }

        if (code == drawn_code[i] && ghost == drawn_ghost[i]) continue;

        if (code == CELL_EMPTY) {
            clear_cell(i);
        } else {
            draw_letter_cell(i, code, ghost ? GHOST_COLOR : WHITE);
        }
        drawn_code[i] = code;
        drawn_ghost[i] = ghost;
    }
}

static const char *verdict_label(word_verdict_t v) {
    switch (v) {
        case WORD_VALID:    return "VALID";
        case WORD_PREFIX:   return "NOT WORD";
        case WORD_DEAD_END: return "DEAD END";
        default:            return "";
    }
}

static uint16_t verdict_color(word_verdict_t v) {
    switch (v) {
        case WORD_VALID:    return GREEN;
        case WORD_PREFIX:   return GRAY50;
        case WORD_DEAD_END: return RED;
        default:            return BLACK;
    }
}

// One space-padded full-width line, so it overwrites the previous one without
// needing the row cleared first.
static void render_status() {
    if (verdict == WORD_EMPTY) {
        snprintf(status_line, sizeof status_line, "%s", "Type a word");
    } else if (verdict == WORD_DEAD_END) {
        snprintf(status_line, sizeof status_line, "%-8s %2dL",
                 verdict_label(verdict), (int)word_len);
    } else {
        snprintf(status_line, sizeof status_line, "%-8s %2dL %3dP",
                 verdict_label(verdict), (int)word_len,
                 (int)scrabble_score(word_codes, word_len));
    }

    size_t n = strlen(status_line);
    while (n < TEXT_COLS) status_line[n++] = ' ';
    status_line[TEXT_COLS] = '\0';

    if (strcmp(status_line, status_drawn) != 0) {
        TFT_099.DispStr(status_line, 0, ROW_STATUS_Y, WHITE, BLACK);
        strcpy(status_drawn, status_line);
    }

    uint16_t color = verdict_color(verdict);
    if (color != band_drawn) {
        fill_rect(BAND_A0, BAND_A1, 0, TFT_HEIGHT - 1, color);
        band_drawn = color;
    }
}

// ---------------------------------------------------------------- word edits

static void recompute() {
    if (word_len == 0) {
        verdict = WORD_EMPTY;
        suggest_len = random_startup_word(word_suggest, TEXT_COLS);
    } else {
        verdict = scrabble_judge(word_codes, word_len);

        // Only as many suggestion letters as there are free cells left on the row;
        // scrabble_complete reports none rather than a truncated word.
        uint8_t room = (word_len < TEXT_COLS) ? (uint8_t)(TEXT_COLS - word_len) : 0;
        suggest_len = scrabble_complete(word_codes, word_len, word_suggest, room);
    }

    char tail[SCRABBLE_MAX_WORD_LEN * 4 + 1];
    codes_to_utf8(word_suggest, suggest_len, tail, sizeof tail);

    Serial.printf("%-16s %-8s %2uL %3uP  -> %s%s\n",
                  word_len ? query : "(empty)",
                  verdict == WORD_EMPTY ? "EMPTY" : verdict_label(verdict),
                  (unsigned)word_len,
                  (unsigned)scrabble_score(word_codes, word_len),
                  suggest_len ? (word_len ? query : "") : "",
                  suggest_len ? tail : "-");
}

static void append_code(uint8_t code) {
    if (word_len >= SCRABBLE_MAX_WORD_LEN) return;
    word_codes[word_len++] = code;
    recompute();
}

static void backspace() {
    if (word_len == 0) return;
    word_len--;
    recompute();
}

static void clear_word() {
    word_len = 0;
    recompute();
}

static void set_kb_backlight(bool state) {
    digitalWrite(KB_BACKLIGHT_PIN, state ? HIGH : LOW);
}

// --------------------------------------------------------------------- entry

void scrabble_dict_main::begin() {
    Serial.begin(115200);
    delay(200);
    Serial.println("Scrabble word judge starting");
    Serial.printf("dictionary: %u nodes, %u bytes\n",
                  (unsigned)WORD_RADIX_TRIE_DATA_COUNT,
                  (unsigned)WORD_RADIX_TRIE_DATA_SIZE_BYTES);

    pinMode(KB_BACKLIGHT_PIN, OUTPUT);
    set_kb_backlight(kb_backlight_on);
    // delay(50);
    for (int r = 0; r < rowCount; r++) pinMode(rows[r], INPUT);
    for (int c = 0; c < colCount; c++) pinMode(cols[c], INPUT_PULLUP);

    memset(key_held, 0, sizeof key_held);
    memset(key_prev, 0, sizeof key_prev);
    memset(key_edge, 0, sizeof key_edge);
    scan_matrix();
    memset(key_edge, 0, sizeof key_edge);

    TFT_099.begin();

    TFT_099.backlight(SCREEN_BACKLIGHT);

    TFT_099.DispColor(0, 0, TFT_WIDTH, TFT_HEIGHT, BLACK);


    screen_on = true;
    last_key_ms = millis();
    randomSeed((uint32_t)micros());

    status_drawn[0] = '\0';
    for (uint8_t i = 0; i < TEXT_COLS; i++) {
        drawn_code[i] = CELL_EMPTY;
        drawn_ghost[i] = false;
    }
    recompute();
    render_word();
    render_status();
    Serial.println("Ready");
}

void scrabble_dict_main::loop() {
    scan_matrix();

    bool any_key = false;
    for (int c = 0; c < colCount && !any_key; c++) {
        for (int r = 0; r < rowCount; r++) {
            if (key_edge[c][r]) { any_key = true; break; }
        }
    }

    if (any_key) {
        last_key_ms = millis();
        if (!screen_on) {
            TFT_099.backlight(SCREEN_BACKLIGHT);
            screen_on = true;
        }
    } else if (screen_on && millis() - last_key_ms > BACKLIGHT_OFF_TIME) {
        TFT_099.backlight(0);
        screen_on = false;
    }

    // ALT + B toggles the keyboard backlight, same chord as src/main.cpp.
    bool alt = key_active(KEY_ALT_C, KEY_ALT_R);
    if (alt && key_pressed(KEY_B_C, KEY_B_R)) {
        kb_backlight_on = !kb_backlight_on;
        set_kb_backlight(kb_backlight_on);
    }

    if (key_pressed(KEY_SYMBOL_C, KEY_SYMBOL_R)) symbol_latched = true;

    if (key_pressed(KEY_BACKSPACE_C, KEY_BACKSPACE_R)) backspace();

    if (key_pressed(KEY_ENTER_C, KEY_ENTER_R) ||
        key_pressed(KEY_SPACE_C, KEY_SPACE_R)) {
        clear_word();
    }

    if (!alt) {
        for (int c = 0; c < colCount; c++) {
            for (int r = 0; r < rowCount; r++) {
                if (!key_edge[c][r]) continue;

                char k = keyboard[c][r];
                if (k < 'a' || k > 'z') continue;   // skips 0, ' ' and '$'

                if (symbol_latched) {
                    symbol_latched = false;
                    // Symbol + O is the only way to reach the 27th letter.
                    if (c == KEY_O_C && r == KEY_O_R) {
                        append_code(SCRABBLE_CODE_ODIAERESIS);
                        continue;
                    }
                }
                append_code((uint8_t)(k - 'a'));
            }
        }
    }

    render_word();
    render_status();
}

void setup() { scrabble_dict_main::begin(); }
void loop() { scrabble_dict_main::loop(); }
