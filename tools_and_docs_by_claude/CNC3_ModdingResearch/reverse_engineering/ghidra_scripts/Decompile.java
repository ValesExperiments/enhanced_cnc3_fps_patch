// Decompile specific functions to C. Addresses come from the ASTA_FUNCS
// environment variable (comma-separated hex), or fall back to a default list.
//
//@category CNC3

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class Decompile extends GhidraScript {

    private static final String DEFAULT = "00653815";

    @Override
    public void run() throws Exception {
        String spec = System.getenv("ASTA_FUNCS");
        if (spec == null || spec.isEmpty()) {
            spec = DEFAULT;
        }

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try {
            for (String part : spec.split(",")) {
                String addrText = part.trim();
                if (addrText.isEmpty()) {
                    continue;
                }
                Address a = currentProgram.getAddressFactory().getAddress(addrText);
                Function fn = getFunctionContaining(a);
                println("======================================================");
                if (fn == null) {
                    println("NO FUNCTION AT " + addrText);
                    continue;
                }
                println("FUNCTION " + fn.getName() + " @ " + fn.getEntryPoint()
                        + "  size=" + fn.getBody().getNumAddresses());
                DecompileResults res = decomp.decompileFunction(fn, 90, monitor);
                if (res == null || !res.decompileCompleted()) {
                    println("  decompile failed: "
                            + (res == null ? "null" : res.getErrorMessage()));
                    continue;
                }
                println(res.getDecompiledFunction().getC());
            }
        } finally {
            decomp.dispose();
        }
        println("======================================================");
        println("DONE");
    }
}
