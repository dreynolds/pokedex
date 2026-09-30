# pokedex

Firmware for an ESP32-2432S028R ("CYD") board that shows a random card from
your Pokémon wantlist (from the an API), swapping to
a new one every 10 seconds. The next card's art is prefetched in the
background while the current one is on screen, so swaps are near-instant.

## Setup

1. Copy `include/secrets.h.example` to `include/secrets.h` and fill in your
   Wi-Fi credentials and `POKEMON_API_BASE_URL`.
2. Find the board's serial port (`ls /dev/cu.*` on macOS) and set
   `upload_port`/`monitor_port` in `platformio.ini` if it's not already
   `/dev/cu.usbserial-832130`.
3. `just upload`

## Commands

| Command           | Does                                              |
| ------------------ | -------------------------------------------------- |
| `just build`       | Compile only                                        |
| `just upload`       | Build and flash to the device                       |
| `just monitor`      | Open the serial monitor                             |
| `just run`          | Build, flash, then open the serial monitor          |
| `just compiledb`    | Regenerate `compile_commands.json` for VS Code IntelliSense |
| `just clean`        | Remove build artifacts                              |

Run `just compiledb` (then reload the VS Code window) after changing
`lib_deps` or `build_flags` in `platformio.ini`, or if `#include`s start
showing as unresolved.
