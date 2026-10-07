#!/bin/bash
# usage: decomp.sh <outfile> <items...>
T=$HOME/warband_fast_load/tools
export JAVA_HOME="$T/jdk-21.0.12.1+1/Contents/Home" DECOMP_OUT="$1"; shift
: > "$DECOMP_OUT"
"$T/ghidra_12.1.4_PUBLIC/support/analyzeHeadless" "$T/proj2" warband -process mb.bin -noanalysis -readOnly -scriptPath "$T/scripts" -postScript Decomp.java "$@" > "$DECOMP_OUT.log" 2>&1
grep -E "ERROR|Exception" "$DECOMP_OUT.log" | head -3
grep -E "^(==|##)" "$DECOMP_OUT"
