#!/bin/sh
# Build the portable bundle from the current commit and publish it as the
# latest release of the public download repository.
#
#   tools/publish-release.sh [BUNDLE.tar.gz]
#
# Without an argument it builds with tools/build-in-docker.sh rocky9 first.
# Each release carries the bundle under its versioned name and under a fixed
# name, so this link always downloads the newest build:
#
#   https://github.com/REPO/releases/latest/download/graywatch-portable-linux-x86_64.tar.gz
#
# GRAYWATCH_RELEASE_REPO selects the repository (default
# mysysadmin/graywatch-releases). Needs an authenticated gh (gh auth status).
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repo=${GRAYWATCH_RELEASE_REPO:-mysysadmin/graywatch-releases}

command -v gh >/dev/null 2>&1 || { echo "gh is not installed" >&2; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "gh is not logged in: gh auth login" >&2; exit 1; }

if [ "$#" -ge 1 ]; then
    bundle=$1
else
    # A release must be reproducible from a pushed commit.
    if [ -n "$(git -C "$root" status --porcelain --untracked-files=no)" ]; then
        echo "uncommitted changes; commit them before publishing" >&2
        exit 1
    fi
    built=$("$root/tools/build-in-docker.sh" rocky9 | tail -n 1)
    bundle="$root/dist/rocky9/$(basename "$built")"
fi
[ -f "$bundle" ] || { echo "no such bundle: $bundle" >&2; exit 1; }

name=$(basename "$bundle" .tar.gz)
version=${name#graywatch-portable-}
version=${version%-linux-*}
architecture=${name##*-linux-}
case "$version" in
    ''|unknown|*-dirty) echo "refusing to publish version '$version'" >&2; exit 1 ;;
esac
if gh release view "$version" --repo "$repo" >/dev/null 2>&1; then
    echo "release $version already exists in $repo" >&2
    exit 1
fi

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
fixed="graywatch-portable-linux-$architecture.tar.gz"
cp "$bundle" "$stage/$name.tar.gz"
cp "$bundle" "$stage/$fixed"
(cd "$stage" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256" &&
    sha256sum "$fixed" > "$fixed.sha256")

commit=$(git -C "$root" rev-parse "$version^{commit}" 2>/dev/null || echo "$version")
cat > "$stage/notes.md" <<EOF
Built from commit \`$commit\` on $(date -u +%Y-%m-%d).

Download, check and run on the affected node while the problem is happening:

\`\`\`sh
curl -fLO https://github.com/$repo/releases/latest/download/$fixed
curl -fLO https://github.com/$repo/releases/latest/download/$fixed.sha256
sha256sum -c $fixed.sha256
tar -xzf $fixed
cd graywatch-portable-*/
sudo ./collect-graywatch-support.sh
\`\`\`
EOF

gh release create "$version" --repo "$repo" --latest \
    --title "Graywatch $version" --notes-file "$stage/notes.md" \
    "$stage/$name.tar.gz" "$stage/$name.tar.gz.sha256" \
    "$stage/$fixed" "$stage/$fixed.sha256"
echo "https://github.com/$repo/releases/latest/download/$fixed"
