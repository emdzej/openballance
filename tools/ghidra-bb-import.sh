#!/bin/bash
# Fresh import of a Building Block DLL, full analysis, BB function discovery, dump to re/<dll>.c.
# Usage: tools/ghidra-bb-import.sh <path to dll> [...]
JH=${JAVA_HOME_GHIDRA:-/opt/homebrew/Cellar/openjdk@21/21.0.12.1/libexec/openjdk.jdk/Contents/Home}
cd "$(dirname "$0")/.."
P="--project ballance --java-home $JH"
for dll in "$@"; do
  n=$(basename "$dll")
  if [ -n "$REUSE" ]; then prog=$REUSE; else
  before=$(ghidra program list $P 2>/dev/null | grep -oE "$n(\.[0-9]+)?" | sort -u)
  ghidra import "$PWD/$dll" $P >/dev/null 2>&1
  after=$(ghidra program list $P 2>/dev/null | grep -oE "$n(\.[0-9]+)?" | sort -u)
  prog=$(comm -13 <(echo "$before") <(echo "$after") | tail -1); prog=${prog:-$n}
  fi
  ghidra script run $PWD/tools/ghidra/Reanalyze.java $P --program "$prog" >/dev/null 2>&1
  for i in 1 2 3 4; do
    ghidra script run $PWD/tools/ghidra/DumpAllNamed.java $P --program "$prog" -- "$PWD/re/$n.c" >/dev/null 2>&1
    python3 tools/bbindex.py re/$n.c --labels > /tmp/bbaddrs_$n.txt
    [ -s /tmp/bbaddrs_$n.txt ] || break
    ghidra script run $PWD/tools/ghidra/MakeFunctions.java $P --program "$prog" -- /tmp/bbaddrs_$n.txt >/dev/null 2>&1
  done
  echo "$n (program $prog): $(python3 tools/bbindex.py re/$n.c | wc -l | tr -d ' ') BBs, $(python3 tools/bbindex.py re/$n.c | grep -c 'fn=None') without function"
done
