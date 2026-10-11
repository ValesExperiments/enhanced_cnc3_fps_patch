// Find every instruction that WRITES to [reg + OFFSET], where OFFSET is the
// Object field we identified at runtime as the Team pointer (0x33C).
//
// Naively poking that pointer would corrupt the engine's per-team object
// lists, so the mod must call the real setter. This locates it.
//
//@category CNC3

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.lang.OperandType;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

import java.util.LinkedHashMap;
import java.util.Map;

public class FindFieldWrites extends GhidraScript {

    @Override
    public void run() throws Exception {
        String env = System.getenv("ASTA_OFFSET");
        long target = (env == null || env.isEmpty()) ? 0x33CL : Long.decode(env.trim());
        println("searching for accesses at field offset 0x" + Long.toHexString(target));

        FunctionManager fnMgr = currentProgram.getFunctionManager();
        InstructionIterator it = currentProgram.getListing().getInstructions(true);

        Map<String, Integer> writersByFunction = new LinkedHashMap<>();
        int writes = 0;
        int reads = 0;

        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            int nops = ins.getNumOperands();
            boolean hit = false;
            int memOperand = -1;

            for (int i = 0; i < nops; i++) {
                for (Object o : ins.getOpObjects(i)) {
                    if (o instanceof Scalar && ((Scalar) o).getUnsignedValue() == target) {
                        if (OperandType.isDynamic(ins.getOperandType(i))) {
                            hit = true;
                            memOperand = i;
                        }
                    }
                }
            }
            if (!hit) {
                continue;
            }

            // Operand 0 being the memory operand means it is the destination.
            boolean isWrite = memOperand == 0
                    && ins.getMnemonicString().toUpperCase().startsWith("MOV");
            if (isWrite) {
                writes++;
                Function fn = fnMgr.getFunctionContaining(ins.getAddress());
                String key = fn == null
                        ? "<no function> @ " + ins.getAddress()
                        : fn.getEntryPoint() + "  size=" + fn.getBody().getNumAddresses()
                          + "  " + fn.getName();
                writersByFunction.merge(key, 1, Integer::sum);
                println("  WRITE  " + ins.getAddress() + "  " + ins
                        + "   [" + (fn == null ? "?" : fn.getName()) + "]");
            } else {
                reads++;
            }
        }

        println("");
        println("total writes: " + writes + ", other accesses (reads/lea): " + reads);
        println("");
        println("Functions containing writes:");
        for (Map.Entry<String, Integer> e : writersByFunction.entrySet()) {
            println("    " + e.getValue() + "x  " + e.getKey());
        }
        println("DONE");
    }
}
