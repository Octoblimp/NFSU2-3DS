import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import java.io.PrintWriter;

// Env-driven disassembly dump, so a range can be pulled without editing a script.
//   ASM_RANGES  comma-separated "start-end" or "start+len" hex pairs, e.g. "0x4ce3c0-0x4ce4a0"
//   ASM_OUT     output file (stdout if unset)
public class DumpAsmRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        String ranges = System.getenv("ASM_RANGES");
        String out = System.getenv("ASM_OUT");
        if (ranges == null || ranges.isEmpty()) { println("ASM_RANGES not set"); return; }
        StringBuilder sb = new StringBuilder();
        for (String spec : ranges.split(",")) {
            spec = spec.trim();
            if (spec.isEmpty()) continue;
            long start, end;
            int plus = spec.indexOf('+');
            if (plus > 0) {
                start = Long.decode(spec.substring(0, plus).trim());
                end = start + Long.decode(spec.substring(plus + 1).trim());
            } else {
                int dash = spec.lastIndexOf('-');
                start = Long.decode(spec.substring(0, dash).trim());
                end = Long.decode(spec.substring(dash + 1).trim());
            }
            Address a = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(start);
            Address e = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(end);
            sb.append("\n===== ").append(a).append(" .. ").append(e).append(" =====\n");
            InstructionIterator it = currentProgram.getListing().getInstructions(a, true);
            while (it.hasNext()) {
                Instruction ins = it.next();
                if (ins.getAddress().compareTo(e) > 0) break;
                sb.append(ins.getAddress()).append(":  ").append(ins.toString()).append("\n");
            }
        }
        if (out == null || out.isEmpty()) { println(sb.toString()); }
        else { PrintWriter pw = new PrintWriter(out); pw.print(sb); pw.close(); println("Wrote " + out); }
    }
}
