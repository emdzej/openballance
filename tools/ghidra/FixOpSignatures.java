// Give CKParameterManager::RegisterOperationType / RegisterOperationFunction their real signatures so the
// decompiler shows the GUID and function arguments (ParameterOperations.dll registers ~400 operations).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
public class FixOpSignatures extends GhidraScript {
  public void run() throws Exception {
    DataTypeManager dtm = currentProgram.getDataTypeManager();
    StructureDataType g = new StructureDataType("CKGUID", 0);
    g.add(DWordDataType.dataType, "d1", null);
    g.add(DWordDataType.dataType, "d2", null);
    DataType guid = dtm.addDataType(g, DataTypeConflictHandler.REPLACE_HANDLER);
    DataType pg = new PointerDataType(guid);
    DataType pv = new PointerDataType(VoidDataType.dataType);
    DataType pc = new PointerDataType(CharDataType.dataType);
    int n = 0;
    for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
      String name = f.getName();
      Parameter[] ps = null;
      if (name.equals("RegisterOperationFunction")) {
        ps = new Parameter[] {
          new ParameterImpl("op", pg, currentProgram), new ParameterImpl("res", pg, currentProgram),
          new ParameterImpl("p1", pg, currentProgram), new ParameterImpl("p2", pg, currentProgram),
          new ParameterImpl("fn", pv, currentProgram)};
      } else if (name.equals("RegisterOperationType")) {
        ps = new Parameter[] { new ParameterImpl("op_d1", DWordDataType.dataType, currentProgram), new ParameterImpl("op_d2", DWordDataType.dataType, currentProgram), new ParameterImpl("name", pc, currentProgram)};
      }
      if (ps == null) continue;
      f.setCallingConvention("__thiscall");
      f.replaceParameters(Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS, true, SourceType.USER_DEFINED, ps);
      n++;
    }
    println("fixed " + n);
  }
}
