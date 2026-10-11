// Dump disassembly context around given addresses, to learn the layout of the
// script-action registration table.
//
//@category CNC3

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;

public class DumpContext extends GhidraScript {

    // References to the ownership script-action name strings.
    private static String[] sites() {
        String env = System.getenv("ASTA_SITES");
        return (env == null || env.isEmpty()) ? new String[0] : env.split(",");
    }

    private static final int BEFORE = 4;
    private static final int AFTER = 22;

    @Override
    public void run() throws Exception {
        Listing listing = currentProgram.getListing();
        for (String s : sites()) {
            Address site = currentProgram.getAddressFactory().getAddress(s);
            println("==================================================");
            println("CONTEXT AROUND " + site);

            // Walk backwards to find a starting instruction.
            Instruction ins = listing.getInstructionContaining(site);
            if (ins == null) {
                println("  no instruction at site");
                continue;
            }
            Instruction cur = ins;
            for (int i = 0; i < BEFORE && cur != null; i++) {
                Instruction prev = cur.getPrevious();
                if (prev == null) {
                    break;
                }
                cur = prev;
            }
            for (int i = 0; i < BEFORE + AFTER && cur != null; i++) {
                String mark = cur.getAddress().equals(ins.getAddress()) ? ">>" : "  ";
                println(String.format("%s %s  %s", mark, cur.getAddress(), cur.toString()));
                cur = cur.getNext();
            }
        }
        println("==================================================");
        println("DONE");
    }
}
