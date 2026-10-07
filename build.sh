#!/bin/bash
# Build fastswap.dylib without installing it.
here="$(cd "$(dirname "$0")" && pwd)"
clang -arch x86_64 -dynamiclib -O2 -Wall -Wno-deprecated-declarations -o "$here/fastswap.dylib" "$here"/src/{fastswap,patch,texhook,loadstep,fopenhook,markers,shaderdefer,prefetch}.c \
  -framework OpenGL -Wl,-undefined,dynamic_lookup
