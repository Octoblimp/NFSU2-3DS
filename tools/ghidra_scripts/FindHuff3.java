// The magic bytes are inline CMP immediates inside code (cmp ecx, 'JDLZ'),
// not Data-referenced strings -- so find the function *containing* each hit
// address directly, decompile it, and also decompile everything it calls
// (one level deep) to reach the actual HUFF bitstream decoder.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.util.task.ConsoleTaskMonitor;

import java.util.HashSet;
import java.util.Set;

public class FindHuff3 extends GhidraScript {
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        byte[][] patterns = { "HUFF".getBytes(), "JDLZ".getBytes() };
        Set<Function> containing = new HashSet<>();

        for (MemoryBlock block : mem.getBlocks()) {
            if (!block.isInitialized()) continue;
            for (byte[] pat : patterns) {
                Address found = mem.findBytes(block.getStart(), block.getEnd(), pat, null, true, monitor);
                while (found != null) {
                    Function f = currentProgram.getFunctionManager().getFunctionContaining(found);
                    if (f != null) {
                        println("MAGIC " + new String(pat) + " @ " + found + " inside function " + f.getName() + " @ " + f.getEntryPoint());
                        containing.add(f);
                    } else {
                        println("MAGIC " + new String(pat) + " @ " + found + " -- NOT inside any defined function");
                    }
                    try {
                        found = mem.findBytes(found.add(1), block.getEnd(), pat, null, true, monitor);
                    } catch (Exception e) {
                        break;
                    }
                }
            }
        }

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor mon2 = new ConsoleTaskMonitor();

        Set<Function> toDecompile = new HashSet<>(containing);
        for (Function f : containing) {
            for (Function callee : f.getCalledFunctions(mon2)) {
                toDecompile.add(callee);
            }
        }

        for (Function func : toDecompile) {
            println("\n\n##### DECOMPILE: " + func.getName() + " @ " + func.getEntryPoint() + " #####");
            DecompileResults res = decomp.decompileFunction(func, 60, mon2);
            if (res.decompileCompleted()) {
                println(res.getDecompiledFunction().getC());
            } else {
                println("decompile failed: " + res.getErrorMessage());
            }
        }
    }
}
