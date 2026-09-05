#!/bin/sh
# Diagnostics for the AULA F75 receiver: which node is in use, the vendor
# config block, and any unsolicited traffic on the command pipe.
#
# The battery itself is already decoded (command 0xCA, see aula_f75_battery.c).
# This is for probing anything else the keyboard exposes.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
binary=$(sh "$script_dir/../src/build_aula_f75_battery.sh")

echo "helper: $binary"
echo
echo "--- current reading -----------------------------------------------"
"$binary" || echo "(no reading: keyboard asleep, off, or dongle unplugged)"
echo
echo "--- device + feature report 0x06 ----------------------------------"
"$binary" --info 2>&1 >/dev/null || true
echo
echo "--- unsolicited traffic on report 0x13, 4s ------------------------"
echo "Stay idle. Keystroke, media and system reports are suppressed."
"$binary" --raw 2>&1 >/dev/null || true
echo
# Built into a private directory rather than a predictable path in the shared
# /tmp, where another account could win the race and leave a binary of its own
# under the name about to be executed.
echo "To sweep for other commands, build tools/probe_pipe.c and run it:"
echo "  out=\$(mktemp -d \"\${XDG_RUNTIME_DIR:-\$HOME/.cache}/probe.XXXXXX\")"
echo "  cc -O2 -o \"\$out/probe_pipe\" tools/probe_pipe.c"
echo "  \"\$out/probe_pipe\" 0x80 0xff"
