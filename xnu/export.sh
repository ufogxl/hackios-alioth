#!/bin/bash
# Save the kernel work as it is in the working tree, in the form apply.sh puts back:
#   files/   every file of the work, whole, at its path in the tree
#
# Whole files rather than a patch: applying it is a copy and cannot half-succeed, and what is in
# the repository is readable as source -- there is nothing to reconstruct.
#
# usage: xnu/export.sh [KERNEL-TREE]     (default: $XNU_SRC, else xnu-src/xnu-8796.121.2)
#
# Run it whenever the kernel tree has changed. Files that are in the tree and differ from Apple's
# are written out; files that were deleted from Apple's tree are removed from files/.
# JUNK: build output and what editors, patch and python leave behind -- never part of the work.
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=${1:-${XNU_SRC:-$HERE/../xnu-src/xnu-8796.121.2}}
BASE=c54f35ca767986246321eb901baf8f5ff7923f6a           # tag xnu-8796.121.2
# .before-<what> are the snapshots taken before each change (hakios.adreno.c.before-gmu-hfi and so
# on); they are not part of the build and must not be copied over Apple's tree or into the repo.
JUNK='^BUILD/|(^|/)__pycache__/|\.(orig|rej|pyc|o|swp)$|\.bak[0-9]*$|\.before(-[A-Za-z0-9._-]+)?$|~$|(^|/)\.DS_Store$'

cd "$SRC"
git cat-file -e "$BASE^{commit}" 2>/dev/null || { echo "export.sh: $SRC is not a clone of xnu at $BASE" >&2; exit 1; }

changed=$(git diff --name-only --diff-filter=MT "$BASE")
added=$(git ls-files --others --exclude-standard)
deleted=$(git diff --name-only --diff-filter=D "$BASE")

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

n=0
while IFS= read -r f; do
	[ -n "$f" ] || continue
	printf '%s\n' "$f" | grep -Eq "$JUNK" && continue
	mkdir -p "$tmp/files/$(dirname "$f")"
	cp -a "$f" "$tmp/files/$f"
	n=$((n + 1))
done <<EOF
$changed
$added
EOF

# a file that was deleted from Apple's tree must stop being copied back over it
d=0
while IFS= read -r f; do
	[ -n "$f" ] || continue
	if [ -e "$HERE/files/$f" ]; then
		rm -f "$HERE/files/$f"
		d=$((d + 1))
	fi
done <<EOF
$deleted
EOF

rm -rf "$HERE/files"
mv "$tmp/files" "$HERE/files" 2>/dev/null || mv "$tmp" "$HERE/files"
trap - EXIT

echo "export.sh: $HERE/files: $n files ($(find "$HERE/files" -type f | wc -l) in all), $d removed"
