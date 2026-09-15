# T-Keyboard Scrabble Dictionary Firmware

A standalone firmware for the LilyGO T-Keyboard that turns the device into a Scrabble word judge.

It evaluates typed words against a compressed Collins Scrabble Words (2019) dictionary stored on-device, and shows:
- whether the current input is a valid word
- whether it is still a valid prefix
- tile score and letter count
- a ghost-text completion for the first lexicographic matching word

## Hardware target

- Board: `ttgo-t-oi-plus`
- Framework: Arduino (PlatformIO)
- Platform: `espressif32 @ 6.1`

See `/home/runner/work/T-Keyboard-Scrabble-Dictionary/T-Keyboard-Scrabble-Dictionary/platformio.ini` for the exact environment configuration.

## Repository layout

- `/home/runner/work/T-Keyboard-Scrabble-Dictionary/T-Keyboard-Scrabble-Dictionary/src/scrabble_dict_main.cpp` – firmware entry and runtime logic
- `/home/runner/work/T-Keyboard-Scrabble-Dictionary/T-Keyboard-Scrabble-Dictionary/src/scrabble_dict_main.h` – public firmware API and verdict types
- `/home/runner/work/T-Keyboard-Scrabble-Dictionary/T-Keyboard-Scrabble-Dictionary/src/word_radix_trie.h` – generated radix trie metadata and lookup helpers
- `/home/runner/work/T-Keyboard-Scrabble-Dictionary/T-Keyboard-Scrabble-Dictionary/src/word_radix_trie_data.h` – generated packed dictionary payload (large file)
- `/home/runner/work/T-Keyboard-Scrabble-Dictionary/T-Keyboard-Scrabble-Dictionary/src/radix_trie_complete.h` – completion lookup logic
- `/home/runner/work/T-Keyboard-Scrabble-Dictionary/T-Keyboard-Scrabble-Dictionary/compile_word_trie.py` – dictionary compiler/generator script
- `/home/runner/work/T-Keyboard-Scrabble-Dictionary/T-Keyboard-Scrabble-Dictionary/partitions_scrabble.csv` – custom partition table to fit dictionary size

## Local development

### Prerequisites

- Python 3.10+
- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)

### Build

```bash
cd /home/runner/work/T-Keyboard-Scrabble-Dictionary/T-Keyboard-Scrabble-Dictionary
pio run -e T-Keyboard-scrabble
```

Output firmware binary:

- `.pio/build/T-Keyboard-scrabble/firmware.bin`

### Flash from PlatformIO

```bash
pio run -e T-Keyboard-scrabble -t upload --upload-port <PORT>
```

### Serial monitor

```bash
pio device monitor -b 115200
```

## Dictionary generation workflow

The trie data is generated from a word list by:

```bash
python compile_word_trie.py
```

The script emits generated headers used by the firmware. Keep generated trie files in sync with the word list whenever dictionary content changes.

## Device behavior / controls

- Type letters on the T-Keyboard matrix
- `Backspace` deletes one letter
- `Enter` or `Space` clears the current word
- `Alt + B` toggles keyboard backlight
- `Symbol` then `O` is reserved for the dictionary's extra letter slot when present in the generated alphabet

## CI/CD: build and release firmware

This repository includes a workflow at:

- `/home/runner/work/T-Keyboard-Scrabble-Dictionary/T-Keyboard-Scrabble-Dictionary/.github/workflows/build-release-firmware.yml`

What it does:
1. Builds `T-Keyboard-scrabble` with PlatformIO
2. Produces standard binaries (`firmware.bin`, `bootloader.bin`, `partitions.bin`)
3. Creates a single merged flash image `firmware-merged.bin` (flash at `0x0`)
4. Uploads all binaries as workflow artifacts
5. On `v*` tags, creates a GitHub Release and attaches binaries + checksums

### Create a release

Push a tag such as `v1.0.0` to trigger build + release publishing.

### Flash merged release binary

```bash
esptool.py --chip esp32 --port <PORT> --baud 921600 write_flash 0x0 firmware-merged.bin
```

## Notes

- The dictionary payload is large (~2 MB), so BLE-related code is excluded in this firmware profile.
- Custom partitions are required; do not remove `board_build.partitions = partitions_scrabble.csv`.
