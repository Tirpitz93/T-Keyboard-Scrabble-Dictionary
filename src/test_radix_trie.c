/*
 * Host-side regression test for the serialized Patricia trie emitted by
 * compile_word_trie.py. Keeps the generator testable without flashing a board.
 *
 *   gcc -O1 -I tools -o /tmp/trie_test tools/test_radix_trie.c
 *   /tmp/trie_test "res/Collins Scrabble Words (2019).txt"
 *
 * Asserts that (a) every word in the list resolves as both a word and a prefix,
 * (b) a fixed set of non-words fails both query modes, and (c) real prefixes of
 * real words report prefix-only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "word_radix_trie.h"
#include "radix_trie_complete.h"

#define DEFAULT_WORD_LIST "res/Collins Scrabble Words (2019).txt"
#define MAX_LINE 256

static const uint32_t *const NODES = WORD_RADIX_TRIE_DATA;
static const uint8_t *const POOL = WORD_RADIX_LABEL_POOL;

static int failures = 0;

static bool is_word(const char *s) {
    return radix_trie_contains_word(NODES, WORD_RADIX_TRIE_DATA_COUNT, POOL, s);
}

static bool is_prefix(const char *s) {
    return radix_trie_has_prefix(NODES, WORD_RADIX_TRIE_DATA_COUNT, POOL, s);
}

/*
 * The generator normalizes every entry with Python's str.upper() before storing
 * it, so the test has to apply the same folding. Only one non-ASCII letter is in
 * play: U+00F6 (UTF-8 C3 B6) folds to U+00D6 (UTF-8 C3 96), the 27th alphabet code.
 */
static void normalize(char *s) {
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z') {
            s[i] = (char)(c - 'a' + 'A');
        } else if (c == 0xC3 && (unsigned char)s[i + 1] == 0xB6) {
            s[i + 1] = (char)0x96;
            i++;
        }
    }
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

/* --- helpers for the autocomplete cross-check ------------------------------ */

static int to_codes(const char *s, uint8_t *out, int max) {
    int n = 0;
    while (*s) {
        size_t step = 0;
        int c = radix_get_char_code_from_table(s, &step);
        if (c < 0 || n >= max) return -1;
        out[n++] = (uint8_t)c;
        s += step;
    }
    return n;
}

static void from_codes(const uint8_t *codes, int len, char *buf, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < len; i++) {
        const char *g = RADIX_ALPHABET_TABLE[codes[i]];
        while (*g && n + 1 < cap) buf[n++] = *g++;
    }
    buf[n] = '\0';
}

/* Orders two normalized words by alphabet code, which is what the trie's
   sibling ordering means -- not by byte, though for this alphabet they agree. */
static int cmp_word_order(const char *a, const char *b) {
    while (*a && *b) {
        size_t sa = 0, sb = 0;
        int ca = radix_get_char_code_from_table(a, &sa);
        int cb = radix_get_char_code_from_table(b, &sb);
        if (ca < 0 || cb < 0) return 0;
        if (ca != cb) return ca < cb ? -1 : 1;
        a += sa;
        b += sb;
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

/* Independent answer to "first word with this prefix", by scanning the list.
   Comparing bytes for the prefix test is sound because UTF-8 is prefix-free
   and both sides went through the same normalize(). */
static bool brute_first(const char *path, const char *prefix, bool strict,
                        char *best, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[MAX_LINE];
    size_t plen = strlen(prefix);
    bool found = false;
    best[0] = '\0';

    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (line[0] == '\0') continue;
        normalize(line);
        if (strncmp(line, prefix, plen) != 0) continue;
        if (strict && line[plen] == '\0') continue;
        if (!found || cmp_word_order(line, best) < 0) {
            strncpy(best, line, cap - 1);
            best[cap - 1] = '\0';
            found = true;
        }
    }
    fclose(f);
    return found;
}

static void expect(bool ok, const char *what, const char *subject) {
    if (!ok) {
        failures++;
        printf("  FAIL %-12s %s\n", what, subject);
    }
}

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : DEFAULT_WORD_LIST;

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "cannot open word list: %s\n", path);
        return 2;
    }

    printf("trie: %u nodes, %u bytes + %u byte label pool, alphabet %d\n",
           (unsigned)WORD_RADIX_TRIE_DATA_COUNT,
           (unsigned)WORD_RADIX_TRIE_DATA_SIZE_BYTES,
           (unsigned)WORD_RADIX_LABEL_POOL_SIZE_BYTES,
           RADIX_ALPHABET_SIZE);

    char line[MAX_LINE];
    unsigned long total = 0, missing = 0, not_prefix = 0;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (line[0] == '\0') continue;
        normalize(line);
        total++;

        if (!is_word(line)) {
            if (missing < 10) printf("  MISSING word: %s\n", line);
            missing++;
        }
        /* Every stored word is trivially a prefix of itself. */
        if (!is_prefix(line)) {
            if (not_prefix < 10) printf("  MISSING prefix: %s\n", line);
            not_prefix++;
        }
    }
    fclose(f);

    printf("words=%lu missing=%lu missing_prefix=%lu\n", total, missing, not_prefix);
    if (missing || not_prefix) failures++;

    /* Non-words must fail both query modes. */
    static const char *const non_words[] = {
        "BLORP", "ZZZZZ", "QWERTYU", "AARDWOLF", "XQJ",
    };
    printf("non-words (expect neither word nor prefix):\n");
    for (size_t i = 0; i < sizeof non_words / sizeof non_words[0]; i++) {
        const char *w = non_words[i];
        printf("  %-10s word=%d prefix=%d\n", w, is_word(w), is_prefix(w));
        expect(!is_word(w), "is_word", w);
        expect(!is_prefix(w), "is_prefix", w);
    }

    /* Real prefixes of real words must be prefix-only. */
    /* BLOR belongs here, not with the non-words: it still reaches BLORE, so the
       BLORP dead end only appears once the P is typed. */
    static const char *const prefixes[] = {
        "SCRABB", "ZYG", "JUKEB", "AARDW", "BLOR",
    };
    printf("prefixes (expect prefix only):\n");
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        const char *w = prefixes[i];
        printf("  %-10s word=%d prefix=%d\n", w, is_word(w), is_prefix(w));
        expect(is_prefix(w), "is_prefix", w);
        expect(!is_word(w), "not_word", w);
    }

    /* Complete words, including the one that needs the 27th letter. */
    static const char *const words[] = {
        "HELLO", "HELL", "QI", "JUKEBOX", "SCRABBLE",
        "AARDW\303\226LF", "AASV\303\226GELS",
    };
    printf("words (expect both):\n");
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        const char *w = words[i];
        printf("  %-14s word=%d prefix=%d\n", w, is_word(w), is_prefix(w));
        expect(is_word(w), "is_word", w);
        expect(is_prefix(w), "is_prefix", w);
    }

    /*
     * Autocomplete: the trie's answer must equal a brute-force scan of the list.
     * Non-strict counts the prefix itself when it is a word; strict demands a
     * strictly longer word, which is what the device shows as ghost text.
     */
    static const char *const completion_cases[] = {
        "SCRABB", "ZYG", "JUKEB", "AARDW", "BLOR", "Q", "HELL",
        "AASV", "A", "ZZ", "QWERTYU", "HELLO",
    };
    printf("autocomplete (trie vs. brute-force scan):\n");
    for (size_t i = 0; i < sizeof completion_cases / sizeof completion_cases[0]; i++) {
        const char *p = completion_cases[i];

        uint8_t codes[64];
        int len = to_codes(p, codes, (int)sizeof codes);
        if (len <= 0) { failures++; printf("  FAIL to_codes %s\n", p); continue; }

        for (int strict = 0; strict <= 1; strict++) {
            uint8_t tail[64];
            uint8_t n = radix_trie_complete(NODES, WORD_RADIX_TRIE_DATA_COUNT, POOL,
                                            codes, (uint8_t)len,
                                            tail, (uint8_t)sizeof tail, strict != 0);

            char got[MAX_LINE], tail_text[MAX_LINE];
            from_codes(tail, n, tail_text, sizeof tail_text);
            snprintf(got, sizeof got, "%s%s", p, tail_text);

            char want[MAX_LINE];
            bool any = brute_first(path, p, strict != 0, want, sizeof want);
            if (!any) want[0] = '\0';

            /* n == 0 means "no suggestion", which must line up with the scan
               finding nothing -- except non-strict, where the prefix itself
               being the answer also yields an empty tail. */
            const char *shown = (n == 0 && !any) ? "(none)" : got;
            printf("  %-9s strict=%d -> %-14s want %-14s\n",
                   p, strict, shown, any ? want : "(none)");

            if (any) {
                expect(strcmp(got, want) == 0, strict ? "complete/s" : "complete", p);
                expect(is_word(got), "suggestion", got);
            } else {
                expect(n == 0, strict ? "no-complete/s" : "no-complete", p);
            }
        }
    }

    printf(failures ? "FAILED (%d)\n" : "OK (%d failures)\n", failures);
    return failures ? 1 : 0;
}
