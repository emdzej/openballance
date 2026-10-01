// Disassemble every export/entry point, then run the full auto-analysis.
// Usage: ghidra script run tools/ghidra/Reanalyze.java --project ballance --program X.dll
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
public class Reanalyze extends GhidraScript {
  public void run() throws Exception {
    SymbolTable st = currentProgram.getSymbolTable();
    int n = 0;
    AddressIterator it = st.getExternalEntryPointIterator();
    while (it.hasNext()) {
      Address a = it.next();
      if (currentProgram.getMemory().getBlock(a) == null || !currentProgram.getMemory().getBlock(a).isExecute()) continue;
      new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
      if (getFunctionAt(a) == null) createFunction(a, null);
      n++;
    }
    println("entry points: " + n);
    analyzeAll(currentProgram);
    println("functions: " + currentProgram.getFunctionManager().getFunctionCount());
  }
}
