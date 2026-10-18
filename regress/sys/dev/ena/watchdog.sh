#!/bin/sh
# Compile the driver's negotiation and watchdog against a fake-time model.
set -eu
src=${1:-../../../../sys/dev/pci/if_ena.c}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ena-watchdog.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
awk '
/^ena_admin_init\(struct ena_softc \*sc,/ { print "int"; copy = 1 }
/^ena_tick\(void \*arg\)/ { print "void"; copy = 1 }
copy { print }
copy && /^}/ { copy = 0 }
' "$src" > "$work/driver.c"
${CC:-cc} -std=c99 -Wall -Wextra -Werror -I"$work" \
    "$here/watchdog.c" -o "$work/watchdog"
"$work/watchdog"
