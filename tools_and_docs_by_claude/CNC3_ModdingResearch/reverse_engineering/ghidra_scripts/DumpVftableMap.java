// Export every recovered vftable address with its class name, as JSON.
//
// At runtime this turns an opaque pointer into a named class: read the target's
// first dword (its vftable) and look it up. That makes the behaviour/update
// modules hanging off an Object identifiable instead of anonymous.
//
//@category CNC3

import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;

import java.io.PrintWriter;

public class DumpVftableMap extends GhidraScript {

    @Override
    public void run() throws Exception {
        String out = System.getenv("ASTA_OUT");
        if (out == null || out.isEmpty()) {
            println("set ASTA_OUT");
            return;
        }

        SymbolTable st = currentProgram.getSymbolTable();
        PrintWriter w = new PrintWriter(out, "UTF-8");
        w.println("{");

        int count = 0;
        SymbolIterator it = st.getAllSymbols(true);
        StringBuilder sb = new StringBuilder();
        while (it.hasNext() && !monitor.isCancelled()) {
            Symbol s = it.next();
            if (!s.getName().equals("vftable")) {
                continue;
            }
            Namespace ns = s.getParentNamespace();
            String cls = ns == null ? "?" : ns.getName(true);
            if (sb.length() > 0) {
                sb.append(",\n");
            }
            sb.append(String.format("  \"0x%s\": \"%s\"",
                                    s.getAddress().toString(), cls.replace("\"", "'")));
            count++;
        }
        w.println(sb.toString());
        w.println("}");
        w.close();

        println("wrote " + count + " vftable entries to " + out);
        println("DONE");
    }
}
