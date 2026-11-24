#!/usr/bin/env bash
# Reconstruct the pinned Dusklight checkout, from public sources only.
#
#   tools/fetch_dusklight.sh [DEST]     # default: <repo>/../dusklight
#
# Why this exists
# ---------------
# tp-vita does not vendor Dusklight. CMakeLists.txt requires TP_DEPS to point at a checkout that
# already contains our modifications, and until this script existed those modifications had never
# been committed anywhere: they were 546 lines of uncommitted working-tree changes plus one local
# commit on a `vita` branch that TwilitRealm/dusklight does not publish. A fresh clone of tp-vita
# therefore could not be built by anyone, including CI.
#
# The reconstruction is two steps against upstream's main:
#   0001  a local commit that was never pushed (borealis fatal-message hook)
#   0002  the uncommitted working-tree delta (the port's real modifications)
#
# Both are ordinary patches in patches/dusklight/. Applying them to the pin below reproduces this
# project's tree byte for byte; that was verified by diffing the result against the tree the v0.1
# VPKs were built from.
#
# Licences: Dusklight is CC0 1.0, Aurora and Borealis are MIT. Nothing here redistributes a
# copyrighted game asset or a Sony binary. You still supply your own disc image at run time.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$(cd "$ROOT/.." && pwd)/dusklight}"

# The pin recorded in NOTICE and LICENSES/. It is upstream's main, and it is also the parent of
# the local `vita` commit this project used, so main is the correct base for both patches.
DUSKLIGHT_URL="https://github.com/TwilitRealm/dusklight.git"
DUSKLIGHT_PIN="77705f8bb95ef7b7dca4675b149c4bcb6a48da34"

AURORA_PIN="3227d76c60e1e782ca576610bce61c9e7744d8be"
BOREALIS_PIN="4ac5e7052a8c49a122f8d57f626b5c75c5ca6968"

say() { printf '==> %s\n' "$*" >&2; }

if [ -e "$DEST" ] && [ ! -d "$DEST/.git" ]; then
  say "$DEST exists and is not a git checkout; refusing to overwrite it"
  exit 2
fi

if [ -d "$DEST/.git" ]; then
  say "have $DEST"
else
  say "cloning $DUSKLIGHT_URL"
  git clone --filter=blob:none "$DUSKLIGHT_URL" "$DEST"
fi

cd "$DEST"

# A dirty tree means someone is working in there. Do not apply patches on top of an unknown state.
if [ -n "$(git status --porcelain)" ]; then
  say "$DEST has local changes; this script only builds a clean tree"
  say "either commit/stash them, or point DEST at a fresh directory"
  exit 3
fi

say "checking out $DUSKLIGHT_PIN"
git checkout -q "$DUSKLIGHT_PIN"

# Submodules at the revisions the pin records. --filter=blob:none above keeps the clone small, so a
# full recursive update here is the only large download.
say "updating submodules (aurora, borealis)"
git submodule update --init --recursive --depth 1

# Assert the submodule revisions rather than trusting the checkout, because the build's behaviour
# depends on them: Aurora is the GX command processor we overlay, Borealis the windowing layer.
check_sub() { # path expected
  local got
  got="$(git -C "$1" rev-parse HEAD)"
  if [ "$got" != "$2" ]; then
    say "submodule $1 is $got, expected $2"
    exit 4
  fi
  say "ok $1 $2"
}
check_sub extern/aurora   "$AURORA_PIN"
check_sub extern/borealis "$BOREALIS_PIN"

# Apply our patches, in order, and fail loudly if either no longer applies. A silent skip here
# would produce a tree that builds and behaves differently from the released VPKs, which is the
# exact class of mistake this script exists to prevent.
for p in "$ROOT"/patches/dusklight/[0-9][0-9][0-9][0-9]-*.patch; do
  say "applying $(basename "$p")"
  if ! git apply --check "$p"; then
    say "$(basename "$p") no longer applies to $DUSKLIGHT_PIN"
    say "the pin or the patch series was changed; regenerate the patches, do not skip them"
    exit 5
  fi
  git apply "$p"
done

say "done: $DEST"
say "configure tp-vita with -DTP_DEPS=$DEST (this is CMake's default when the directory is adjacent)"
