#!/usr/bin/env bash
set -euo pipefail

cd "C:/dev/3ds-music-player"

if [ ! -f review_result.json ]; then
  echo "[review-gate] review_result.json not found" >&2
  exit 1
fi

STATUS=$(jq -r '.status // ""' review_result.json)

if [ "$STATUS" != "PASS" ]; then
  echo "[review-gate] review_result.json status is '$STATUS'" >&2
  exit 1
fi

echo "[review-gate] PASS" >&2
exit 0
