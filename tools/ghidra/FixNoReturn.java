// Clear "no return" on external functions and thunks (Ghidra sometimes marks imported CK2.dll methods
// as non-returning, which truncates every caller), then re-run analysis.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
public class FixNoReturn extends GhidraScript {
  public void run() throws Exception {
    int n = 0;
    FunctionManager fm = currentProgram.getFunctionManager();
    for (Function f : fm.getExternalFunctions()) if (f.hasNoReturn()) { f.setNoReturn(false); n++; }
    for (Function f : fm.getFunctions(true)) {
      if (f.hasNoReturn() && (f.isThunk() || f.getName().contains("::"))) { f.setNoReturn(false); n++; }
    }
    analyzeAll(currentProgram);
    println("cleared " + n);
  }
}
