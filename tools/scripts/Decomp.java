// Decompile functions for reverse engineering the Warband loader.
// Args: items, each either
//   0x<addr>       decompile the function containing this address
//                  (offsets from profiles: add 0x100000000)
//   str:<text>     find defined strings containing <text>, decompile all referencing functions
//   callers:0x<a>  list the callers of the function containing <a>
// Output goes to the file named by env DECOMP_OUT (appended).
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class Decomp extends GhidraScript {
    DecompInterface di;
    PrintWriter out;
    Set<Function> done = new HashSet<>();

    void decomp(Function f, String why) {
        if (f == null || !done.add(f)) return;
        DecompileResults r = di.decompileFunction(f, 120, monitor);
        out.println("==== " + f.getName() + " @ " + f.getEntryPoint() + "  (" + why + ")");
        out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "decompile failed: " + r.getErrorMessage());
    }

    public void run() throws Exception {
        out = new PrintWriter(new FileWriter(System.getenv("DECOMP_OUT"), true));
        di = new DecompInterface();
        di.openProgram(currentProgram);
        FunctionManager fm = currentProgram.getFunctionManager();
        for (String a : getScriptArgs()) {
            if (a.startsWith("str:")) {
                String needle = a.substring(4);
                for (Data d : currentProgram.getListing().getDefinedData(true)) {
                    Object v = d.getValue();
                    if (!(v instanceof String) || !((String) v).contains(needle)) continue;
                    out.println("## string \"" + v + "\" @ " + d.getAddress());
                    for (Reference ref : getReferencesTo(d.getAddress())) {
                        Function f = fm.getFunctionContaining(ref.getFromAddress());
                        out.println("##   ref from " + ref.getFromAddress() + " in " + (f == null ? "?" : f.getName()));
                        decomp(f, "refs " + needle);
                    }
                }
            } else if (a.startsWith("refs:")) {
                Address ad = toAddr(Long.decode(a.substring(5)));
                out.println("## refs to " + ad);
                for (Reference ref : getReferencesTo(ad)) {
                    Function c = fm.getFunctionContaining(ref.getFromAddress());
                    out.println("##   " + ref.getReferenceType() + " " + ref.getFromAddress() + " in " + (c == null ? "?" : c.getName()));
                }
                for (Reference ref : getReferencesTo(ad))
                    if (ref.getReferenceType().isRead()) decomp(fm.getFunctionContaining(ref.getFromAddress()), "reads " + ad);
            } else if (a.startsWith("callers:")) {
                Function f = fm.getFunctionContaining(toAddr(Long.decode(a.substring(8))));
                out.println("## callers of " + f.getName());
                for (Reference ref : getReferencesTo(f.getEntryPoint())) {
                    Function c = fm.getFunctionContaining(ref.getFromAddress());
                    out.println("##   " + ref.getFromAddress() + " in " + (c == null ? "?" : c.getName() + " @ " + c.getEntryPoint()));
                }
            } else {
                Address ad = toAddr(Long.decode(a));
                Function f = fm.getFunctionContaining(ad);
                if (f == null) out.println("## no function at " + ad);
                decomp(f, "contains " + ad);
            }
        }
        out.close();
    }
}
