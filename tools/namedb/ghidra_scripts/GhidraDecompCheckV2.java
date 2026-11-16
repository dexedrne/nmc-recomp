// Decompiler sanity check after GhidraImportNameDbV2: decompiles a few known functions and reports size, callee names seen,
// bad-data markers, and whether the function body equals the merged extent.
// Usage: -postScript GhidraDecompCheckV2.java <out.json> <rva:expectedCalleeSubstring[|substr2]> ...
//@category KHNMC
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.util.*;

public class GhidraDecompCheckV2 extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        long base = currentProgram.getImageBase().getOffset();
        StringBuilder sb = new StringBuilder("[\n");
        for (int i = 1; i < a.length; i++) {
            String[] kv = a[i].split(":", 2);
            long rva = Long.decode(kv[0]);
            String[] expect = kv.length > 1 ? kv[1].split("\\|") : new String[0];
            Address ad = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(base + rva);
            Function f = getFunctionAt(ad);
            sb.append("  {\"rva\": \"0x").append(Long.toHexString(rva)).append("\"");
            if (f == null) { sb.append(", \"error\": \"no function\"},\n"); continue; }
            long t0 = System.currentTimeMillis();
            DecompileResults dr = di.decompileFunction(f, 120, monitor);
            long ms = System.currentTimeMillis() - t0;
            boolean ok = dr != null && dr.decompileCompleted();
            String c = ok ? dr.getDecompiledFunction().getC() : "";
            int lines = c.isEmpty() ? 0 : c.split("\n").length;
            List<String> seen = new ArrayList<>();
            for (String e : expect) if (c.contains(e)) seen.add(e);
            sb.append(", \"name\": \"").append(f.getName(true).replace("\"", "'")).append("\"");
            sb.append(", \"body_bytes\": ").append(f.getBody().getNumAddresses());
            sb.append(", \"completed\": ").append(ok);
            sb.append(", \"c_lines\": ").append(lines);
            sb.append(", \"c_chars\": ").append(c.length());
            sb.append(", \"calls_in_c\": ").append(c.split("\\w+\\(").length - 1);
            sb.append(", \"bad_data_markers\": ").append(c.contains("halt_baddata") || c.contains("BAD") ? 1 : 0);
            sb.append(", \"unrecovered_jumptable\": ").append(c.contains("Could not recover jumptable") ? 1 : 0);
            sb.append(", \"expected_substrings\": ").append(expect.length).append(", \"expected_found\": ").append(seen.size());
            sb.append(", \"millis\": ").append(ms);
            sb.append(", \"error\": \"").append(ok ? "" : (dr == null ? "null" : dr.getErrorMessage().replace("\"", "'"))).append("\"");
            sb.append(", \"head\": \"").append(c.length() > 600 ? c.substring(0, 600).replace("\\", "\\\\").replace("\"", "'").replace("\n", "\\n") : c.replace("\\", "\\\\").replace("\"", "'").replace("\n", "\\n")).append("\"},\n");
        }
        sb.append("  {}\n]\n");
        try (PrintWriter pw = new PrintWriter(new FileWriter(a[0]))) { pw.print(sb); }
        println("decomp check written");
    }
}
