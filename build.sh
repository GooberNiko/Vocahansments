#!/bin/sh
# Build the Vocahansments loader and mods (needs MSYS2 mingw32 / i686 gcc).
set -e
cd "$(dirname "$0")"
export PATH=/c/msys64/mingw32/bin:$PATH
CFLAGS="-O2 -Wall -Wno-unused-function -DUNICODE -D_UNICODE -shared -static -s"
mkdir -p dist/vhmods
gcc $CFLAGS -o dist/version.dll src/loader/loader.c src/loader/version.def -luser32
gcc $CFLAGS -o dist/vhmods/SliceTool.dll src/mods/slicetool/slicetool.c -luser32 -lgdi32 -lcomctl32
gcc $CFLAGS -o dist/vhmods/PitchTools.dll src/mods/pitchtools/pitchtools.c -luser32 -lgdi32 -lcomctl32 -lmsimg32
[ -d src/mods/uicolors ] && gcc $CFLAGS -o dist/vhmods/UIColors.dll src/mods/uicolors/*.c -luser32 -lgdi32 -lcomctl32 -lcomdlg32 -lshell32 -lmsimg32
echo built
