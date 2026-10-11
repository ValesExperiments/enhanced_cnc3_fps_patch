// Find instructions that reference a given absolute address as an operand.
// Used to locate the PlayerList constructor (which writes the vftable) and,
// from there, the global that stores the singleton.
//
//@category CNC3

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

public class FindAddrRefs extends GhidraScript {

    @Override
    public void run() throws Exception {
        String env = System.getenv("ASTA_ADDR");
        if (env == null || env.isEmpty()) {
            println("set ASTA_ADDR");
            return;
        }
        long target = Long.decode(env.trim());
        println("looking for operand references to 0x" + Long.toHexString(target));

        FunctionManager fnMgr = currentProgram.getFunctionManager();
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        int found = 0;

        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            boolean hit = false;
            for (int i = 0; i < ins.getNumOperands(); i++) {
                for (Object o : ins.getOpObjects(i)) {
                    if (o instanceof Scalar
                            && ((Scalar) o).getUnsignedValue() == target) {
                        hit = true;
                    } else if (o instanceof Address
                            && ((Address) o).getOffset() == target) {
                        hit = true;
                    }
                }
            }
            if (!hit) {
                continue;
            }
            found++;
            Function fn = fnMgr.getFunctionContaining(ins.getAddress());
            println("  " + ins.getAddress() + "  " + ins
                    + "   [" + (fn == null ? "no function"
                                           : fn.getName() + " @ " + fn.getEntryPoint()
                                             + " size=" + fn.getBody().getNumAddresses())
                    + "]");
            if (found > 40) {
                println("  ... stopping after 40");
                break;
            }
        }
        println("total: " + found);
        println("DONE");
    }
}
