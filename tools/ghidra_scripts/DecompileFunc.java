import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.symbol.Symbol;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public class DecompileFunc extends GhidraScript {

    @Override
    public void run() throws Exception {
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        String outPath = System.getenv("DECOMPILE_OUT");
        if (outPath == null) outPath = "decompile_out.c";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        // 1. find callers of RenderObject__7FEngineP8FEObjectR9FEMatrix4Us
        Address target = currentProgram.getAddressFactory().getAddress("801478f8");
        Set<Address> callerFuncAddrs = new HashSet<>();
        ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(target);
        while (refs.hasNext()) {
            Reference r = refs.next();
            Function f = currentProgram.getFunctionManager().getFunctionContaining(r.getFromAddress());
            if (f != null) callerFuncAddrs.add(f.getEntryPoint());
        }
        out.println("// Callers of RenderObject__7FEngine (801478f8): " + callerFuncAddrs);

        // 2. find MakeRenderMatrix by name search
        SymbolTable st = currentProgram.getSymbolTable();
        List<Address> extra = new ArrayList<>();
        for (Symbol s : st.getAllSymbols(true)) {
            String nm = s.getName();
            if (nm.contains("MakeRenderMatrix") || nm.contains("RenderGroup") || nm.contains("RenderChildren")) {
                extra.add(s.getAddress());
                out.println("// found symbol: " + nm + " @ " + s.getAddress());
            }
        }

        List<Address> toDecompile = new ArrayList<>();
        toDecompile.addAll(callerFuncAddrs);
        toDecompile.addAll(extra);

        for (Address addr : toDecompile) {
            Function f = currentProgram.getFunctionManager().getFunctionContaining(addr);
            out.println("// ==== " + addr + " -> " + (f == null ? "NOT FOUND" : f.getName()) + " ====");
            if (f == null) continue;
            DecompileResults res = decomp.decompileFunction(f, 60, monitor);
            if (res != null && res.decompileCompleted()) {
                out.println(res.getDecompiledFunction().getC());
            } else {
                out.println("// decompile failed: " + (res != null ? res.getErrorMessage() : "null result"));
            }
        }
        out.flush();
        out.close();
        println("Wrote " + outPath);
    }
}
