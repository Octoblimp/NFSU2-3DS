import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import java.io.PrintWriter;

// Generic, parameterised headless helper -- no source edit needed per run.
//
//   DECOMPILE_ADDRS  comma-separated hex addresses to decompile (function containing addr)
//   DECOMPILE_ASM    comma-separated hex addresses to raw-disassemble instead
//   DECOMPILE_SYMS   comma-separated substrings to match against symbol names
//   DECOMPILE_SCAN   comma-separated substrings to grep across all disassembly
//   DECOMPILE_XREFS  comma-separated hex addresses to list callers of
//   DECOMPILE_OUT    output file (default decompile_out.c)
//
// See "Ghidra headless decompile recipe" in docs/formats/frontend-menu-re.md.
public class DecompileAddrs extends GhidraScript {

    private Address addr(String s) {
        s = s.trim();
        if (s.startsWith("0x") || s.startsWith("0X")) s = s.substring(2);
        return currentProgram.getAddressFactory().getDefaultAddressSpace()
                .getAddress(Long.parseLong(s, 16));
    }

    @Override
    public void run() throws Exception {
        String outPath = System.getenv("DECOMPILE_OUT");
        if (outPath == null) outPath = "decompile_out.c";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        String xrefs = System.getenv("DECOMPILE_XREFS");
        if (xrefs != null && !xrefs.isEmpty()) {
            for (String s : xrefs.split(",")) {
                if (s.trim().isEmpty()) continue;
                Address a = addr(s);
                out.println("// ==== xrefs to " + a + " ====");
                ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(a);
                while (it.hasNext()) {
                    Reference r = it.next();
                    Function f = currentProgram.getFunctionManager()
                            .getFunctionContaining(r.getFromAddress());
                    out.println("//   from " + r.getFromAddress() + " (" + r.getReferenceType()
                            + ") in " + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
                }
            }
        }

        String sym = System.getenv("DECOMPILE_SYMS");
        if (sym != null && !sym.isEmpty()) {
            String[] needles = sym.split(",");
            for (ghidra.program.model.symbol.Symbol sy :
                    currentProgram.getSymbolTable().getAllSymbols(true)) {
                String nm = sy.getName();
                for (String nd : needles) {
                    if (nd.trim().isEmpty()) continue;
                    if (nm.toLowerCase().contains(nd.trim().toLowerCase())) {
                        out.println("// sym " + sy.getAddress() + "  " + nm);
                        break;
                    }
                }
            }
        }

        String scan = System.getenv("DECOMPILE_SCAN");
        if (scan != null && !scan.isEmpty()) {
            String[] needles = scan.split(",");
            InstructionIterator it = currentProgram.getListing().getInstructions(true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                String t = ins.toString();
                for (String nd : needles) {
                    if (nd.trim().isEmpty()) continue;
                    if (t.contains(nd.trim())) {
                        Function f = currentProgram.getFunctionManager()
                                .getFunctionContaining(ins.getAddress());
                        out.println("// scan " + ins.getAddress() + ": " + t + "   in "
                                + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
                        break;
                    }
                }
            }
        }

        String asm = System.getenv("DECOMPILE_ASM");
        if (asm != null && !asm.isEmpty()) {
            for (String s : asm.split(",")) {
                if (s.trim().isEmpty()) continue;
                Address a = addr(s);
                Function f = currentProgram.getFunctionManager().getFunctionContaining(a);
                Address end = f != null ? f.getBody().getMaxAddress() : a.add(0x400);
                out.println("// ==== asm " + a + " (" + (f == null ? "?" : f.getName())
                        + ") .. " + end + " ====");
                InstructionIterator it = currentProgram.getListing().getInstructions(a, true);
                while (it.hasNext()) {
                    Instruction ins = it.next();
                    if (ins.getAddress().compareTo(end) > 0) break;
                    out.println(ins.getAddress() + ":  " + ins.toString());
                }
            }
        }

        String addrs = System.getenv("DECOMPILE_ADDRS");
        if (addrs != null && !addrs.isEmpty()) {
            DecompInterface decomp = new DecompInterface();
            decomp.openProgram(currentProgram);
            for (String s : addrs.split(",")) {
                if (s.trim().isEmpty()) continue;
                Address a = addr(s);
                Function f = currentProgram.getFunctionManager().getFunctionContaining(a);
                // Code the analysis never reached (vtable-only entries on the
                // PC): take the address as an entry point and define the
                // function for this run. The project is opened -readOnly, so
                // nothing is saved.
                boolean created = false;
                if (f == null) {
                    disassemble(a);
                    f = createFunction(a, null);
                    created = f != null;
                }
                out.println("// ==== " + a + " -> " + (f == null ? "NOT FOUND" : f.getName()
                        + " @ " + f.getEntryPoint() + (created ? " (defined here)" : "")) + " ====");
                if (f == null) continue;
                DecompileResults res = decomp.decompileFunction(f, 120, monitor);
                if (res != null && res.decompileCompleted()) {
                    out.println(res.getDecompiledFunction().getC());
                } else {
                    out.println("// decompile failed: "
                            + (res != null ? res.getErrorMessage() : "null result"));
                }
            }
        }
        out.flush();
        out.close();
        println("Wrote " + outPath);
    }
}
