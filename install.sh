#!/bin/sh
# Grant this user access to the AULA F75 receiver.
#
# The kernel creates /dev/hidraw* root-only, so an unprivileged helper cannot
# open the receiver without a udev rule. That rule has to be installed once,
# as root. Omarchy's plugin installer deliberately never runs sudo or install
# hooks, so this is a separate, explicit step.
#
# Run:  sudo ./install.sh

set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "This installs a udev rule and must run as root:" >&2
    echo "  sudo $0" >&2
    exit 1
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
rule=60-aula-f75.rules

cp "$script_dir/udev/$rule" /etc/udev/rules.d/
udevadm control --reload-rules
udevadm trigger --subsystem-match=hidraw --action=add

echo "Installed /etc/udev/rules.d/$rule"
echo
echo "Replug the receiver, then check its nodes show a '+' (an ACL):"
echo "  ls -l /dev/hidraw*"
