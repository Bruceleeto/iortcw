#!/bin/sh
# ui_blob.sh <dir>: packs the UI's menus and text in an unpacked game dir
# (build/disc/main) into <dir>/ui.blob, which the game reads in one go while
# the UI starts (FS_LoadBlob in SP/code/qcommon/files.c):
#
#   DCBLOB <count>
#   <size>\t<path>       a line a file
#   the files, one after another, in that order
#
# The UI opens over 500 of them as it starts, each a trip to the disc. The
# files stay on the disc as well. Its textures (.dt) aren't in it: they're
# most of the bytes, few of the files.
set -e
dir=$1
[ -d "$dir" ] || { echo "ui_blob.sh: no $dir" >&2; exit 1; }
cd "$dir"

list=$(find ui text -type f \( -name '*.menu' -o -name '*.txt' -o -name '*.h' \) | LC_ALL=C sort)
{
	echo "DCBLOB $(echo "$list" | wc -l)"
	echo "$list" | while read -r f; do printf '%s\t%s\n' "$(stat -c %s "$f")" "$f"; done
	echo "$list" | while read -r f; do cat "$f"; done
} > ui.blob
