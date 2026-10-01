// Decompile every function of the current program into one C file for grepping.
// Usage: ghidra script run tools/ghidra/DumpAllNamed.java --project ballance --program <prog> -- /abs/path/re/all_named.c
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class DumpAllNamed extends GhidraScript {
  public void run() throws Exception {
    String[] args = getScriptArgs();
    if (args.length < 1) { printerr("usage: DumpAllNamed.java <output.c>"); return; }
    DecompInterface d = new DecompInterface();
    d.openProgram(currentProgram);
    PrintWriter w = new PrintWriter(new FileWriter(args[0]));
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      DecompileResults r = d.decompileFunction(f, 60, monitor);
      w.println("// ==== " + f.getSymbol().getName(true) + " @ " + f.getEntryPoint());
      if (r.decompileCompleted()) w.println(r.getDecompiledFunction().getC());
      else w.println("// FAILED");
    }
    w.close();
  }
}
