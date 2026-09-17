# T-Keyboard Scrabble Dictionary Firmware

A standalone firmware for the LilyGO T-Keyboard that turns the device into a Scrabble word judge.
[![Everything Is AWESOME](https://img.youtube.com/vi/ZCGrr6VZcCE/0.jpg)]("https://youtu.be/ZCGrr6VZcCE?si=6sznrlx1wRNlPA5e" Quick demo")


It evaluates typed words against a compressed Collins Scrabble Words (2019) dictionary stored on-device, and shows:
- [x]  Whether the current input is a valid word
- [x]  Whether it is still a valid prefix
- [x]  Tile score and letter count
- [x]  A ghost-text completion for the first lexicographic matching word
- [ ]  Show valid character combinations (not yet implemented)
- [ ]  Symbols and special characters (not yet implemented)
- [ ]  Support for multiple dictionaries (not yet implemented)

## Controls
-  Use the keyboard to type letters
- `ENTER` clears the current word, 
- `BACKSPACE` deletes one letter, and 
- `ALT+B` toggles the keyboard backlight. 
- `SPACE` wakes the screen again but does not clear the current word.



## Hardware target

- Board: LilyGO T-Keyboard Platform IO target: `ttgo-t-oi-plus`
- Framework: Arduino (PlatformIO)
- Platform: `espressif32 @ 6.1`

See `platformio.ini` for the exact environment configuration.

## Repository layout

- `src/scrabble_dict_main.cpp` – firmware entry and runtime logic
- `src/scrabble_dict_main.h` – public firmware API and verdict types
- `src/word_radix_trie.h` – generated radix trie metadata and lookup helpers
- `src/word_radix_trie_data.h` – generated packed dictionary payload (large file)
- `src/radix_trie_complete.h` – completion lookup logic
- `compile_word_trie.py` – dictionary compiler/generator script
- `partitions_scrabble.csv` – custom partition table to fit dictionary size


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
TODO: Add a `--wordlist` option to specify a custom word list file. and an interactive choice to compile a trie from a custom word list.
The script emits generated headers used by the firmware. Keep generated trie files in sync with the word list whenever dictionary content changes.

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
