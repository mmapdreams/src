#!/bin/sh
# Compile the driver's recovery path against a failed-admin-queue model.
set -eu
src=${1:-../../../../sys/dev/pci/if_ena.c}
testdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ena-reset.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
awk '
/^ena_reset_task\(void \*arg\)/ { print "void"; copy = 1 }
/^ena_reset_device\(struct ena_softc \*sc\)/ { print "int"; copy = 1 }
/^ena_intr_admin\(void \*arg\)/ { print "int"; copy = 1 }
copy { print }
copy && /^}/ { copy = 0 }
' "$src" > "$work/driver.c"
${CC:-cc} -std=c99 -Wall -Wextra -Werror -I"$work" \
    "$testdir/reset.c" -o "$work/reset"
"$work/reset"
