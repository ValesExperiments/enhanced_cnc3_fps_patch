// Recover the engine's named subsystem globals.
//
// The registration helper is called as:
//     PUSH <instance>            (in a register)
//     PUSH <nameString>
//     PUSH <globalSlot>
//     CALL helper
// so walking back from each call gives globalVA + name.
//
//@category CNC3

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

import java.util.ArrayList;
import java.util.List;

public class DumpSubsystems extends GhidraScript {

    @Override
    public void run() throws Exception {
        String env = System.getenv("ASTA_HELPER");
        long helper = Long.decode(env == null ? "0x0055331b" : env.trim());
        println("registration helper = 0x" + Long.toHexString(helper));

        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        int found = 0;

        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            if (!ins.getMnemonicString().equalsIgnoreCase("CALL")) {
                continue;
            }
            boolean isHelper = false;
            for (int i = 0; i < ins.getNumOperands(); i++) {
                for (Object o : ins.getOpObjects(i)) {
                    if (o instanceof Address && ((Address) o).getOffset() == helper) {
                        isHelper = true;
                    } else if (o instanceof Scalar
                               && ((Scalar) o).getUnsignedValue() == helper) {
                        isHelper = true;
                    }
                }
            }
            if (!isHelper) {
                continue;
            }

            // Collect immediate PUSHes immediately preceding the call.
            List<Long> pushes = new ArrayList<>();
            Instruction cur = ins.getPrevious();
            for (int back = 0; back < 10 && cur != null; back++, cur = cur.getPrevious()) {
                if (!cur.getMnemonicString().equalsIgnoreCase("PUSH")) {
                    continue;
                }
                Object[] objs = cur.getOpObjects(0);
                if (objs.length == 1 && objs[0] instanceof Scalar) {
                    pushes.add(((Scalar) objs[0]).getUnsignedValue());
                }
            }
            if (pushes.size() < 2) {
                continue;
            }

            // Nearest push is the global slot, next is the name string.
            long globalVA = pushes.get(0);
            long nameVA = pushes.get(1);
            String name = readCString(toAddr(nameVA));
            if (name == null || name.isEmpty()) {
                continue;
            }
            found++;
            println(String.format("  %-28s global 0x%08x   (call at %s)",
                                  name, globalVA, ins.getAddress()));
        }
        println("total subsystems: " + found);
        println("DONE");
    }

    private String readCString(Address a) {
        StringBuilder sb = new StringBuilder();
        try {
            for (int i = 0; i < 64; i++) {
                byte b = getByte(a.add(i));
                if (b == 0) {
                    break;
                }
                if (b < 0x20 || b > 0x7e) {
                    return null;
                }
                sb.append((char) b);
            }
        } catch (Exception e) {
            return null;
        }
        return sb.toString();
    }
}
