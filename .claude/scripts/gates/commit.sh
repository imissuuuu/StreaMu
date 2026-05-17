#!/usr/bin/env bash
set -euo pipefail

GIT_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$GIT_ROOT" ]]; then
  echo "[commit] no git repository found"
  exit 1
fi

cd "$GIT_ROOT"

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if [[ "$BRANCH" != "dev" ]]; then
  echo "[commit] refusing to auto-commit outside dev branch (current: $BRANCH)"
  exit 1
fi

if [[ -z "$(git status --porcelain)" ]]; then
  echo "[commit] working tree clean"
  exit 0
fi

MESSAGE="${WFAT_COMMIT_MESSAGE:-Update workflow gates}"

echo "[commit] staging changes in $GIT_ROOT"
git add -A

if git diff --cached --quiet; then
  echo "[commit] no staged changes after git add"
else
  echo "[commit] committing: $MESSAGE"
  git commit -m "$MESSAGE"
fi

if [[ -n "$(git status --porcelain)" ]]; then
  echo "[commit] uncommitted changes remain after auto-commit:"
  git status --short
  exit 1
fi

echo "[commit] working tree clean"
