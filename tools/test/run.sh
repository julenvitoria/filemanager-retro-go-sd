#!/bin/sh
# Builds tools/test/test_tools.c on the PC and checks hashing and extracting against Python.
set -e
cd "$(dirname "$0")/../.."
W=build/test; rm -rf $W; mkdir -p $W
gcc -O1 -g -Wall -Isrc -DMINIZ_NO_MALLOC -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -o $W/t tools/test/test_tools.c src/hash.c src/extract.c src/unzip.c src/tinfl_tools.c 
python3 -I tools/test/check.py $W
