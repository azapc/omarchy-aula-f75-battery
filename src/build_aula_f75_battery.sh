#!/bin/sh
# Build the hidraw battery helper on first run and cache it by source checksum,
# so the shell rebuilds only when the C file actually changes. Prints the path
# to the binary on stdout; BatteryService.qml reads that and runs it.
#
# Derived from build_steam_controller_battery.sh in hl.peripheral_battery
# by hlasensky (https://github.com/hlasensky/omarchy-peripheral-battery),
# used under the MIT License.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_file="$script_dir/aula_f75_battery.c"
cache_root="${XDG_CACHE_HOME:-${HOME}/.cache}/omarchy-aula-f75-battery"

checksum_output=$(cksum < "$source_file")
source_checksum=${checksum_output%% *}
source_size=${checksum_output#* }
source_size=${source_size%% *}
binary_dir="$cache_root/$source_checksum-$source_size-v1"
binary_path="$binary_dir/aula-f75-battery"

if [ ! -x "$binary_path" ]; then
    command -v cc >/dev/null 2>&1 || {
        echo "A C compiler is required to build the AULA F75 battery helper" >&2
        exit 2
    }

    mkdir -p "$binary_dir"
    temporary_binary=$(mktemp "$binary_dir/.aula-f75-battery.XXXXXX")
    trap 'rm -f "$temporary_binary"' EXIT HUP INT TERM

    cc -O2 -s -std=c11 -Wall -Wextra -Wpedantic \
        "$source_file" -o "$temporary_binary"
    chmod 700 "$temporary_binary"
    mv "$temporary_binary" "$binary_path"
    trap - EXIT HUP INT TERM

    # Each source change keys a new cache directory, so drop the older ones.
    # Only ever inside this plugin's own cache root, and never the build just
    # made. Deleting a binary a running helper was started from is safe: the
    # kernel keeps the inode alive until that process exits.
    for stale_dir in "$cache_root"/*/; do
        [ -d "$stale_dir" ] || continue
        stale_dir=${stale_dir%/}
        [ "$stale_dir" = "$binary_dir" ] && continue
        rm -rf -- "$stale_dir"
    done
fi

printf '%s\n' "$binary_path"
