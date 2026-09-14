/*
 * Companion to word_radix_trie.h: lexicographically-first completion lookup.
 *
 * Hand-written, not generated. Kept beside the trie it walks so both the
 * firmware and tools/test_radix_trie.c reach it through the same -I tools.
 *
 * Works on alphabet codes rather than UTF-8, because the caller (a word being
 * typed one letter at a time) already holds codes and the 27th letter is a
 * two-byte sequence.
 *
 * Siblings are emitted by the generator in ascending order of their first label
 * character, so "always take the first child" yields the lexicographically
 * smallest word below a node.
 */
#ifndef RADIX_TRIE_COMPLETE_H
#define RADIX_TRIE_COMPLETE_H

#include "word_radix_trie.h"

/*
 * Finds where `codes` ends inside the trie.
 *
 * On success `*out_node` is the node the walk stopped in and `*out_consumed`
 * is how many of that node's label characters the input used up — which may be
 * fewer than the label length, since a Patricia edge can carry several
 * characters and the input is free to stop in the middle of one.
 *
 * Returns false when no stored word starts with `codes`.
 */
static inline bool radix_trie_locate(const uint32_t *nodes, uint32_t word_count,
                                     const uint8_t *pool,
                                     const uint8_t *codes, uint8_t len,
                                     uint32_t *out_node, uint32_t *out_consumed) {
    if (len == 0 || word_count == 0) return false;

    uint32_t node_idx = 0;
    uint8_t i = 0;

    while (i < len) {
        uint32_t target = codes[i];
        bool descended = false;

        while (node_idx < word_count) {
            uint32_t header = nodes[node_idx];

            if (radix_label_char(pool, header, 0) == target) {
                uint32_t label_len = header & RADIX_LEN_MASK;
                uint32_t k = 1;
                i++;

                while (k < label_len && i < len) {
                    if (radix_label_char(pool, header, k) != (uint32_t)codes[i]) return false;
                    k++;
                    i++;
                }

                if (k < label_len) {            /* input ran out inside the label */
                    *out_node = node_idx;
                    *out_consumed = k;
                    return true;
                }
                if (i == len) {                 /* input ended on the label boundary */
                    *out_node = node_idx;
                    *out_consumed = label_len;
                    return true;
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

    return false;   /* not reached: the loop returns as soon as i reaches len */
}

/*
 * Writes the letters that extend `codes` into the lexicographically first word
 * starting with it, and returns how many were written.
 *
 * `strict` decides what happens when `codes` is already a complete word:
 *   false -> returns 0, i.e. the answer is the typed word itself
 *   true  -> skips it and returns the first strictly longer word instead
 *
 * Returns 0 when there is no such word, and also when the completion would not
 * fit in `out_max` — a truncated suggestion would be a wrong one.
 */
static inline uint8_t radix_trie_complete(const uint32_t *nodes, uint32_t word_count,
                                          const uint8_t *pool,
                                          const uint8_t *codes, uint8_t len,
                                          uint8_t *out, uint8_t out_max, bool strict) {
    uint32_t node_idx = 0;
    uint32_t consumed = 0;
    if (!radix_trie_locate(nodes, word_count, pool, codes, len, &node_idx, &consumed)) {
        return 0;
    }

    uint32_t header = nodes[node_idx];
    uint8_t n = 0;

    /* Finish the label the input stopped inside, if it stopped inside one. */
    for (uint32_t k = consumed; k < (header & RADIX_LEN_MASK); k++) {
        if (n >= out_max) return 0;
        out[n++] = (uint8_t)radix_label_char(pool, header, k);
    }

    for (;;) {
        if ((header & RADIX_FLAG_END_OF_WORD) && !(strict && n == 0)) return n;
        if (!(header & RADIX_FLAG_HAS_CHILD)) return 0;

        node_idx = nodes[node_idx + 1];
        if (node_idx >= word_count) return 0;
        header = nodes[node_idx];

        for (uint32_t k = 0; k < (header & RADIX_LEN_MASK); k++) {
            if (n >= out_max) return 0;
            out[n++] = (uint8_t)radix_label_char(pool, header, k);
        }
    }
}

#endif /* RADIX_TRIE_COMPLETE_H */
