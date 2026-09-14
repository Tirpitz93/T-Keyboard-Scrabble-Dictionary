//
// Created by loenja on 14/09/2026.
//
// Standalone Scrabble word judge for the T-Keyboard: type letters on the matrix
// and the display reports whether they form a valid Collins (2019) word, whether
// they can still grow into one, plus the tile score and letter count.
//
// This header deliberately does NOT include word_radix_trie.h. That header
// defines the 2 MB dictionary as `static const`, so pulling it into a second
// translation unit would duplicate the whole array in flash.
//

#ifndef T_KEYBOARD_SCRABBLE_DICT_MAIN_H
#define T_KEYBOARD_SCRABBLE_DICT_MAIN_H

#include <stdint.h>

// Bounded by the display (20 glyph cells), not by the dictionary: the longest
// Collins word is 15 letters.
#define SCRABBLE_MAX_WORD_LEN 20

enum word_verdict_t {
    WORD_EMPTY = 0,   // nothing typed yet
    WORD_PREFIX,      // not a word, but at least one word starts with it
    WORD_VALID,       // a word in the list
    WORD_DEAD_END     // no word starts with it
};

class scrabble_dict_main {
public:
    static void begin();
    static void loop();
};

// `codes` are alphabet indices (0..RADIX_ALPHABET_SIZE-1), not characters:
// the 27th letter is a two-byte UTF-8 sequence, so codes keep indexing safe.
word_verdict_t scrabble_judge(const uint8_t *codes, uint8_t len);
uint16_t scrabble_score(const uint8_t *codes, uint8_t len);

// Letters that extend `codes` into the first valid word starting with it,
// written to `out`; returns how many. When `codes` is already a word this skips
// it and suggests the first strictly longer one, so the suggestion always tells
// you something the verdict does not. Returns 0 when there is nothing to
// suggest, or when the completion would not fit in `out_max`.
uint8_t scrabble_complete(const uint8_t *codes, uint8_t len,
                          uint8_t *out, uint8_t out_max);

#endif //T_KEYBOARD_SCRABBLE_DICT_MAIN_H
