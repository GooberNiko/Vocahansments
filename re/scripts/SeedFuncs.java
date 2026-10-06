//@category Voca
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
public class SeedFuncs extends GhidraScript {
  public void run() throws Exception {
    Memory m = currentProgram.getMemory();
    long ts=0x401000L, te=0x557000L; int n=0;
    for (long a=0x557000L; a<0x5db000L; a+=4) {
      int v; try { v = m.getInt(toAddr(a)); } catch(Exception e){ continue; }
      long u = v & 0xffffffffL;
      if (u<ts || u>=te) continue;
      Address t = toAddr(u);
      if (getFunctionAt(t)!=null) continue;
      if (getInstructionAt(t)==null) { DisassembleCommand dc=new DisassembleCommand(t,null,true); dc.applyTo(currentProgram,monitor); }
      if (getInstructionAt(t)!=null && getFunctionContaining(t)==null) { if (new CreateFunctionCmd(t).applyTo(currentProgram,monitor)) n++; }
    }
    println("seeded "+n);
  }
}
