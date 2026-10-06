//@category Voca
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import java.io.*;
public class LabelIat extends GhidraScript {
  public void run() throws Exception {
    BufferedReader br = new BufferedReader(new FileReader("C:/Users/slimm/Desktop/Claude/Toolz/Vocahansments/re/iat.txt"));
    String l; int n=0;
    while ((l = br.readLine()) != null) {
      String[] p = l.trim().split(" ");
      Address a = toAddr(Long.parseLong(p[0],16));
      String nm = p[1].replace("!","::").replace("?","_").replace("@","_").replace("$","_");
      if (nm.length()>180) nm = nm.substring(0,180);
      try { clearListing(a, a.add(3)); createData(a, new PointerDataType()); } catch(Exception e){}
      try { createLabel(a, "IAT_"+nm, true, SourceType.USER_DEFINED); n++; } catch(Exception e){ println("fail "+nm); }
    }
    println("labeled "+n);
  }
}
