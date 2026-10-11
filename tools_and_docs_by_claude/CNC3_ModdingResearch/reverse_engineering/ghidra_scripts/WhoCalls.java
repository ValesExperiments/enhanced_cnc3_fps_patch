// Report the function containing an address, and every function that calls it.
//
// Motivation: every Object::setTeam call in a live capture arrives through one
// wrapper (return address VA 0x64722b), so the immediate caller cannot tell the
// "transfer the building" path from the "transfer the docked unit" path. The
// distinction has to come from the wrapper's own callers, and a stack walk is
// unreliable in this binary (no frame pointers).
//
// ASTA_ADDRS = comma-separated hex VAs to resolve.
//
//@category CNC3

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class WhoCalls extends GhidraScript {

    private static final String DEFAULT = "0064722b";

    @Override
    public void run() throws Exception {
        String spec = System.getenv("ASTA_ADDRS");
        if (spec == null || spec.isEmpty()) {
            spec = DEFAULT;
        }
        FunctionManager fm = currentProgram.getFunctionManager();

        for (String part : spec.split(",")) {
            part = part.trim();
            if (part.isEmpty()) {
                continue;
            }
            Address a = toAddr(Long.parseLong(part, 16));
            println("=== " + part + " ===");

            Function f = fm.getFunctionContaining(a);
            if (f == null) {
                println("  no function contains this address");
                continue;
            }
            println("  inside: " + f.getName() + " @ " + f.getEntryPoint()
                    + "  size=" + f.getBody().getNumAddresses());

            int n = 0;
            ReferenceIterator it =
                currentProgram.getReferenceManager()
                    .getReferencesTo(f.getEntryPoint());
            while (it.hasNext()) {
                Reference r = it.next();
                Function c = fm.getFunctionContaining(r.getFromAddress());
                String who = (c == null) ? "(not in a function)"
                        : c.getName() + " @ " + c.getEntryPoint();
                println("  called from " + r.getFromAddress() + "  by " + who);
                n++;
                if (n >= 60) {
                    println("  ... (truncated)");
                    break;
                }
            }
            println("  " + n + " caller reference(s)");
        }
    }
}
