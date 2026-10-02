#!/bin/sh
# prune_disc.sh <dir>: takes out of an unpacked game dir (build/disc/main)
# each original file that a `make assets` file next to it replaces, as the
# Dreamcast build (and the sim, which runs from the same dir) never reads it:
#
#   x.dt              x.tga, x.jpg         (the pvr renderer reads only .dt)
#   x.col and x.wld   x.bsp                (collision and world; the .bsp is
#                                           read only for a map without them)
#   x.aasc            x.aas                (AAS_COMPACT reads only the .aasc)
#   x.mdsc            x.mds
#   x.mdb             x.mdc, x.md3
#
# Only ever an original with its replacement there, so nothing goes that is
# still wanted.
set -e
dir=$1
[ -d "$dir" ] || { echo "prune_disc.sh: no $dir" >&2; exit 1; }

# $1: the replacement's extension, the rest: the originals' it replaces
prune() {
	new=$1
	shift
	find "$dir" -type f -name "*.$new" | while read -r f; do
		stem=${f%.*}
		if [ "$new" = col ] && [ ! -f "$stem.wld" ]; then
			continue
		fi
		for old in "$@"; do
			rm -f "$stem.$old"
		done
	done
}

before=$(du -sk "$dir" | cut -f1)
prune dt tga jpg
prune col bsp
prune aasc aas
prune mdsc mds
prune mdb mdc md3
after=$(du -sk "$dir" | cut -f1)
echo "prune_disc: $(( ( before - after ) / 1024 )) MB of originals with a converted file left out"
