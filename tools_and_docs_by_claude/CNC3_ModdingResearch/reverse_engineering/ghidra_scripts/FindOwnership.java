// Locate the ownership-transfer implementation in cnc3game.dat.
//
// The script-action name strings live in .rdata. Whatever references them is
// the script-action registration table, and from there we reach the
// ScriptActions method that performs the transfer.
//
//@category CNC3

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

public class FindOwnership extends GhidraScript {

    private static final String[] TARGETS = {
        "NAMED_TRANSFER_OWNERSHIP_PLAYER",
        "PLAYER_TRANSFER_OWNERSHIP_PLAYER",
        "SET_PLAYER_OWNERSHIP_OF_TYPE_COUNTER",
    };

    private Memory mem;
    private ReferenceManager refMgr;
    private FunctionManager fnMgr;

    @Override
    public void run() throws Exception {
        mem = currentProgram.getMemory();
        refMgr = currentProgram.getReferenceManager();
        fnMgr = currentProgram.getFunctionManager();

        println("image base = " + currentProgram.getImageBase());

        for (String name : TARGETS) {
            println("======================================================");
            println("STRING: " + name);
            byte[] pattern = (name + "\0").getBytes("US-ASCII");
            Address at = mem.getMinAddress();
            int hits = 0;
            while (true) {
                Address hit = mem.findBytes(at, pattern, null, true, monitor);
                if (hit == null) {
                    break;
                }
                hits++;
                println("  found at " + hit);
                describeRefs(hit, "    ");
                at = hit.add(1);
            }
            if (hits == 0) {
                println("  NOT FOUND");
            }
        }
        println("======================================================");
        println("DONE");
    }

    /** Print what references addr; if a referrer is data, walk one level up. */
    private void describeRefs(Address addr, String indent) {
        boolean any = false;
        for (Reference r : refMgr.getReferencesTo(addr)) {
            any = true;
            Address from = r.getFromAddress();
            Function fn = fnMgr.getFunctionContaining(from);
            println(indent + "ref from " + from + "  [" + describe(fn) + "]  " + r.getReferenceType());
            if (fn == null) {
                // Referrer is data (a table entry) -- find who reads that table.
                for (Reference up : refMgr.getReferencesTo(from)) {
                    Function upFn = fnMgr.getFunctionContaining(up.getFromAddress());
                    println(indent + "    <- " + up.getFromAddress() + " [" + describe(upFn) + "]");
                }
            }
        }
        if (!any) {
            println(indent + "(no references)");
        }
    }

    private String describe(Function fn) {
        return fn == null ? "data/no function" : fn.getName() + " @ " + fn.getEntryPoint();
    }
}
