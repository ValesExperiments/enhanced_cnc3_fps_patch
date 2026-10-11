// Decompile functions after forcing __thiscall, so the implicit `this` pointer
// is modelled and member offsets appear in the output.
//
//@category CNC3

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class DecompileThis extends GhidraScript {

    @Override
    public void run() throws Exception {
        String spec = System.getenv("ASTA_FUNCS");
        if (spec == null || spec.isEmpty()) {
            println("set ASTA_FUNCS");
            return;
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
                try {
                    fn.setCallingConvention("__thiscall");
                } catch (Exception e) {
                    println("  (could not set __thiscall: " + e.getMessage() + ")");
                }
                println("FUNCTION " + fn.getName() + " @ " + fn.getEntryPoint()
                        + "  conv=" + fn.getCallingConventionName());
                DecompileResults res = decomp.decompileFunction(fn, 180, monitor);
                if (res == null || !res.decompileCompleted()) {
                    println("  decompile failed");
                    continue;
                }
                println(res.getDecompiledFunction().getC());
            }
        } finally {
            decomp.dispose();
        }
        println("DONE");
    }
}
