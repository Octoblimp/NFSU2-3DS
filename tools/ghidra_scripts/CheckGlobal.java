import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;

public class CheckGlobal extends GhidraScript {
    @Override
    public void run() throws Exception {
        Address a = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(0x008650f8L);
        byte b = currentProgram.getMemory().getByte(a);
        println("DAT_008650f8 byte value = " + (b & 0xFF));
        Data d = getDataAt(a);
        println("Data at that addr: " + (d == null ? "null" : d.toString()));
        // dump surrounding bytes
        byte[] buf = new byte[16];
        currentProgram.getMemory().getBytes(a, buf);
        StringBuilder sb = new StringBuilder();
        for (byte x : buf) sb.append(String.format("%02x ", x & 0xFF));
        println(sb.toString());
    }
}
