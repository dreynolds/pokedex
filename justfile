# Build the firmware
build:
    pio run

# Build and flash the firmware to the device
upload:
    pio run -t upload

# Open a serial monitor on the device (115200 baud, matches platformio.ini)
monitor:
    pio device monitor -b 115200

# Build, flash, and immediately open a serial monitor
run: upload
    pio device monitor -b 115200

# Regenerate compile_commands.json for VS Code IntelliSense
compiledb:
    pio run -t compiledb

# Remove build artifacts
clean:
    pio run -t clean

default: build
