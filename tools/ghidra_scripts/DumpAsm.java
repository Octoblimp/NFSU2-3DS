import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;

public class DumpAsm extends GhidraScript {
    @Override
    public void run() throws Exception {
        // Dump disassembly of FUN_005744c0 (HUFF entry / call site) and the first ~40
        // instructions of FUN_00573840 (the real decoder), to nail down calling convention.
        long[] starts = { 0x00573840L };
        for (long s : starts) {
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(s);
            Function f = currentProgram.getFunctionManager().getFunctionContaining(addr);
            Address end = f != null ? f.getBody().getMaxAddress() : addr.add(0x1000);
            println("\n\n===== DISASM of " + (f == null ? "?" : f.getName()) + " @ " + addr + " to " + end + " =====");
            InstructionIterator it = currentProgram.getListing().getInstructions(addr, true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                if (ins.getAddress().compareTo(end) > 0) break;
                println(ins.getAddress() + ":  " + ins.toString());
            }
        }

        // Also dump the module's preferred image base and .text-ish section layout
        println("\n\n===== IMAGE BASE =====");
        println("image base: " + currentProgram.getImageBase());
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            println(b.getName() + " start=" + b.getStart() + " end=" + b.getEnd()
                + " size=0x" + Long.toHexString(b.getSize())
                + " initialized=" + b.isInitialized() + " exec=" + b.isExecute());
        }
    }
}
