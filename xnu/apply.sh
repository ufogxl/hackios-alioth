#!/bin/bash
# Get Apple's XNU sources (the release that iOS 16.5 was built from) and lay the HakiOS work on
# top of them.
#
# files/ holds EVERY file of the work as a whole file, at its path in the tree: the ones we added
# and the ones we changed. So this script only copies -- there is no patch, and applying it cannot
# half-fail. What lands on disk is exactly what was built and booted.
#
# usage: xnu/apply.sh [DIR]      (default: $XNU_SRC, else xnu-src/xnu-8796.121.2 next to this file)
# env:   XNU_URL   where to fetch from (default: Apple's repository on GitHub; a local clone that
#                  has the tag will do: XNU_URL=file:///path/to/xnu)
#        XNU_JOBS  passed to the build, if it is asked to run one (see --build)
#
# A directory that already exists is left alone, so this is safe to re-run.
set -e
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DIR=${1:-${XNU_SRC:-$HERE/../xnu-src/xnu-8796.121.2}}
URL=${XNU_URL:-https://github.com/apple-oss-distributions/xnu.git}
TAG=xnu-8796.121.2
BASE=c54f35ca767986246321eb901baf8f5ff7923f6a

[ -d "$HERE/files" ] || { echo "apply.sh: no files/ in $HERE" >&2; exit 1; }
mkdir -p "$(dirname "$DIR")"
DIR=$(cd "$(dirname "$DIR")" && pwd)/$(basename "$DIR")

if [ -e "$DIR" ]; then
	echo "apply.sh: $DIR already exists; not touching it" >&2
	exit 1
fi
git init -q "$DIR"
trap 'echo "apply.sh: failed; removing the incomplete $DIR" >&2; rm -rf "$DIR"' ERR
cd "$DIR"
git remote add origin "$URL"
git fetch -q --depth 1 origin "refs/tags/$TAG:refs/tags/$TAG"      # that one release, without the history
git checkout -q -b hakios "$TAG"
[ "$(git rev-parse HEAD)" = "$BASE" ] || { echo "apply.sh: $TAG is $(git rev-parse HEAD), expected $BASE" >&2; false; }

# the work, whole files over Apple's
count=$(find "$HERE/files" -type f | wc -l)
cp -a "$HERE/files/." .

echo "apply.sh: $DIR: $TAG ($BASE) + $count files from $HERE/files"
