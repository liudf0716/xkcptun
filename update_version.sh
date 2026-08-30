#!/bin/bash
# Get current date components
YEAR=$(date +%Y)
MONTH=$(date +%m)

# Major version based on year:
# 2026 -> 1, 2027 -> 2, etc.
BASE_YEAR=2025
CURRENT_YEAR=$YEAR

if [ "$CURRENT_YEAR" -lt "$BASE_YEAR" ]; then
    MAJOR_VERSION=1
else
    MAJOR_VERSION=$((CURRENT_YEAR - BASE_YEAR))
fi

# Get Git commit count for master branch (fallback to HEAD or 0)
COMMIT_COUNT=$(git rev-list --count master 2>/dev/null || git rev-list --count HEAD 2>/dev/null || echo "0")

# Create version string
VERSION="${MAJOR_VERSION}.${MONTH}.${COMMIT_COUNT}"

# Update version.h if different or missing
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET_FILE="${SCRIPT_DIR}/version.h"
TMP_FILE="$(mktemp)"

cat > "$TMP_FILE" << EOF
#ifndef	_VERSION_
#define	_VERSION_

#define	VERSION		"${VERSION}"

#endif
EOF

if [ ! -f "$TARGET_FILE" ] || ! cmp -s "$TMP_FILE" "$TARGET_FILE"; then
    mv "$TMP_FILE" "$TARGET_FILE"
    echo "Updated version.h to ${VERSION}"
else
    rm -f "$TMP_FILE"
fi
