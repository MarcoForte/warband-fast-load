#!/bin/bash
# Install / uninstall the Warband fast-loading patch.  Usage: ./install.sh [--uninstall]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
M="$HOME/Library/Application Support/Steam/steamapps/common/MountBlade Warband/Mount and Blade.app/Contents/MacOS"
if [[ "${1:-}" == "--uninstall" ]]; then
  [[ -f "$M/Mount and Blade.real" ]] || { echo "not installed"; exit 0; }
  mv -f "$M/Mount and Blade.real" "$M/Mount and Blade"
  rm -f "$M/fastswap.dylib" "$M/Mount and Blade.patched"
  echo "uninstalled"; exit 0
fi
"$here/build.sh"
clang -arch x86_64 -O2 -o "$here/wrapper" "$here/src/wrapper.c"
if [[ ! -f "$M/Mount and Blade.real" ]]; then
  mv "$M/Mount and Blade" "$M/Mount and Blade.real"
fi
python3 "$here/make_patched.py" "$M/Mount and Blade.real" "$M/Mount and Blade.patched"
chmod +x "$M/Mount and Blade.patched"
cp "$here/wrapper" "$M/Mount and Blade"
cp "$here/fastswap.dylib" "$M/fastswap.dylib"
echo "installed (original binary kept as 'Mount and Blade.real')"

# Uncap the frame limiter (it throttles the loading loop; macOS still syncs
# presents to the display).  The game creates this file on its first run.
C="$HOME/Library/Application Support/MBWarband/rgl_config.txt"
if [[ -f "$C" ]]; then
  sed -i '' -e 's/^max_framerate = .*/max_framerate = 10000/' -e 's/^force_vsync = .*/force_vsync = 0/' "$C"
  echo "rgl_config.txt: max_framerate = 10000, force_vsync = 0"
else
  echo "note: run the game once, then rerun this script to apply the rgl_config.txt settings"
fi
