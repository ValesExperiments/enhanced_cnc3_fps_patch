// Find functions that reference given strings. Strings come from the
// ASTA_STRINGS environment variable, separated by '|'.
//
//@category CNC3

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.util.Set;
import java.util.TreeSet;

public class FindStrRefs extends GhidraScript {

    @Override
    public void run() throws Exception {
        String spec = System.getenv("ASTA_STRINGS");
        if (spec == null || spec.isEmpty()) {
            println("set ASTA_STRINGS");
            return;
        }
        Memory mem = currentProgram.getMemory();
        ReferenceManager refMgr = currentProgram.getReferenceManager();
        FunctionManager fnMgr = currentProgram.getFunctionManager();

        // Pattern.quote avoids any source-level escaping of the regex metachar.
        for (String text : spec.split(java.util.regex.Pattern.quote("|"))) {
            if (text.isEmpty()) {
                continue;
            }
            println("======================================================");
            println("STRING: " + text);
            byte[] pattern = (text + "\0").getBytes("US-ASCII");
            Address at = mem.getMinAddress();
            int occurrences = 0;
            while (occurrences < 12) {
                Address hit = mem.findBytes(at, pattern, null, true, monitor);
                if (hit == null) {
                    break;
                }
                occurrences++;
                Set<String> fns = new TreeSet<>();
                for (Reference r : refMgr.getReferencesTo(hit)) {
                    Function fn = fnMgr.getFunctionContaining(r.getFromAddress());
                    fns.add(fn == null
                            ? r.getFromAddress() + "  <data>"
                            : fn.getEntryPoint() + "  size=" + fn.getBody().getNumAddresses()
                              + "  " + fn.getName());
                }
                println("  at " + hit + " -> " + fns.size() + " referring function(s)");
                for (String s : fns) {
                    println("      " + s);
                }
                at = hit.add(1);
            }
            if (occurrences == 0) {
                println("  not found");
            }
        }
        println("======================================================");
        println("DONE");
    }
}
