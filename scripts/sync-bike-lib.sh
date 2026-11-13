#!/usr/bin/env bash
# Refresh the vendored bike library (libs/bike-core, libs/bike) from a soloton checkout.
#
#   scripts/sync-bike-lib.sh                       ../soloton at origin/main
#   scripts/sync-bike-lib.sh ~/dev/soloton v0.2.0  another checkout or ref
#
# Replaces everything upstream owns (sources, tests, LICENSE, README) and keeps this repo's own
# build.gradle.kts files, then records the commit in libs/UPSTREAM. Review with git diff.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
soloton="${1:-$here/../soloton}"
ref="${2:-origin/main}"

git -C "$soloton" fetch --quiet origin || echo "warning: could not fetch $soloton; using what it has" >&2
commit="$(git -C "$soloton" rev-parse "$ref^{commit}")"

for module in bike-core bike; do
    dest="$here/libs/$module"
    keep="$(mktemp)"
    cp "$dest/build.gradle.kts" "$keep"
    rm -rf "$dest"
    mkdir -p "$here/libs"
    git -C "$soloton" archive "$commit" "libs/$module" | tar -x -C "$here"
    cp "$keep" "$dest/build.gradle.kts"
    rm "$keep"
done

sed -i "s/^commit=.*/commit=$commit/" "$here/libs/UPSTREAM"
echo "libs/bike-core and libs/bike now at soloton $commit"
