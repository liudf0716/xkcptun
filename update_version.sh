#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET_FILE="${SCRIPT_DIR}/version.h"

# Allow explicit override (e.g. from OpenWrt/packaging builds without .git)
if [ -n "$XKCP_VERSION" ]; then
    VERSION="$XKCP_VERSION"
elif [ -d "${SCRIPT_DIR}/.git" ] && command -v git >/dev/null 2>&1; then
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

    # Get Git commit count and short hash for HEAD
    COMMIT_COUNT=$(git -C "$SCRIPT_DIR" rev-list --count HEAD 2>/dev/null || echo "0")
    GIT_HASH=$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo "unknown")
    DIRTY=""
    if [ -n "$(git -C "$SCRIPT_DIR" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
        DIRTY="-dirty"
    fi

    VERSION="${MAJOR_VERSION}.${MONTH}.${COMMIT_COUNT}-${GIT_HASH}${DIRTY}"
elif [ -f "$TARGET_FILE" ]; then
    # No .git directory and existing version.h present: keep as-is
    exit 0
else
    VERSION="1.0.0-unknown"
fi

# Update version.h only if content differs
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
