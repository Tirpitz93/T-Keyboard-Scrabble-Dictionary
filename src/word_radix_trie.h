/* Auto-generated Dynamic Compact RADIX (Patricia) Trie binary data for embedded word search */
#ifndef WORD_RADIX_TRIE_H
#define WORD_RADIX_TRIE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "word_radix_trie_data.h"

#define WORD_RADIX_TRIE_DATA_COUNT 509165
#define WORD_RADIX_TRIE_DATA_SIZE_BYTES 2036660
#define WORD_RADIX_LABEL_POOL_SIZE_BYTES 20541
#define RADIX_ALPHABET_SIZE 26
#define RADIX_CHAR_BITS 5
#define RADIX_LETTER_MASK 0x1FU
#define RADIX_LEN_BITS 5
#define RADIX_LEN_MASK 0x1FU
#define RADIX_MAX_LABEL_LEN 31
#define RADIX_INLINE_MAX_CHARS 4
#define RADIX_FLAG_END_OF_WORD 0x20U
#define RADIX_FLAG_LAST_SIBLING 0x40U
#define RADIX_FLAG_HAS_CHILD 0x80U
#define RADIX_PAYLOAD_SHIFT 8

/* Dynamic Alphabet Character Table (UTF-8 encoded, one entry per character code) */
static const char* const RADIX_ALPHABET_TABLE[26] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", 
    "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", 
    "U", "V", "W", "X", "Y", "Z", 
};

/* Bit-packed shared label pool (5 bits per character, LSB-first);
   the last 3 bytes are padding so the reader may over-read safely. */

/* Returns code for character if found in dynamic alphabet table, or -1 if invalid */
static inline int radix_get_char_code_from_table(const char *c_str, size_t *char_byte_len) {
    if (!c_str || !*c_str) return -1;
    for (int i = 0; i < RADIX_ALPHABET_SIZE; i++) {
        const char *entry = RADIX_ALPHABET_TABLE[i];
        size_t len = 0;
        while (entry[len]) len++;

        bool match = true;
        for (size_t k = 0; k < len; k++) {
            if (c_str[k] != entry[k]) {
                match = false;
                break;
            }
        }
        if (match) {
            *char_byte_len = len;
            return i;
        }
    }
    return -1;
}

/* Reads one bit-packed character code out of the shared label pool */
static inline uint32_t radix_pool_char(const uint8_t *pool, uint32_t char_offset) {
    uint32_t bit_pos = char_offset * RADIX_CHAR_BITS;
    uint32_t byte_pos = bit_pos >> 3;
    uint32_t shift = bit_pos & 7u;
    uint32_t acc = (uint32_t)pool[byte_pos]
                 | ((uint32_t)pool[byte_pos + 1] << 8)
                 | ((uint32_t)pool[byte_pos + 2] << 16);
    return (acc >> shift) & RADIX_LETTER_MASK;
}

/* Returns the k-th character code of a node's edge label (inline or pooled) */
static inline uint32_t radix_label_char(const uint8_t *pool, uint32_t header, uint32_t k) {
    uint32_t payload = header >> RADIX_PAYLOAD_SHIFT;
    if ((header & RADIX_LEN_MASK) <= RADIX_INLINE_MAX_CHARS) {
        return (payload >> (k * RADIX_CHAR_BITS)) & RADIX_LETTER_MASK;
    }
    return radix_pool_char(pool, payload + k);
}

/* Word index of the next sibling's header: 1 word for the header, plus 1 for the child pointer */
static inline uint32_t radix_next_sibling(uint32_t node_idx, uint32_t header) {
    return node_idx + ((header & RADIX_FLAG_HAS_CHILD) ? 2u : 1u);
}

/*
 * Walks `text` through the radix trie.
 *   prefix_mode == false -> true only if `text` is a complete stored word
 *   prefix_mode == true  -> true if any stored word starts with `text`
 *                           (this includes prefixes that end inside a label)
 */
static inline bool radix_trie_match(const uint32_t *nodes, uint32_t word_count,
                                    const uint8_t *pool, const char *text,
                                    bool prefix_mode) {
    if (!text || !*text || word_count == 0) return false;
    uint32_t node_idx = 0;

    while (*text) {
        size_t byte_len = 0;
        int code = radix_get_char_code_from_table(text, &byte_len);
        if (code < 0) return false;

        uint32_t target_code = (uint32_t)code;
        bool descended = false;

        while (node_idx < word_count) {
            uint32_t header = nodes[node_idx];

            if (radix_label_char(pool, header, 0) == target_code) {
                uint32_t label_len = header & RADIX_LEN_MASK;
                const char *cursor = text + byte_len;

                /* Match the remaining label characters against the input */
                for (uint32_t k = 1; k < label_len; k++) {
                    if (*cursor == '\0') {
                        /* Input ran out in the middle of this edge label */
                        return prefix_mode;
                    }
                    size_t step = 0;
                    int next_code = radix_get_char_code_from_table(cursor, &step);
                    if (next_code < 0 || (uint32_t)next_code != radix_label_char(pool, header, k)) {
                        return false;
                    }
                    cursor += step;
                }
                text = cursor;

                if (*text == '\0') {
                    return prefix_mode || (header & RADIX_FLAG_END_OF_WORD) != 0;
                }
                if (!(header & RADIX_FLAG_HAS_CHILD)) return false;
                node_idx = nodes[node_idx + 1];
                descended = true;
                break;
            }

            if (header & RADIX_FLAG_LAST_SIBLING) return false;
            node_idx = radix_next_sibling(node_idx, header);
        }

        if (!descended) return false;
    }
    return prefix_mode;
}

static inline bool radix_trie_contains_word(const uint32_t *nodes, uint32_t word_count,
                                            const uint8_t *pool, const char *word) {
    return radix_trie_match(nodes, word_count, pool, word, false);
}

static inline bool radix_trie_has_prefix(const uint32_t *nodes, uint32_t word_count,
                                         const uint8_t *pool, const char *prefix) {
    return radix_trie_match(nodes, word_count, pool, prefix, true);
}
#endif /* WORD_RADIX_TRIE_H */
