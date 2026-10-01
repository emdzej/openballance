#!/bin/bash
# Make Ghidra functions out of Building Block creation/execute/callback labels and re-dump, until none
# are left. Usage: tools/bbpass.sh X.dll [...]
JH=${JAVA_HOME_GHIDRA:-/opt/homebrew/Cellar/openjdk@21/21.0.12.1/libexec/openjdk.jdk/Contents/Home}
cd "$(dirname "$0")/.."
for p in "$@"; do
  for i in 1 2 3; do
    python3 tools/bbindex.py re/$p.c --labels > /tmp/bbaddrs_$p.txt
    [ -s /tmp/bbaddrs_$p.txt ] || break
    ghidra script run $PWD/tools/ghidra/MakeFunctions.java --project ballance --program $p --java-home $JH -- /tmp/bbaddrs_$p.txt >/dev/null 2>&1
    ghidra script run $PWD/tools/ghidra/DumpAllNamed.java --project ballance --program $p --java-home $JH -- "$PWD/re/$p.c" >/dev/null 2>&1
  done
  echo "$p: $(python3 tools/bbindex.py re/$p.c | wc -l) BBs, $(python3 tools/bbindex.py re/$p.c | grep -c 'fn=None') without function"
done
