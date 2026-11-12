import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

// Generic headless decompile-by-name recipe, so a session does not have to edit
// a .java file to ask a new question of either binary. Two env vars drive it:
//
//   DECOMPILE_NAMES  semicolon-separated symbol names or 0x-less hex addresses.
//                    A name is matched exactly first; if nothing matches
//                    exactly, every symbol whose name CONTAINS it is taken.
//   DECOMPILE_OUT    output path (default decompile_out.c).
//
// Invoke exactly as docs/formats/frontend-menu-re.md's headless recipe says:
// from the Development parent directory, through PowerShell, with relative
// paths (the .bat mangles absolute paths containing spaces).
public class DecompileNames extends GhidraScript {

    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        String outPath = System.getenv("DECOMPILE_OUT");
        if (outPath == null) outPath = "decompile_out.c";
        String namesEnv = System.getenv("DECOMPILE_NAMES");
        if (namesEnv == null) namesEnv = "";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        SymbolTable st = currentProgram.getSymbolTable();
        for (String raw : namesEnv.split(";")) {
            String name = raw.trim();
            if (name.isEmpty()) continue;
            out.println("// ==== " + name + " ====");

            Set<Address> hits = new LinkedHashSet<>();
            if (name.matches("[0-9a-fA-F]{6,8}")) {
                hits.add(currentProgram.getAddressFactory().getAddress(name));
            } else {
                for (Symbol s : st.getSymbols(name)) hits.add(s.getAddress());
                if (hits.isEmpty()) {
                    List<String> seen = new ArrayList<>();
                    for (Symbol s : st.getAllSymbols(true)) {
                        if (s.getName().contains(name)) {
                            hits.add(s.getAddress());
                            seen.add(s.getName() + " @ " + s.getAddress());
                        }
                    }
                    for (String s : seen) out.println("//   substring match: " + s);
                }
            }
            if (hits.isEmpty()) {
                out.println("// NOT FOUND");
                continue;
            }
            for (Address addr : hits) {
                Function f = currentProgram.getFunctionManager().getFunctionContaining(addr);
                if (f == null) {
                    out.println("// " + addr + " : no function here (data symbol?)");
                    continue;
                }
                out.println("// ---- " + f.getEntryPoint() + " " + f.getName() + " ----");
                DecompileResults res = decomp.decompileFunction(f, 120, monitor);
                if (res != null && res.decompileCompleted())
                    out.println(res.getDecompiledFunction().getC());
                else
                    out.println("// decompile failed: " + (res != null ? res.getErrorMessage() : "null"));
            }
        }
        out.flush();
        out.close();
        println("Wrote " + outPath);
    }
}
