// For each named class, list the symbols Ghidra placed in its namespace and, if
// a vftable is present, dump the virtual function pointers it holds.
//
//@category CNC3

import ghidra.program.model.address.Address;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;

public class DumpClassDetail extends GhidraScript {

    private static final String[] CLASSES = {
        "HordeGarrisonContain", "GarrisonContain",
    };

    private static final int MAX_VFUNCS = 60;

    @Override
    public void run() throws Exception {
        SymbolTable st = currentProgram.getSymbolTable();
        Memory mem = currentProgram.getMemory();
        FunctionManager fnMgr = currentProgram.getFunctionManager();

        for (String cls : CLASSES) {
            println("======================================================");
            println("CLASS: " + cls);

            Namespace ns = null;
            SymbolIterator si = st.getSymbolIterator(cls, true);
            while (si.hasNext()) {
                Symbol s = si.next();
                if (s.getName().equals(cls) && s.getObject() instanceof Namespace) {
                    ns = (Namespace) s.getObject();
                    break;
                }
            }
            if (ns == null) {
                println("  no namespace recovered");
                continue;
            }

            for (Symbol s : st.getSymbols(ns)) {
                println("  " + s.getAddress() + "  " + s.getName());
                if (!s.getName().contains("vftable")) {
                    continue;
                }
                // Dump the vtable slots.
                Address p = s.getAddress();
                for (int i = 0; i < MAX_VFUNCS; i++) {
                    Address slot = p.add(i * 4L);
                    long target;
                    try {
                        target = mem.getInt(slot) & 0xffffffffL;
                    } catch (Exception e) {
                        break;
                    }
                    Address fa = currentProgram.getAddressFactory()
                            .getDefaultAddressSpace().getAddress(target);
                    Function fn = fnMgr.getFunctionAt(fa);
                    if (fn == null) {
                        break;  // end of table
                    }
                    println(String.format("      [%2d] %s  %s  size=%d",
                            i, fa, fn.getName(), fn.getBody().getNumAddresses()));
                }
            }
        }
        println("======================================================");
        println("DONE");
    }
}
