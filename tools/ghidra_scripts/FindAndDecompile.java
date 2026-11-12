import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import java.io.PrintWriter;

// Reusable Ghidra headless recipe: looks up functions by exact symbol name (falling back to
// substring match + xref-caller listing when a name isn't found), and can also dump a vtable's
// function-pointer slots and decompile whatever each one points to. Edit `names`/`vtableNames`
// and re-run via the "Ghidra headless decompile recipe" in docs/formats/frontend-menu-re.md.
public class FindAndDecompile extends GhidraScript {
    String[] names = {
        "Startup__9FEPackageP15FEGameInterface",
        "ConnectObjectResources__9FEPackage",
    };
    // vtable label symbols (e.g. "_vt_17ResourceConnector") to dump + decompile each slot's target
    String[] vtableNames = {
        "_vt.17ResourceConnector",
    };

    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        String outPath = System.getenv("DECOMPILE_OUT");
        if (outPath == null) outPath = "decompile_out.c";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        SymbolTable st = currentProgram.getSymbolTable();
        for (String name : names) {
            out.println("// ==== searching for: " + name + " ====");
            SymbolIterator it = st.getSymbols(name);
            boolean found = false;
            while (it.hasNext()) {
                Symbol sym = it.next();
                Function f = currentProgram.getFunctionManager().getFunctionAt(sym.getAddress());
                if (f == null) continue;
                found = true;
                out.println("// " + sym.getAddress() + " -> " + f.getName());
                DecompileResults res = decomp.decompileFunction(f, 60, monitor);
                if (res != null && res.decompileCompleted()) {
                    out.println(res.getDecompiledFunction().getC());
                } else {
                    out.println("// decompile failed: " + (res != null ? res.getErrorMessage() : "null result"));
                }
            }
            if (!found) out.println("// NOT FOUND by exact name");
        }

        Memory mem = currentProgram.getMemory();
        for (String vtName : vtableNames) {
            SymbolIterator it = st.getSymbols(vtName);
            while (it.hasNext()) {
                Address vtAddr = it.next().getAddress();
                out.println("// ---- dumping vtable " + vtName + " at " + vtAddr + " ----");
                for (int i = 0; i < 8; i++) {
                    Address slot = vtAddr.add(i * 4L);
                    try {
                        int val = mem.getInt(slot);
                        Address target = currentProgram.getAddressFactory().getDefaultAddressSpace()
                                .getAddress(val & 0xFFFFFFFFL);
                        Function f = currentProgram.getFunctionManager().getFunctionAt(target);
                        out.println("// slot " + i + " (" + slot + ") = " + String.format("%08x", val)
                                + (f != null ? " -> " + f.getName() : ""));
                        if (f != null) {
                            DecompileResults res = decomp.decompileFunction(f, 60, monitor);
                            if (res != null && res.decompileCompleted()) {
                                out.println(res.getDecompiledFunction().getC());
                            }
                        }
                    } catch (Exception e) {
                        out.println("// slot " + i + " read failed: " + e.getMessage());
                    }
                }
            }
        }

        out.flush();
        out.close();
        println("Wrote " + outPath);
    }
}
