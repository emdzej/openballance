// Create functions at the hex addresses listed in a file (one per line), e.g. Building Block creation
// and execute functions only referenced as data. Usage:
//   ghidra script run tools/ghidra/MakeFunctions.java --project ballance --program X.dll -- /abs/addrs.txt
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.Address;
import java.nio.file.*;
public class MakeFunctions extends GhidraScript {
  public void run() throws Exception {
    int made = 0;
    for (String line : Files.readAllLines(Paths.get(getScriptArgs()[0]))) {
      line = line.trim();
      if (line.isEmpty()) continue;
      Address a = toAddr(Long.parseLong(line, 16));
      if (getFunctionAt(a) != null) continue;
      new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
      if (createFunction(a, null) != null) made++;
    }
    analyzeChanges(currentProgram);
    println("created " + made);
  }
}
