import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

public class DumpRegion extends GhidraScript {
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        Address start = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(0x00573680L);
        Address end = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(0x00574700L);
        long len = end.subtract(start);
        byte[] buf = new byte[(int) len];
        mem.getBytes(start, buf);
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < buf.length; i += 16) {
            sb.setLength(0);
            sb.append(String.format("%08x: ", start.getOffset() + i));
            StringBuilder ascii = new StringBuilder();
            for (int j = 0; j < 16 && i + j < buf.length; j++) {
                int b = buf[i + j] & 0xFF;
                sb.append(String.format("%02x ", b));
                ascii.append((b >= 32 && b < 127) ? (char) b : '.');
            }
            println(sb.toString() + " " + ascii.toString());
        }
    }
}
