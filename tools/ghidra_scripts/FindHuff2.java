// Search raw memory bytes for "HUFF"/"JDLZ" (not relying on Data being marked
// as a string -- the magic may be an inline DWORD compare with no Data literal
// at all), then find code referencing any address within +/- 4 bytes of each
// hit, and decompile the containing functions.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.util.task.ConsoleTaskMonitor;

import java.util.HashSet;
import java.util.Set;

public class FindHuff2 extends GhidraScript {
    @Override
    public void run() throws Exception {
        println("=== Program: " + currentProgram.getName() + " ===");
        Memory mem = currentProgram.getMemory();
        byte[][] patterns = { "HUFF".getBytes(), "JDLZ".getBytes() };
        Set<Function> targetFuncs = new HashSet<>();

        for (MemoryBlock block : mem.getBlocks()) {
            if (!block.isInitialized()) continue;
            for (byte[] pat : patterns) {
                Address start = block.getStart();
                Address found = mem.findBytes(start, block.getEnd(), pat, null, true, monitor);
                while (found != null) {
                    println("MAGIC " + new String(pat) + " @ " + found);
                    // references directly to this address
                    ReferenceIterator refIt = currentProgram.getReferenceManager().getReferencesTo(found);
                    while (refIt.hasNext()) {
                        Reference ref = refIt.next();
                        Function f = currentProgram.getFunctionManager().getFunctionContaining(ref.getFromAddress());
                        if (f != null) {
                            println("  ref by " + f.getName() + " @ " + f.getEntryPoint());
                            targetFuncs.add(f);
                        }
                    }
                    try {
                        found = mem.findBytes(found.add(1), block.getEnd(), pat, null, true, monitor);
                    } catch (Exception e) {
                        break;
                    }
                }
            }
        }

        if (targetFuncs.isEmpty()) {
            println("No direct references found to raw magic bytes either.");
        }

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor2 = new ConsoleTaskMonitor();
        for (Function func : targetFuncs) {
            println("\n\n##### DECOMPILE: " + func.getName() + " @ " + func.getEntryPoint() + " #####");
            DecompileResults res = decomp.decompileFunction(func, 60, monitor2);
            if (res.decompileCompleted()) {
                println(res.getDecompiledFunction().getC());
            } else {
                println("decompile failed: " + res.getErrorMessage());
            }
        }
    }
}
