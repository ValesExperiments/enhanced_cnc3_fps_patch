// For every site that loads a given global into a register, look at the next
// few instructions and record which field offset of that object is read.
// The most frequently accessed field of ThePlayerList should be the local
// player, since that is what game code asks for constantly.
//
//@category CNC3

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.lang.OperandType;
import ghidra.program.model.scalar.Scalar;

import java.util.HashMap;
import java.util.Map;
import java.util.ArrayList;
import java.util.List;

public class FieldAccessStats extends GhidraScript {

    @Override
    public void run() throws Exception {
        String env = System.getenv("ASTA_ADDR");
        long target = Long.decode(env.trim());
        println("global 0x" + Long.toHexString(target));

        Map<Long, Integer> offsets = new HashMap<>();
        Map<Long, List<String>> examples = new HashMap<>();
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        int sites = 0;

        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            boolean loadsGlobal = false;
            for (int i = 0; i < ins.getNumOperands(); i++) {
                for (Object o : ins.getOpObjects(i)) {
                    if (o instanceof Scalar && ((Scalar) o).getUnsignedValue() == target) {
                        loadsGlobal = true;
                    } else if (o instanceof Address && ((Address) o).getOffset() == target) {
                        loadsGlobal = true;
                    }
                }
            }
            if (!loadsGlobal || !ins.getMnemonicString().toUpperCase().startsWith("MOV")) {
                continue;
            }
            sites++;

            // The register just loaded is the destination of this MOV.
            Object[] dst = ins.getOpObjects(0);
            if (dst.length == 0) {
                continue;
            }
            String reg = dst[0].toString();

            // Look ahead a few instructions for a read of [reg + disp].
            Instruction cur = ins.getNext();
            for (int step = 0; step < 3 && cur != null; step++, cur = cur.getNext()) {
                if (!cur.getMnemonicString().toUpperCase().startsWith("MOV")) {
                    break;
                }
                for (int i = 0; i < cur.getNumOperands(); i++) {
                    if (!OperandType.isDynamic(cur.getOperandType(i))) {
                        continue;
                    }
                    Object[] objs = cur.getOpObjects(i);
                    boolean usesReg = false;
                    Long disp = null;
                    for (Object o : objs) {
                        if (o.toString().equals(reg)) {
                            usesReg = true;
                        } else if (o instanceof Scalar) {
                            disp = ((Scalar) o).getUnsignedValue();
                        }
                    }
                    if (usesReg) {
                        long d = disp == null ? 0L : disp;
                        offsets.merge(d, 1, Integer::sum);
                        examples.computeIfAbsent(d, k -> new ArrayList<>())
                                .add(cur.getAddress() + "  " + cur);
                    }
                }
                break;  // only the instruction immediately after matters most
            }
        }

        println("sites that load the global: " + sites);
        println("");
        println("field offsets read immediately afterwards:");
        offsets.entrySet().stream()
               .sorted((a, b) -> b.getValue() - a.getValue())
               .limit(20)
               .forEach(e -> {
                   println(String.format("   +0x%04x   %d time(s)", e.getKey(), e.getValue()));
                   List<String> ex = examples.get(e.getKey());
                   for (int i = 0; i < Math.min(2, ex.size()); i++) {
                       println("        " + ex.get(i));
                   }
               });
        println("DONE");
    }
}
