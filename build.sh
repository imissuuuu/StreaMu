#!/bin/bash
# Build script for Claude Code Bash tool (works around DEVKITPRO/TEMP issues)
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitARM/bin:$PATH
export TEMP=/tmp
export TMP=/tmp
export TMPDIR=/tmp

cd /c/dev/3ds-music-player

if [ "$1" = "clean" ]; then
    /c/devkitPro/msys2/usr/bin/make clean 2>&1
fi

/c/devkitPro/msys2/usr/bin/make 2>&1
