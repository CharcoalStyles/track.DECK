#!/usr/bin/env bash
# Build the firmware, set the esp32s3 target, wait for the device to be
# plugged in, then flash it. See PLAN/README for the manual equivalent.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

CONNECT_TIMEOUT_S=120

if ! command -v idf.py >/dev/null 2>&1; then
    IDF_EXPORT="$HOME/esp/esp-idf/export.sh"
    if [[ -f "$IDF_EXPORT" ]]; then
        echo "Sourcing ESP-IDF environment from $IDF_EXPORT ..."
        # shellcheck disable=SC1090
        source "$IDF_EXPORT"
    else
        echo "error: idf.py not found on PATH and $IDF_EXPORT does not exist." >&2
        echo "Source your ESP-IDF export.sh manually and re-run this script." >&2
        exit 1
    fi
fi

if ! command -v idf.py >/dev/null 2>&1; then
    echo "error: idf.py still not found after sourcing $IDF_EXPORT." >&2
    exit 1
fi

if [[ ! -f main/secrets.h ]]; then
    echo "error: main/secrets.h is missing." >&2
    echo "Run: cp main/secrets.h.example main/secrets.h   # then fill in real values" >&2
    exit 1
fi

echo "==> idf.py set-target esp32s3"
idf.py set-target esp32s3

echo "==> idf.py build"
idf.py build

echo "==> Waiting for device to connect (up to ${CONNECT_TIMEOUT_S}s)..."
shopt -s nullglob
before=( /dev/ttyACM* /dev/ttyUSB* )
shopt -u nullglob

port=""
elapsed=0
while (( elapsed < CONNECT_TIMEOUT_S )); do
    shopt -s nullglob
    now=( /dev/ttyACM* /dev/ttyUSB* )
    shopt -u nullglob

    for node in "${now[@]}"; do
        found=0
        for old in "${before[@]}"; do
            [[ "$node" == "$old" ]] && found=1 && break
        done
        if [[ $found -eq 0 ]]; then
            port="$node"
            break 2
        fi
    done

    sleep 1
    elapsed=$(( elapsed + 1 ))
done

if [[ -z "$port" ]]; then
    echo "error: timed out after ${CONNECT_TIMEOUT_S}s waiting for a new serial device." >&2
    echo "If the device was already plugged in before this script started, unplug it," >&2
    echo "re-run, then plug it back in once the script says it's waiting." >&2
    exit 1
fi

echo "==> Detected device at $port"
echo "==> idf.py -p $port flash"
idf.py -p "$port" flash

echo "==> Flash complete on $port"
echo "    (run 'idf.py -p $port monitor' to view serial output)"
