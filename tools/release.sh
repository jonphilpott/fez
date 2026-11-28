#!/bin/bash
# Makes a FEZ release: bumps VERSION, builds every download, then (after you say yes) commits, tags, pushes and publishes a GitHub
# release with the files attached.
#
#   tools/release.sh VERSION [options]        e.g.  tools/release.sh 0.4.15
#
#   --notes FILE   the release notes (Markdown). Without it, a draft is made from the commit messages since the last release
#                  and opened in $EDITOR (or `vi`) for you to rewrite.
#   --skip WHAT    leave one download out: mac, win, brick or usb (repeat for more). A release with fewer files than the last is
#                  allowed but the script says so.
#   --yes          do not ask before publishing (for a script; nothing else is skipped)
#   --dry-run      check everything and say what would happen, build and publish nothing
#
# What each download needs on this machine:  mac: macOS (hdiutil, codesign)   win: MinGW (make win-sdl fetches SDL)
#   brick: Docker (brick/build.sh)   usb: Docker with 8 GB (os/build.sh: an hour the first time, minutes after)
# The order is: checks, version bump, tests, manual, every build, then ONE question, then commit/tag/push/publish. If a build
# fails, the VERSION and manual edits are put back and nothing has left this machine.
set -e -o pipefail          # a failing step in a pipe (make test | tail) must stop the release
cd "$(dirname "$0")/.."

# ---- 0. arguments
VERSION=; NOTES=; SKIP=" "; YES=; DRY=
while [ $# -gt 0 ]; do
  case $1 in
    --notes) NOTES=$2; shift ;;
    --skip)  SKIP="$SKIP$2 "; shift ;;
    --yes)   YES=1 ;;
    --dry-run) DRY=1 ;;
    -*) echo "unknown option $1"; exit 2 ;;
    *)  VERSION=$1 ;;
  esac
  shift
done
echo "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo "usage: tools/release.sh X.Y.Z [--notes FILE] [--skip mac|win|brick|usb] [--yes] [--dry-run]"; exit 2; }
TAG=v$VERSION
want() { case "$SKIP" in *" $1 "*) return 1 ;; esac; }       # want mac: true unless --skip mac was given
say() { echo; echo "==> $*"; }

# ---- 1. checks: nothing is changed until they pass
say "Checking"
[ "$(git rev-parse --abbrev-ref HEAD)" = main ] || { echo "not on main"; exit 1; }
[ -z "$(git status --porcelain)" ] || { echo "uncommitted changes: commit or stash them first"; git status --short; exit 1; }
git fetch -q --tags
[ "$(git rev-list --count HEAD..origin/main)" = 0 ] || { echo "origin/main has commits you do not have: pull first"; exit 1; }
git rev-parse -q --verify "refs/tags/$TAG" > /dev/null && { echo "tag $TAG already exists"; exit 1; }
command -v gh > /dev/null || { echo "needs the GitHub CLI (gh), logged in"; exit 1; }
gh auth status > /dev/null 2>&1 || { echo "gh is not logged in: run  gh auth login"; exit 1; }
PREV=$(git describe --tags --abbrev=0 2>/dev/null || true)
echo "this release: $TAG   last release: ${PREV:-none}   commits since: $(git rev-list --count ${PREV:+$PREV..}HEAD)"
[ -z "$PREV" ] || [ "$(git rev-list --count $PREV..HEAD)" != 0 ] || { echo "nothing new since $PREV"; exit 1; }
for w in mac win brick usb; do want $w || echo "skipping: $w"; done

# ---- 2. the notes (asked for first so a long build does not end with a question)
NOTES_OUT=dist/notes-$TAG.md; mkdir -p dist
if [ -n "$NOTES" ]; then cp "$NOTES" "$NOTES_OUT"
else
  { echo "<!-- Rewrite this: what is new for someone using FEZ, not a list of commits. Delete this line. -->"
    echo; git log --format='- %s' ${PREV:+$PREV..}HEAD
    echo; echo "The Mac app and Windows exe are unsigned: see the README for the first-launch steps."; } > "$NOTES_OUT"
  [ -n "$DRY" ] || ${EDITOR:-vi} "$NOTES_OUT"
fi
if want usb; then
  # the USB image carries Linux and other free software: say where its source is (Buildroot's legal-info is still to be done)
  grep -q "USB" "$NOTES_OUT" || echo "note: the notes do not mention the USB image (FEZ-usb-$TAG.zip)"
fi
[ -z "$DRY" ] || { say "Dry run: checks passed. Would build $(for w in mac win brick usb; do want $w && printf '%s ' $w; done), then publish $TAG. Notes draft: $NOTES_OUT"; exit 0; }

# ---- 3. bump, test, build. Any failure puts the edited files back.
restore() { git checkout -q -- VERSION docs/manual.html 2>/dev/null || true; echo "failed: VERSION and the manual were put back"; }
trap restore ERR
say "Version $VERSION"
echo "$VERSION" > VERSION
say "Tests";  make test 2>&1 | tail -1
say "Manual"; make manual 2>&1 | tail -1
rm -f dist/FEZ-*-v$VERSION.*
if want mac;   then say "Mac app";      make mac-dmg 2>&1 | tail -1; fi
if want win;   then say "Windows exe";  make win-zip 2>&1 | tail -1; fi
if want brick; then say "Brick, Knulli, TrimUI"; brick/build.sh > dist/brick-build.log 2>&1; make brick-zip 2>&1 | grep made; fi
if want usb; then
  say "USB image"
  os/build.sh > dist/usb-build.log 2>&1
  os/make-image.sh > /dev/null
  cp os/out/fez.img dist/FEZ-usb-v$VERSION.img
  (cd dist && rm -f FEZ-usb-v$VERSION.zip && zip -q FEZ-usb-v$VERSION.zip FEZ-usb-v$VERSION.img) && rm dist/FEZ-usb-v$VERSION.img
  echo "made dist/FEZ-usb-v$VERSION.zip"
fi
trap - ERR
FILES=$(ls dist/FEZ-*-v$VERSION.* 2>/dev/null)
[ -n "$FILES" ] || { restore; echo "nothing was built"; exit 1; }

# ---- 4. the one question
say "Ready to publish $TAG"
echo "files:"; ls -l $FILES | awk '{printf "  %-44s %s bytes\n", $9, $5}'
echo "notes: $NOTES_OUT"; head -12 "$NOTES_OUT" | sed 's/^/  | /'
echo "will: commit VERSION and the manual, tag $TAG, push main and the tag, create the GitHub release"
if [ -z "$YES" ]; then
  printf "Publish? [y/N] "; read -r ans
  [ "$ans" = y ] || { restore; echo "not published; the built files are still in dist/"; exit 1; }
fi

# ---- 5. publish
git add VERSION docs/manual.html
git commit -q -m "Release $VERSION"
git tag "$TAG"
git push -q origin main "$TAG"
gh release create "$TAG" --title "FEZ $VERSION" --notes-file "$NOTES_OUT" $FILES
say "Published $TAG"
