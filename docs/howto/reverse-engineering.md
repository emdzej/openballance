# Reverse-engineering workflow

Ballance is a Virtools 2.1 game: `Player.exe` hosts the Virtools runtime (`CK2.dll`, `CK2_3D.dll`,
`VxMath.dll`) and loads Building Block DLLs (`BuildingBlocks/*.dll`, including the Terratools `TT_*.dll`
and `physics_RT.dll`).

1. **Graphs first.** `tools/` has Python readers for the Virtools file format (`ck.py`, `nmo.py`) that dump
   every behaviour graph with its parameters and links; most questions are answered there.
2. **Which Building Blocks are used.** `tools/bbindex.py` / `bbmap.py` list every BB prototype the game
   uses and map its GUID to the DLL and function that implements it.
3. **Decompile and disassemble.** The DLLs are analysed with Ghidra headless (scripts in `tools/ghidra/`)
   and, where the decompiler is unreliable (x87 float code, merged stack slots), read from the
   disassembly with capstone.
4. **Specify, then port.** Larger subsystems get a written specification first (the
   [internals](/internals/) pages) and are ported from it, citing the original addresses in the code.
   Decompiler output is never pasted into the source.
5. **Verify** with headless runs: `run_test` traces, probes and screenshots.

The original binaries and data are never committed.
