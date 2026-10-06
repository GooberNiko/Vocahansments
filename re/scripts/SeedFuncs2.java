//@category Voca
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import java.io.*;
public class SeedFuncs2 extends GhidraScript {
  public void run() throws Exception {
    BufferedReader br=new BufferedReader(new FileReader("C:/Users/slimm/Desktop/Claude/Toolz/Vocahansments/re/seeds.txt"));
    String l; int n=0;
    while((l=br.readLine())!=null){
      Address t=toAddr(Long.parseLong(l.trim(),16));
      if (getInstructionAt(t)==null && getInstructionContaining(t)!=null) continue;
      if (getInstructionAt(t)==null){ new DisassembleCommand(t,null,true).applyTo(currentProgram,monitor);} 
      if (getInstructionAt(t)!=null && getFunctionAt(t)==null){ if(new CreateFunctionCmd(t).applyTo(currentProgram,monitor)) n++; }
    }
    println("seeded2 "+n);
  }
}
