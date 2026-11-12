import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import java.io.PrintWriter;

public class DumpRenderImage extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = System.getenv("DECOMPILE_OUT");
        if (outPath == null) outPath = "dump_out.txt";
        PrintWriter out = new PrintWriter(outPath, "UTF-8");

        Address start = currentProgram.getAddressFactory().getAddress("80149198");
        Function f = currentProgram.getFunctionManager().getFunctionContaining(start);
        out.println("Function: " + (f == null ? "NOT FOUND" : (f.getName() + " body " + f.getBody())));

        // Raw bytes around 801491a0, +/- 0x40
        Address dumpStart = start.subtract(0x10);
        Address dumpEnd = start.add(0x80);
        Memory mem = currentProgram.getMemory();
        out.println("\n=== RAW BYTES " + dumpStart + " to " + dumpEnd + " ===");
        Address a = dumpStart;
        StringBuilder line = new StringBuilder();
        int col = 0;
        while (a.compareTo(dumpEnd) <= 0) {
            byte b = mem.getByte(a);
            line.append(String.format("%02x ", b & 0xff));
            col++;
            if (col == 16) {
                out.println(a.subtract(15) + ": " + line);
                line.setLength(0);
                col = 0;
            }
            a = a.add(1);
        }
        if (col > 0) out.println(a.subtract(col) + ": " + line);

        out.println("\n=== EXISTING INSTRUCTIONS (disassembled so far) ===");
        InstructionIterator it = currentProgram.getListing().getInstructions(dumpStart, true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (ins.getAddress().compareTo(dumpEnd) > 0) break;
            out.println(ins.getAddress() + ":  " + ins.toString() + "   bytes=" + ins.toString());
        }

        out.flush();
        out.close();
        println("Wrote " + outPath);
    }
}
