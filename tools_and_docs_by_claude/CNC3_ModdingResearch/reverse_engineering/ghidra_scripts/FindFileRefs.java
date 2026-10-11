// Find every function containing an assert that cites a given source file.
// Asserts embed __FILE__, so the functions referencing "Object.cpp" are
// Object:: methods. This localises the class without symbols.
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

public class FindFileRefs extends GhidraScript {

    // Backslash built as a char so no source-level escaping is involved.
    private static final String BS = String.valueOf((char) 92);

    private static final String[] FILES = {
        "gamelogic" + BS + "object" + BS + "Object.cpp",
        "gamelogic" + BS + "object" + BS + "Contain" + BS + "GarrisonContain.cpp",
        "gamelogic" + BS + "object" + BS + "Contain" + BS + "OpenContain.cpp",
    };

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        ReferenceManager refMgr = currentProgram.getReferenceManager();
        FunctionManager fnMgr = currentProgram.getFunctionManager();

        for (String file : FILES) {
            println("======================================================");
            println("FILE: " + file);
            byte[] pattern = (file + "\0").getBytes("US-ASCII");
            Address at = mem.getMinAddress();
            Address hit = mem.findBytes(at, pattern, null, true, monitor);
            if (hit == null) {
                println("  string not found");
                continue;
            }
            Address start = stringStart(mem, hit);
            println("  match at " + hit + ", string starts at " + start);
            hit = start;

            Set<String> fns = new TreeSet<>();
            int rawRefs = 0;
            for (Reference r : refMgr.getReferencesTo(hit)) {
                rawRefs++;
                Function fn = fnMgr.getFunctionContaining(r.getFromAddress());
                if (fn != null) {
                    fns.add(fn.getEntryPoint() + "  size=" + fn.getBody().getNumAddresses()
                            + "  " + fn.getName());
                } else {
                    fns.add(r.getFromAddress() + "  <no function>");
                }
            }
            println("  " + rawRefs + " references, " + fns.size() + " distinct functions:");
            for (String s : fns) {
                println("    " + s);
            }
        }
        println("======================================================");
        println("DONE");
    }

    /**
     * The match is a substring of a longer "C:\Projects\..." path, but code
     * references the START of that string. Walk back over printable ASCII to
     * find where the NUL-terminated string actually begins.
     */
    private Address stringStart(Memory mem, Address hit) throws Exception {
        Address cur = hit;
        while (true) {
            Address prev = cur.subtract(1);
            int b = mem.getByte(prev) & 0xff;
            if (b < 0x20 || b > 0x7e) {
                return cur;
            }
            cur = prev;
        }
    }
}
