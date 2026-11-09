#!/bin/bash
# MSR-nx: runtime/gpu/vsgen.c must write exactly what tools/shaders/pica2glsl.py writes.
# usage: vsgen_test.sh <vs dump>   (the dump is game-derived: keep it and the output out of the repo)
set -e
R=$(dirname "$(realpath "$0")")/../..
T=$(mktemp -d)
mkdir -p $T/py $T/c
python3 $R/tools/shaders/pica2glsl.py "$1" $T/py
gcc -O1 -Wall -I$R/runtime/gpu -o $T/vsgen_test $R/tools/tests/vsgen_test.c $R/runtime/gpu/vsgen.c
$T/vsgen_test "$1" $T/c
diff -r -x check.cpp -x keys.txt $T/py $T/c && echo "vsgen: identical to pica2glsl.py"
rm -rf $T
