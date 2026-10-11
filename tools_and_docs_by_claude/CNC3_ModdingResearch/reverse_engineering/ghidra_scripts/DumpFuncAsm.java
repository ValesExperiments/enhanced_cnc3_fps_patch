// Dump every instruction of a function, for offline parsing.
//@category CNC3

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class DumpFuncAsm extends GhidraScript {
    @Override
    public void run() throws Exception {
        String env = System.getenv("ASTA_FUNCS");
        for (String part : env.split(",")) {
            Address a = currentProgram.getAddressFactory().getAddress(part.trim());
            Function fn = getFunctionContaining(a);
            if (fn == null) {
                println("NO FUNCTION AT " + part);
                continue;
            }
            println("=== FUNCTION " + fn.getEntryPoint() + " ===");
            InstructionIterator it =
                currentProgram.getListing().getInstructions(fn.getBody(), true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                println(ins.getAddress() + "  " + ins);
            }
        }
        println("DONE");
    }
}
