// Report what the RTTI analyser recovered for the classes we care about:
// namespaces, vftable symbols and member function counts.
//
//@category CNC3

import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;

public class DumpClasses extends GhidraScript {

    private static final String[] CLASSES = {
        "Object", "Team", "Player", "PlayerList", "Squad", "ScriptActions", "GameLogic",
    };

    @Override
    public void run() throws Exception {
        SymbolTable st = currentProgram.getSymbolTable();

        println("total symbols: " + st.getNumSymbols());

        int vft = 0;
        SymbolIterator it = st.getAllSymbols(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Symbol s = it.next();
            String n = s.getName();
            if (n.contains("vftable") || n.contains("vtable")) {
                vft++;
            }
        }
        println("vftable-ish symbols: " + vft);

        for (String cls : CLASSES) {
            println("======================================================");
            println("CLASS: " + cls);
            int shown = 0;
            SymbolIterator si = st.getSymbolIterator(cls + "*", true);
            while (si.hasNext() && shown < 12 && !monitor.isCancelled()) {
                Symbol s = si.next();
                println("    " + s.getAddress() + "  " + s.getName()
                        + "   parent=" + s.getParentNamespace().getName());
                shown++;
            }
            if (shown == 0) {
                println("    (no symbols matching)");
            }
        }
        println("======================================================");
        println("DONE");
    }
}
