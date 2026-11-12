// Ghidra headless script (Java -- this Ghidra 12.1.2 install's .py support requires
// PyGhidra/CPython which isn't set up, so plain GhidraScript Java is the path of
// least resistance): locate "HUFF"/"JDLZ" magic strings in the current program,
// find every function referencing them, and dump each one's decompiled pseudocode.
// See docs/formats/texture-tpk.md's open HUFF item / task #28.
//
// Run via: analyzeHeadless <project dir> <project name> -process <program> \
//     -noanalysis -scriptPath tools/ghidra_scripts -postScript FindHuff.java

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;

import java.util.HashSet;
import java.util.Set;

public class FindHuff extends GhidraScript {
    @Override
    public void run() throws Exception {
        println("=== Program: " + currentProgram.getName() + " ===");

        Set<Function> targetFuncs = new HashSet<>();

        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext()) {
            Data data = it.next();
            Object val;
            try {
                val = data.getValue();
            } catch (Exception e) {
                continue;
            }
            if (val == null) continue;
            String s = val.toString();
            if (s.contains("HUFF") || s.contains("JDLZ")) {
                Address addr = data.getAddress();
                println("STRING @ " + addr + " : " + s);
                for (Reference ref : getReferencesTo(addr)) {
                    Address fromAddr = ref.getFromAddress();
                    Function func = currentProgram.getFunctionManager().getFunctionContaining(fromAddr);
                    if (func != null) {
                        println("  referenced by function: " + func.getName() + " @ " + func.getEntryPoint());
                        targetFuncs.add(func);
                    }
                }
            }
        }

        if (targetFuncs.isEmpty()) {
            println("No functions found referencing HUFF/JDLZ strings directly.");
        }

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        ConsoleTaskMonitor monitor = new ConsoleTaskMonitor();

        for (Function func : targetFuncs) {
            println("\n\n##### DECOMPILE: " + func.getName() + " @ " + func.getEntryPoint() + " #####");
            DecompileResults res = decomp.decompileFunction(func, 60, monitor);
            if (res.decompileCompleted()) {
                println(res.getDecompiledFunction().getC());
            } else {
                println("decompile failed: " + res.getErrorMessage());
            }
        }
    }
}
