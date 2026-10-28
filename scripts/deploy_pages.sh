#!/usr/bin/env bash
# Publishes web/index.html to the gh-pages branch, which GitHub Pages is configured to serve
# directly (Settings -> Pages -> Deploy from a branch -> gh-pages / root) - no GitHub Actions
# workflow involved. Run from a checkout of main with no uncommitted changes to web/index.html.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKTREE_DIR="$(mktemp -d)"
trap 'git -C "$REPO_ROOT" worktree remove --force "$WORKTREE_DIR" >/dev/null 2>&1 || true' EXIT

cd "$REPO_ROOT"

if git show-ref --verify --quiet refs/heads/gh-pages || git show-ref --verify --quiet refs/remotes/origin/gh-pages; then
    git worktree add "$WORKTREE_DIR" gh-pages
else
    git worktree add --orphan -b gh-pages "$WORKTREE_DIR"
fi

cp web/index.html "$WORKTREE_DIR/index.html"
touch "$WORKTREE_DIR/.nojekyll"

cd "$WORKTREE_DIR"
git add -A
if git diff --cached --quiet; then
    echo "gh-pages already up to date with web/index.html"
    exit 0
fi

SOURCE_COMMIT="$(git -C "$REPO_ROOT" rev-parse --short HEAD)"
git commit -m "Deploy web/index.html from main@${SOURCE_COMMIT}"
git push origin gh-pages

echo "Deployed. Enable it once under Settings -> Pages -> Deploy from a branch -> gh-pages / (root)."
