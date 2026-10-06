//@category ManeMane
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class DumpAll extends GhidraScript {
  public void run() throws Exception {
    String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "C:/Users/slimm/ManeMane-re/decomp.c";
    DecompInterface di = new DecompInterface();
    DecompileOptions opt = new DecompileOptions();
    di.setOptions(opt);
    di.openProgram(currentProgram);
    PrintWriter pw = new PrintWriter(new OutputStreamWriter(new FileOutputStream(out), "UTF-8"));
    FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
    int n = 0;
    while (it.hasNext() && !monitor.isCancelled()) {
      Function f = it.next();
      if (f.isThunk() || f.isExternal()) continue;
      DecompileResults r = di.decompileFunction(f, 60, monitor);
      pw.println("// ==== " + f.getName(true) + " @ " + f.getEntryPoint() + " size=" + f.getBody().getNumAddresses());
      if (r != null && r.decompileCompleted()) pw.println(r.getDecompiledFunction().getC());
      else pw.println("// decompile failed");
      n++;
    }
    pw.close();
    // symbols dump
    PrintWriter sw = new PrintWriter(new OutputStreamWriter(new FileOutputStream(out + ".symbols.txt"), "UTF-8"));
    SymbolIterator si = currentProgram.getSymbolTable().getAllSymbols(true);
    while (si.hasNext()) { Symbol s = si.next(); if (s.getSource() != SourceType.DEFAULT) sw.println(s.getAddress() + " " + s.getSymbolType() + " " + s.getName(true)); }
    sw.close();
    println("dumped " + n);
  }
}
