
import math
import os
import struct
from collections import deque

word_list_path = r"Collins Scrabble Words (2019).txt"

word_list = []
if os.path.exists(word_list_path):
    with open(word_list_path, "r", encoding="utf-8") as f:
        for line in f:
            w = line.strip().upper()
            if w:
                word_list.append(w)
        print(f"Size of word list on disk: {os.path.getsize(word_list_path):,} bytes")
else:
    # Fallback sample words (including German characters with umlauts & sharp S 'ß'/'ẞ').
    # Normalized with the same .upper() the file path applies, otherwise the stored
    # words and the lookups disagree: Python uppercases 'ß' to 'SS', so an
    # un-normalized "FUß" would be inserted verbatim but searched for as "FUSS".
    word_list = [w.upper() for w in
                 ["CAT", "CATS", "CATER", "CALL", "DOG", "DOGS", "DO", "BEE", "BEES", "BE", "BED",
                  "FUß", "FÜßE", "SCHÖN", "ÄPFEL", "ÜBER", "GRÜßEN", "STRAßE"]]

# =====================================================================
# DYNAMIC ALPHABET EXTRACTION & BIT ALLOCATION
# =====================================================================
# Extract exactly all distinct characters that appear in the word list
unique_chars = sorted(set("".join(word_list)))
alphabet_size = len(unique_chars)

# Dynamic mapping: char <-> integer code [0 .. alphabet_size - 1]
char_to_code = {c: i for i, c in enumerate(unique_chars)}
code_to_char = {i: c for i, c in enumerate(unique_chars)}

# Calculate the minimum number of bits needed to store alphabet_size characters
char_bits = max(1, math.ceil(math.log2(alphabet_size))) if alphabet_size > 0 else 1
letter_mask = (1 << char_bits) - 1

# Node Bitfield Layout (32-bit uint32_t):
#   - Bits 0 .. (char_bits - 1) : dynamically encoded letter code
#   - Bit (char_bits + 0)       : FLAG_END_OF_WORD
#   - Bit (char_bits + 1)       : FLAG_LAST_SIBLING
#   - Bit (char_bits + 2)       : FLAG_HAS_CHILD
#   - Bits (char_bits + 3) .. 31: child_node_index (up to 32 - (char_bits + 3) bits for index)

flag_end_of_word = 1 << (char_bits + 0)
flag_last_sibling = 1 << (char_bits + 1)
flag_has_child = 1 << (char_bits + 2)
child_index_shift = char_bits + 3

node_struct_format = "<I"
node_size = struct.calcsize(node_struct_format)

# =====================================================================
# RADIX (PATRICIA) TRIE BIT ALLOCATION
# =====================================================================
# A radix trie collapses every chain of single-child nodes that is not itself
# the end of a word into ONE node carrying a multi-character edge label. That
# removes ~44% of the nodes of the plain trie, but each surviving node now needs
# to store a variable-length label, so the per-node cost must stay tiny for the
# compression to pay off. The layout below therefore:
#
#   * spends only ONE 32-bit word on a node, and appends a SECOND word holding
#     the child index only for nodes that actually have children (leaves, which
#     are ~52% of all radix nodes, stay 4 bytes),
#   * stores short labels (the vast majority: ~90% are 1-3 characters) INLINE in
#     the header's payload field, so they cost nothing extra,
#   * stores long labels in a shared, bit-packed character pool that is
#     de-duplicated by substring merging (a label already occurring anywhere in
#     the pool reuses that offset).
#
# Radix Node Header Layout (32-bit uint32_t):
#   - Bits 0 .. (radix_len_bits - 1)   : label length (1 .. radix_max_label_len)
#   - Bit (radix_len_bits + 0)         : RADIX_FLAG_END_OF_WORD
#   - Bit (radix_len_bits + 1)         : RADIX_FLAG_LAST_SIBLING
#   - Bit (radix_len_bits + 2)         : RADIX_FLAG_HAS_CHILD
#   - Bits (radix_len_bits + 3) .. 31  : payload
#                                          label length <= inline_max_chars ->
#                                              label chars packed LSB-first
#                                          otherwise -> character offset into
#                                              the shared label pool
# Followed by (only when RADIX_FLAG_HAS_CHILD is set):
#   - One uint32_t holding the WORD index of the first child's header.
#
# Whether the payload is an inline label or a pool offset is derived from the
# label length, so no extra flag bit is needed. Labels longer than
# radix_max_label_len are split into a chain of nodes during construction.

radix_len_bits = 5
radix_max_label_len = (1 << radix_len_bits) - 1
radix_len_mask = radix_max_label_len

radix_flag_end_of_word = 1 << (radix_len_bits + 0)
radix_flag_last_sibling = 1 << (radix_len_bits + 1)
radix_flag_has_child = 1 << (radix_len_bits + 2)
radix_payload_shift = radix_len_bits + 3
radix_payload_bits = 32 - radix_payload_shift
radix_payload_mask = (1 << radix_payload_bits) - 1

# How many label characters fit into the header payload field
radix_inline_max_chars = radix_payload_bits // char_bits

# The C pool reader over-reads up to 3 bytes to extract a straddling code
radix_pool_padding = 3

print(f"Alphabet size ({alphabet_size} distinct characters): {repr(''.join(unique_chars))}")
print(f"Allocated {char_bits} bits per character (Supports max {1 << char_bits} distinct symbols).")
print(f"Child pointer capacity: 2^{32 - child_index_shift} ({1 << (32 - child_index_shift):,} nodes max).")
print(f"Radix labels: up to {radix_max_label_len} chars, {radix_inline_max_chars} stored inline, "
      f"pool capacity {1 << radix_payload_bits:,} chars.")


# =====================================================================
# PLAIN TRIE (ONE NODE PER CHARACTER)
# =====================================================================

class Node:
    def __init__(self, letter):
        self.letter = letter
        self.children = {}
        self.is_end_of_word = False

tree = Node(None)

def insert_word(word):
    current_node = tree
    for letter in word:
        if letter not in current_node.children:
            current_node.children[letter] = Node(letter)
        current_node = current_node.children[letter]
    current_node.is_end_of_word = True

for word in word_list:
    insert_word(word)

def serialize_trie(root_node):
    """
    Serializes a Trie tree into a contiguous bytearray of packed 32-bit nodes using BFS.
    Characters are dynamically mapped to the minimal bit-width required by the word list.
    Returns: bytes containing the flat binary trie.
    """
    if not root_node.children:
        return b""

    nodes = []

    # Queue contains: (parent_node_idx, sorted_children_list)
    queue = deque()
    queue.append((None, sorted(root_node.children.values(), key=lambda n: n.letter)))

    while queue:
        parent_node_idx, children = queue.popleft()
        current_node_idx = len(nodes)

        # Patch parent's child_node_index field if this is not root
        if parent_node_idx is not None:
            nodes[parent_node_idx] |= (current_node_idx << child_index_shift)

        num_children = len(children)
        for i, child in enumerate(children):
            letter = child.letter
            letter_code = char_to_code[letter] & letter_mask
            node_val = letter_code

            if child.is_end_of_word:
                node_val |= flag_end_of_word
            if i == num_children - 1:
                node_val |= flag_last_sibling
            if child.children:
                node_val |= flag_has_child

            my_idx = len(nodes)
            nodes.append(node_val)

            if child.children:
                queue.append((my_idx, sorted(child.children.values(), key=lambda n: n.letter)))

    return struct.pack(f"<{len(nodes)}I", *nodes)


# =====================================================================
# RADIX TRIE CONSTRUCTION & SERIALIZATION
# =====================================================================

class RadixNode:
    def __init__(self, label, is_end_of_word=False):
        self.label = label
        self.children = {}          # keyed by first character of the child label
        self.is_end_of_word = is_end_of_word

def _sibling_key(node):
    """Siblings are ordered by the code of their first label character, so the
    embedded lookup can pick a branch by comparing a single character."""
    return char_to_code[node.label[0]]

def _attach_label(parent, label, is_end_of_word):
    """
    Attaches `label` below `parent`, splitting it into a chain of nodes if it
    exceeds radix_max_label_len. Returns the tail node, which is the one that
    carries the end-of-word flag and receives the real children.
    """
    while len(label) > radix_max_label_len:
        segment = label[:radix_max_label_len]
        label = label[radix_max_label_len:]
        link = RadixNode(segment)
        parent.children[segment[0]] = link
        parent = link

    tail = RadixNode(label, is_end_of_word)
    parent.children[label[0]] = tail
    return tail

def build_radix_trie(plain_root):
    """
    Compresses a plain character trie into a radix trie: every maximal chain of
    single-child nodes that does not terminate a word becomes one multi-character
    edge label. Iterative, so a deep word list cannot blow the recursion limit.
    """
    radix_root = RadixNode("")
    stack = [(radix_root, child) for child in plain_root.children.values()]

    while stack:
        parent, plain_node = stack.pop()

        # Absorb the single-child chain starting at plain_node
        label = plain_node.letter
        node = plain_node
        while len(node.children) == 1 and not node.is_end_of_word:
            only_child, = node.children.values()
            label += only_child.letter
            node = only_child

        radix_node = _attach_label(parent, label, node.is_end_of_word)
        for grandchild in node.children.values():
            stack.append((radix_node, grandchild))

    return radix_root

def pack_char_codes(codes):
    """Bit-packs a sequence of character codes into bytes, char_bits each, LSB-first."""
    out = bytearray()
    acc = 0
    pending = 0
    for code in codes:
        acc |= (code & letter_mask) << pending
        pending += char_bits
        while pending >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            pending -= 8
    if pending:
        out.append(acc & 0xFF)
    return bytes(out)

def read_pool_char(pool, char_offset):
    """Reads one bit-packed character code from the label pool (mirrors the C reader)."""
    bit_pos = char_offset * char_bits
    byte_pos, shift = divmod(bit_pos, 8)
    acc = pool[byte_pos] | (pool[byte_pos + 1] << 8) | (pool[byte_pos + 2] << 16)
    return (acc >> shift) & letter_mask

def _build_label_pool(root_node):
    """
    Collects every label too long to be stored inline and packs them into one
    shared character pool, de-duplicated by substring merging: labels are placed
    longest-first, and any label that already occurs somewhere in the pool simply
    reuses that offset (e.g. "NESSES" is free once "FULNESSES" is stored).
    Returns: (packed_pool_bytes, {label: char_offset}, pool_char_count)
    """
    pooled_labels = set()
    stack = [root_node]
    while stack:
        node = stack.pop()
        for child in node.children.values():
            if len(child.label) > radix_inline_max_chars:
                pooled_labels.add(child.label)
            stack.append(child)

    pool_text = ""
    label_offset = {}
    # Longest first, then alphabetically: length alone is not a total order, and
    # tie-breaking by set iteration order would make the pool layout (and the
    # generated header) differ between runs with different string hash seeds.
    for label in sorted(pooled_labels, key=lambda s: (-len(s), s)):
        offset = pool_text.find(label)
        if offset < 0:
            offset = len(pool_text)
            pool_text += label
        label_offset[label] = offset

    if len(pool_text) > radix_payload_mask:
        raise ValueError(f"Label pool needs {len(pool_text):,} characters but only "
                         f"{radix_payload_mask:,} are addressable with {radix_payload_bits} "
                         f"payload bits. Increase radix_payload_bits by lowering radix_len_bits.")

    packed = pack_char_codes(char_to_code[c] for c in pool_text)
    return packed + bytes(radix_pool_padding), label_offset, len(pool_text)

def serialize_radix_trie(root_node):
    """
    Serializes a radix trie into a flat uint32 word stream plus a shared label
    pool, using BFS so that siblings are contiguous.

    Returns: (node_blob, pool_blob, stats_dict)
    """
    pool_blob, label_offset, pool_chars = _build_label_pool(root_node)
    stats = {"nodes": 0, "words": 0, "inline_labels": 0, "pooled_labels": 0,
             "pool_chars": pool_chars, "pool_bytes": len(pool_blob),
             "label_chars": 0, "max_label_len": 0}

    if not root_node.children:
        return b"", pool_blob, stats

    words = []

    # Queue contains: (word index of the parent's child-pointer slot, sorted siblings)
    queue = deque()
    queue.append((None, sorted(root_node.children.values(), key=_sibling_key)))

    while queue:
        child_slot, siblings = queue.popleft()

        # Patch the parent's child pointer to where this sibling group starts
        if child_slot is not None:
            words[child_slot] = len(words)

        last = len(siblings) - 1
        for i, node in enumerate(siblings):
            label_len = len(node.label)
            header = label_len

            if node.is_end_of_word:
                header |= radix_flag_end_of_word
            if i == last:
                header |= radix_flag_last_sibling
            if node.children:
                header |= radix_flag_has_child

            if label_len <= radix_inline_max_chars:
                payload = 0
                for k, ch in enumerate(node.label):
                    payload |= char_to_code[ch] << (k * char_bits)
                stats["inline_labels"] += 1
            else:
                payload = label_offset[node.label]
                stats["pooled_labels"] += 1

            words.append(header | (payload << radix_payload_shift))

            stats["nodes"] += 1
            stats["label_chars"] += label_len
            stats["max_label_len"] = max(stats["max_label_len"], label_len)

            if node.children:
                slot = len(words)
                words.append(0)  # placeholder, patched when the group is emitted
                queue.append((slot, sorted(node.children.values(), key=_sibling_key)))

    stats["words"] = len(words)
    return struct.pack(f"<{len(words)}I", *words), pool_blob, stats


# =====================================================================
# C HEADER EXPORT
# =====================================================================

def _write_uint32_array(f, declaration, values, per_line=8):
    """Writes `values` as a C uint32_t initializer list, buffered for speed."""
    f.write(declaration + " = {\n    ")
    chunk = []
    for i, val in enumerate(values, start=1):
        chunk.append(f"0x{val:08X}U, ")
        if i % per_line == 0:
            chunk.append("\n    ")
        if len(chunk) >= 4096:
            f.write("".join(chunk))
            chunk.clear()
    f.write("".join(chunk))
    f.write("\n};\n\n")

def c_string_literal(ch):
    """
    Renders one character as a C string literal holding its UTF-8 bytes.

    Every non-trivial byte is emitted as a 3-digit octal escape: octal escapes
    are capped at three digits, so unlike \\x they can never swallow a following
    character. Using unicode_escape here would be wrong - it emits 'O' with a
    diaeresis as "\\xd6" (one Latin-1 byte), which never matches the two UTF-8
    bytes the firmware actually receives.
    """
    out = []
    for byte in ch.encode("utf-8"):
        if 0x20 <= byte < 0x7F and byte not in (0x22, 0x5C):  # printable, not " or \
            out.append(chr(byte))
        else:
            out.append(f"\\{byte:03o}")
    return '"' + "".join(out) + '"'

def _write_alphabet_table(f, name):
    """Writes the dynamic char code -> UTF-8 string table."""
    f.write("/* Dynamic Alphabet Character Table (UTF-8 encoded, one entry per character code) */\n")
    f.write(f"static const char* const {name}[{alphabet_size}] = {{\n    ")
    for i, ch in enumerate(unique_chars):
        f.write(f"{c_string_literal(ch)}, ")
        if (i + 1) % 10 == 0 and (i + 1) != alphabet_size:
            f.write("\n    ")
    f.write("\n};\n\n")


def export_to_c_header(blob, filename="src/word_trie.h", var_name="WORD_TRIE_DATA"):
    """
    Exports the serialized binary blob and dynamic character map into a C/C++ header file
    for embedded firmware (stored in Flash/PROGMEM/ROM).
    """
    node_count = len(blob) // node_size
    uint32_array = struct.unpack(f"<{node_count}I", blob)

    with open(filename, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated Dynamic Compact Trie binary data for embedded word search */\n")
        f.write("#ifndef WORD_TRIE_H\n#define WORD_TRIE_H\n\n")
        f.write("#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\n\n")
        f.write(f"#define {var_name}_COUNT {node_count}\n")
        f.write(f"#define {var_name}_SIZE_BYTES {len(blob)}\n")
        f.write(f"#define ALPHABET_SIZE {alphabet_size}\n")
        f.write(f"#define CHAR_BITS {char_bits}\n")
        f.write(f"#define LETTER_MASK 0x{letter_mask:X}U\n")
        f.write(f"#define FLAG_END_OF_WORD 0x{flag_end_of_word:X}U\n")
        f.write(f"#define FLAG_LAST_SIBLING 0x{flag_last_sibling:X}U\n")
        f.write(f"#define FLAG_HAS_CHILD 0x{flag_has_child:X}U\n")
        f.write(f"#define CHILD_INDEX_SHIFT {child_index_shift}\n\n")

        # Encode alphabet mapping table (as UTF-8 strings / codepoints)
        _write_alphabet_table(f, "ALPHABET_TABLE")

        _write_uint32_array(f, f"static const uint32_t {var_name}[{node_count}]", uint32_array)

        # Embedded C helpers & lookup functions
        f.write("""
/* Returns code for character if found in dynamic alphabet table, or -1 if invalid */
static inline int get_char_code_from_table(const char *c_str, size_t *char_byte_len) {
    if (!c_str || !*c_str) return -1;
    // Direct match against dynamic alphabet strings
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        const char *entry = ALPHABET_TABLE[i];
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

static inline bool trie_contains_word(const uint32_t *nodes, uint32_t node_count, const char *word) {
    if (!word || !*word || node_count == 0) return false;
    uint32_t node_idx = 0;

    while (*word) {
        size_t byte_len = 0;
        int code = get_char_code_from_table(word, &byte_len);
        if (code < 0) return false;
        word += byte_len;

        uint32_t target_code = (uint32_t)code;
        bool found_child = false;

        while (node_idx < node_count) {
            uint32_t node = nodes[node_idx];
            uint32_t letter_code = node & LETTER_MASK;

            if (letter_code == target_code) {
                if (*word == '\\0') {
                    return (node & FLAG_END_OF_WORD) != 0;
                }
                if (!(node & FLAG_HAS_CHILD)) return false;
                node_idx = node >> CHILD_INDEX_SHIFT;
                found_child = true;
                break;
            }

            if (node & FLAG_LAST_SIBLING) break;
            node_idx++;
        }

        if (!found_child) return false;
    }
    return false;
}

static inline bool trie_has_prefix(const uint32_t *nodes, uint32_t node_count, const char *prefix) {
    if (!prefix || !*prefix || node_count == 0) return false;
    uint32_t node_idx = 0;

    while (*prefix) {
        size_t byte_len = 0;
        int code = get_char_code_from_table(prefix, &byte_len);
        if (code < 0) return false;
        prefix += byte_len;

        uint32_t target_code = (uint32_t)code;
        bool found_child = false;

        while (node_idx < node_count) {
            uint32_t node = nodes[node_idx];
            uint32_t letter_code = node & LETTER_MASK;

            if (letter_code == target_code) {
                if (*prefix == '\\0') return true;
                if (!(node & FLAG_HAS_CHILD)) return false;
                node_idx = node >> CHILD_INDEX_SHIFT;
                found_child = true;
                break;
            }

            if (node & FLAG_LAST_SIBLING) break;
            node_idx++;
        }

        if (!found_child) return false;
    }
    return true;
}
#endif /* WORD_TRIE_H */
""")


def export_radix_to_c_header(node_blob, pool_blob, filename="word_radix_trie.h",
                             var_name="WORD_RADIX_TRIE_DATA", pool_name="WORD_RADIX_LABEL_POOL"):
    """
    Exports the serialized radix trie (node word stream + bit-packed label pool)
    into a C/C++ header file for embedded firmware.

    The alphabet table and char decoder are duplicated here under RADIX_ names so
    that this header is self-contained and can be included alongside word_trie.h
    when comparing both layouts on device.
    """
    word_count = len(node_blob) // 4
    uint32_array = struct.unpack(f"<{word_count}I", node_blob)
    with open(filename[:-2]+"_data.h", "w", encoding="utf-8") as f:
        f.write("/* Auto-generated Dynamic Compact RADIX (Patricia) Trie binary data for embedded word search */\n")
        f.write("#ifndef WORD_RADIX_TRIE_DATA_H\n#define WORD_RADIX_TRIE_DATA_H\n\n")
        f.write("#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\n\n")
        f.write(f"#define {var_name}_COUNT {word_count}\n")
        f.write(f"#define {var_name}_SIZE_BYTES {len(node_blob)}\n")
        f.write(f"#define {pool_name}_SIZE_BYTES {len(pool_blob)}\n\n")

        _write_uint32_array(f, f"static const uint32_t {var_name}[{word_count}]", uint32_array)

        f.write(f"/* Bit-packed shared label pool ({char_bits} bits per character, LSB-first);\n")
        f.write(f"   the last {radix_pool_padding} bytes are padding so the reader may over-read safely. */\n")
        f.write(f"static const uint8_t {pool_name}[{len(pool_blob)}] = {{\n    ")
        chunk = []
        for i, byte in enumerate(pool_blob, start=1):
            chunk.append(f"0x{byte:02X}, ")
            if i % 16 == 0:
                chunk.append("\n    ")
            if len(chunk) >= 4096:
                f.write("".join(chunk))
                chunk.clear()
        f.write("".join(chunk))
        f.write("\n};\n\n")

        f.write("#endif /* WORD_RADIX_TRIE_DATA_H */\n")
    with open(filename, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated Dynamic Compact RADIX (Patricia) Trie binary data for embedded word search */\n")
        f.write("#ifndef WORD_RADIX_TRIE_H\n#define WORD_RADIX_TRIE_H\n\n")
        f.write("#include <stdint.h>\n#include <stdbool.h>\n#include <stddef.h>\n\n")
        f.write("#include \"" + filename[:-2]+"_data.h\"\n\n")
        f.write(f"#define {var_name}_COUNT {word_count}\n")
        f.write(f"#define {var_name}_SIZE_BYTES {len(node_blob)}\n")
        f.write(f"#define {pool_name}_SIZE_BYTES {len(pool_blob)}\n")
        f.write(f"#define RADIX_ALPHABET_SIZE {alphabet_size}\n")
        f.write(f"#define RADIX_CHAR_BITS {char_bits}\n")
        f.write(f"#define RADIX_LETTER_MASK 0x{letter_mask:X}U\n")
        f.write(f"#define RADIX_LEN_BITS {radix_len_bits}\n")
        f.write(f"#define RADIX_LEN_MASK 0x{radix_len_mask:X}U\n")
        f.write(f"#define RADIX_MAX_LABEL_LEN {radix_max_label_len}\n")
        f.write(f"#define RADIX_INLINE_MAX_CHARS {radix_inline_max_chars}\n")
        f.write(f"#define RADIX_FLAG_END_OF_WORD 0x{radix_flag_end_of_word:X}U\n")
        f.write(f"#define RADIX_FLAG_LAST_SIBLING 0x{radix_flag_last_sibling:X}U\n")
        f.write(f"#define RADIX_FLAG_HAS_CHILD 0x{radix_flag_has_child:X}U\n")
        f.write(f"#define RADIX_PAYLOAD_SHIFT {radix_payload_shift}\n\n")

        _write_alphabet_table(f, "RADIX_ALPHABET_TABLE")

        f.write(f"/* Bit-packed shared label pool ({char_bits} bits per character, LSB-first);\n")
        f.write(f"   the last {radix_pool_padding} bytes are padding so the reader may over-read safely. */\n")
        # f.write(f"static const uint8_t {pool_name}[{len(pool_blob)}] = {{\n    ")
        # chunk = []
        # for i, byte in enumerate(pool_blob, start=1):
        #     chunk.append(f"0x{byte:02X}, ")
        #     if i % 16 == 0:
        #         chunk.append("\n    ")
        #     if len(chunk) >= 4096:
        #         f.write("".join(chunk))
        #         chunk.clear()
        # f.write("".join(chunk))
        # f.write("\n};\n\n")

        # _write_uint32_array(f, f"static const uint32_t {var_name}[{word_count}]", uint32_array)

        f.write("""
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
                    if (*cursor == '\\0') {
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

                if (*text == '\\0') {
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
""")

# =====================================================================
# PYTHON LOOKUP (SIMULATING EMBEDDED EXECUTION ON THE DYNAMIC BINARY BLOB)
# =====================================================================

def search_blob(blob, word):
    """Checks whether a word exists in the compact serialized binary trie with dynamic encoding."""
    if not word or not blob:
        return False
    word = word.upper()
    node_count = len(blob) // node_size
    node_idx = 0

    for i, char in enumerate(word):
        if char not in char_to_code:
            return False
        target_code = char_to_code[char]
        found = False

        while node_idx < node_count:
            node, = struct.unpack_from(node_struct_format, blob, node_idx * node_size)
            letter_code = node & letter_mask
            if letter_code == target_code:
                if i == len(word) - 1:
                    return bool(node & flag_end_of_word)
                if not (node & flag_has_child):
                    return False
                node_idx = node >> child_index_shift
                found = True
                break
            if node & flag_last_sibling:
                break
            node_idx += 1

        if not found:
            return False
    return False

def starts_with_blob(blob, prefix):
    """Checks whether any word in the serialized trie starts with the given prefix."""
    if not prefix or not blob:
        return False
    prefix = prefix.upper()
    node_count = len(blob) // node_size
    node_idx = 0

    for i, char in enumerate(prefix):
        if char not in char_to_code:
            return False
        target_code = char_to_code[char]
        found = False

        while node_idx < node_count:
            node, = struct.unpack_from(node_struct_format, blob, node_idx * node_size)
            letter_code = node & letter_mask
            if letter_code == target_code:
                if i == len(prefix) - 1:
                    return True
                if not (node & flag_has_child):
                    return False
                node_idx = node >> child_index_shift
                found = True
                break
            if node & flag_last_sibling:
                break
            node_idx += 1

        if not found:
            return False
    return True


# =====================================================================
# PYTHON LOOKUP ON THE SERIALIZED RADIX BLOB (MIRRORS THE C IMPLEMENTATION)
# =====================================================================

def radix_label_char(header, pool, k):
    """Returns the k-th character code of a node's edge label (inline or pooled)."""
    payload = header >> radix_payload_shift
    if (header & radix_len_mask) <= radix_inline_max_chars:
        return (payload >> (k * char_bits)) & letter_mask
    return read_pool_char(pool, payload + k)

def _radix_match(node_blob, pool_blob, text, prefix_mode):
    """
    Walks `text` through the serialized radix trie.
    prefix_mode=False -> True only for complete stored words.
    prefix_mode=True  -> True if any stored word starts with `text`, including
                         prefixes that end in the middle of an edge label.
    """
    if not text or not node_blob:
        return False
    text = text.upper()
    word_count = len(node_blob) // 4
    length = len(text)
    node_idx = 0
    pos = 0

    while pos < length:
        char = text[pos]
        if char not in char_to_code:
            return False
        target_code = char_to_code[char]
        descended = False

        while node_idx < word_count:
            header, = struct.unpack_from("<I", node_blob, node_idx * 4)

            if radix_label_char(header, pool_blob, 0) == target_code:
                label_len = header & radix_len_mask

                # Match the remaining label characters against the input
                for k in range(1, label_len):
                    if pos + k >= length:
                        # Input ran out in the middle of this edge label
                        return prefix_mode
                    next_char = text[pos + k]
                    if char_to_code.get(next_char, -1) != radix_label_char(header, pool_blob, k):
                        return False
                pos += label_len

                if pos == length:
                    return prefix_mode or bool(header & radix_flag_end_of_word)
                if not (header & radix_flag_has_child):
                    return False
                node_idx, = struct.unpack_from("<I", node_blob, (node_idx + 1) * 4)
                descended = True
                break

            if header & radix_flag_last_sibling:
                return False
            node_idx += 2 if (header & radix_flag_has_child) else 1

        if not descended:
            return False
    return prefix_mode

def search_radix_blob(node_blob, pool_blob, word):
    """Checks whether a word exists in the compact serialized radix trie."""
    return _radix_match(node_blob, pool_blob, word, False)

def starts_with_radix_blob(node_blob, pool_blob, prefix):
    """Checks whether any word in the serialized radix trie starts with the given prefix."""
    return _radix_match(node_blob, pool_blob, prefix, True)


# =====================================================================
# SIZE COMPARISON REPORT
# =====================================================================

def print_size_comparison(plain_blob, radix_node_blob, radix_pool_blob, radix_stats):
    plain_nodes = len(plain_blob) // node_size
    plain_total = len(plain_blob)
    radix_total = len(radix_node_blob) + len(radix_pool_blob)
    raw_bytes = sum(len(w.encode("utf-8")) + 1 for w in word_list)

    print("\n=== Layout Size Comparison ===")
    header = f"{'Layout':<14}{'Nodes':>12}{'Node bytes':>14}{'Pool bytes':>12}{'Total':>14}{'vs plain':>10}"
    print(header)
    print("-" * len(header))
    print(f"{'Plain trie':<14}{plain_nodes:>12,}{plain_total:>14,}{0:>12,}{plain_total:>14,}{'100.0%':>10}")
    print(f"{'Radix trie':<14}{radix_stats['nodes']:>12,}{len(radix_node_blob):>14,}"
          f"{len(radix_pool_blob):>12,}{radix_total:>14,}{radix_total / plain_total:>9.1%}")
    print("-" * len(header))
    print(f"Radix saves {plain_total - radix_total:,} bytes "
          f"({1 - radix_total / plain_total:.1%}) and {plain_nodes - radix_stats['nodes']:,} nodes "
          f"({1 - radix_stats['nodes'] / plain_nodes:.1%}).")
    print(f"Raw word list (UTF-8 + newline): {raw_bytes:,} bytes -> "
          f"plain {plain_total / raw_bytes:.1%}, radix {radix_total / raw_bytes:.1%}.")
    print(f"Bytes per word: plain {plain_total / len(word_list):.2f}, "
          f"radix {radix_total / len(word_list):.2f}.")

    print("\n--- Radix internals ---")
    print(f"Node stream: {radix_stats['words']:,} words for {radix_stats['nodes']:,} nodes "
          f"({radix_stats['words'] - radix_stats['nodes']:,} child pointers, "
          f"{radix_stats['words'] / radix_stats['nodes']:.2f} words/node).")
    print(f"Labels: {radix_stats['inline_labels']:,} inline "
          f"({radix_stats['inline_labels'] / radix_stats['nodes']:.1%}, <= {radix_inline_max_chars} chars), "
          f"{radix_stats['pooled_labels']:,} pooled, longest {radix_stats['max_label_len']} chars.")
    print(f"Label pool: {radix_stats['pool_chars']:,} unique chars after substring merging "
          f"(covers {radix_stats['label_chars']:,} total label chars), "
          f"{radix_stats['pool_bytes']:,} bytes packed.")


if __name__ == "__main__":
    print(f"\nLoaded {len(word_list)} words into Python Trie.")
    serialized = serialize_trie(tree)
    print(f"Serialized Trie Size: {len(serialized):,} bytes ({len(serialized)/1024:.2f} KB)")
    print(f"Total Nodes: {len(serialized)//node_size:,}")

    radix_tree = build_radix_trie(tree)
    radix_serialized, radix_pool, radix_stats = serialize_radix_trie(radix_tree)
    print(f"Serialized Radix Trie Size: {len(radix_serialized) + len(radix_pool):,} bytes "
          f"({(len(radix_serialized) + len(radix_pool))/1024:.2f} KB)")
    print(f"Total Radix Nodes: {radix_stats['nodes']:,}")

    with open("trie.bin", "wb") as f:
        f.write(serialized)
    with open("radix_trie.bin", "wb") as f:
        f.write(radix_serializsrced)
    with open("radix_trie_pool.bin", "wb") as f:
        f.write(radix_pool)

    export_to_c_header(serialized, "src/word_trie.h")
    export_radix_to_c_header(radix_serialized, radix_pool, "src/word_radix_trie.h")

    print_size_comparison(serialized, radix_serialized, radix_pool, radix_stats)

    # Verification checks. Test words are compared against the word list using the
    # same .upper() normalization the lookups apply, so a word whose case folding
    # changes its length (e.g. 'ß' -> 'SS') is not reported as a false mismatch.
    word_set = set(word_list)
    test_words = word_list[:20] + word_list[-20:] + ["NONEXISTENTWORD", "XYZ", "CAT", "DOG", "BE", "FÜßE", "STRAßE"]
    print("\n--- Search Verification ---")
    for w in test_words:
        in_orig = w.upper() in word_set
        in_blob = search_blob(serialized, w)
        in_radix = search_radix_blob(radix_serialized, radix_pool, w)
        print(f"Word '{w}': In original list={in_orig}, Found in binary blob={in_blob}, "
              f"Found in radix blob={in_radix}")
        assert in_orig == in_blob, f"Mismatch for word: {w}"
        assert in_orig == in_radix, f"Radix mismatch for word: {w}"

    # Cross-check the radix layout against the plain one. Every word in the list
    # must round-trip through BOTH blobs; for prefixes and for near-misses the
    # plain trie acts as the reference oracle, so positive and negative cases are
    # handled by the same comparison.
    print("\n--- Cross-Check (plain vs radix) ---")
    for w in word_list:
        assert search_blob(serialized, w), f"Plain lost word: {w}"
        assert search_radix_blob(radix_serialized, radix_pool, w), f"Radix lost word: {w}"
    print(f"All {len(word_list):,} words found in both layouts.")

    # Prefix / near-miss probes are quadratic in word length, so run them over a
    # deterministic, evenly spread sample instead of the whole list.
    sample_step = max(1, len(word_list) // 4000)
    sample = word_list[::sample_step]
    candidates = {w[:i] for w in sample for i in range(1, len(w) + 1)}
    # Extend every 10th prefix by one character to produce near-misses
    for p in sorted(candidates)[::10]:
        for ch in unique_chars:
            candidates.add(p + ch)
    candidates.update(["", "NONEXISTENTWORD", "QQQQ", "ZZZZZZ", "A" * 40])

    mismatches = 0
    for c in candidates:
        expect_prefix = starts_with_blob(serialized, c)
        expect_word = c in word_set

        if starts_with_radix_blob(radix_serialized, radix_pool, c) != expect_prefix:
            print(f"  PREFIX MISMATCH for {c!r}: plain={expect_prefix}")
            mismatches += 1
        if search_radix_blob(radix_serialized, radix_pool, c) != expect_word:
            print(f"  WORD MISMATCH for {c!r}: expected={expect_word}")
            mismatches += 1
        if search_blob(serialized, c) != expect_word:
            print(f"  PLAIN WORD MISMATCH for {c!r}: expected={expect_word}")
            mismatches += 1

    assert mismatches == 0, f"{mismatches} plain/radix disagreements"
    print(f"Checked {len(candidates):,} prefixes and near-misses "
          f"(sampled from {len(sample):,} words) - both layouts agree.")

    print("\nAll verification checks passed successfully!")
