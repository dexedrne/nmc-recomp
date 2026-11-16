// Name DB v2 import for kh2fm.exe (image base 0x140000000), Ghidra headless.
// Creates every function of the canonical universe with its MERGED extent (one contiguous range), marks data-in-code ranges as data
// BEFORE disassembling (so jump tables are never decoded as code), disassembles each function body linearly, renames functions,
// creates GhidraClass namespaces for 'Class::method' names, sets plate comments (tier/confidence/source/evidence), labels vtables and
// Panacea data labels. Writes a JSON report with accepted/rejected counts.
//
// Usage (headless, no auto-analysis):
//   analyzeHeadless <projdir> <proj> -import kh2fm.exe -noanalysis -scriptPath <dir with this file> \
//       -postScript GhidraImportNameDbV2.java <name_db_v2.csv> <data_in_code_v2.csv> <vtables_v2.csv> <data_labels.csv> <report.json> [maxRows]
//@category KHNMC
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import ghidra.util.exception.*;
import java.io.*;
import java.util.*;

public class GhidraImportNameDbV2 extends GhidraScript {
    static final long BASE = 0x140000000L;

    static List<String> parseCsvLine(String line) {
        List<String> out = new ArrayList<>();
        StringBuilder sb = new StringBuilder();
        boolean q = false;
        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (q) {
                if (c == '"') {
                    if (i + 1 < line.length() && line.charAt(i + 1) == '"') { sb.append('"'); i++; }
                    else q = false;
                } else sb.append(c);
            } else if (c == '"') q = true;
            else if (c == ',') { out.add(sb.toString()); sb.setLength(0); }
            else sb.append(c);
        }
        out.add(sb.toString());
        return out;
    }

    static List<List<String>> readCsv(String path) throws IOException {
        List<List<String>> rows = new ArrayList<>();
        try (BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(path), "UTF-8"))) {
            String l;
            StringBuilder cur = null;
            while ((l = br.readLine()) != null) {
                if (cur != null) { cur.append('\n').append(l); l = cur.toString(); cur = null; }
                int quotes = 0;
                for (int i = 0; i < l.length(); i++) if (l.charAt(i) == '"') quotes++;
                if (quotes % 2 == 1) { cur = new StringBuilder(l); continue; }
                rows.add(parseCsvLine(l));
            }
        }
        return rows;
    }

    static String sanitize(String s) {
        return s.replaceAll("\\s+", "_").replace("\"", "'");
    }

    // split on '::' at template depth 0
    static List<String> splitNs(String s) {
        List<String> parts = new ArrayList<>();
        int depth = 0, last = 0;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '<') depth++;
            else if (c == '>') depth = Math.max(0, depth - 1);
            else if (c == ':' && depth == 0 && i + 1 < s.length() && s.charAt(i + 1) == ':') {
                parts.add(s.substring(last, i));
                last = i + 2;
                i++;
            }
        }
        parts.add(s.substring(last));
        return parts;
    }

    Namespace getOrMakeClass(String qualified, SymbolTable st, Map<String, Namespace> cache, int[] nclasses) throws Exception {
        Namespace hit = cache.get(qualified);
        if (hit != null) return hit;
        Namespace parent = currentProgram.getGlobalNamespace();
        StringBuilder path = new StringBuilder();
        List<String> parts = splitNs(qualified);
        for (int i = 0; i < parts.size(); i++) {
            String p = sanitize(parts.get(i));
            if (p.isEmpty()) continue;
            if (path.length() > 0) path.append("::");
            path.append(parts.get(i));
            Namespace c = cache.get(path.toString());
            if (c == null) {
                Namespace ex = st.getNamespace(p, parent);
                boolean last = (i == parts.size() - 1);
                if (ex == null) {
                    ex = last ? st.createClass(parent, p, SourceType.IMPORTED) : st.getOrCreateNameSpace(parent, p, SourceType.IMPORTED);
                    if (last) nclasses[0]++;
                } else if (last && !(ex instanceof GhidraClass)) {
                    try { ex = st.convertNamespaceToClass(ex); nclasses[0]++; } catch (Exception e) { /* keep namespace */ }
                }
                c = ex;
                cache.put(path.toString(), c);
            }
            parent = c;
        }
        return parent;
    }

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        if (a.length < 5) { printerr("args: name_db_v2.csv data_in_code_v2.csv vtables_v2.csv data_labels.csv report.json [maxRows]"); return; }
        int maxRows = a.length > 5 ? Integer.parseInt(a[5]) : Integer.MAX_VALUE;
        long ib = currentProgram.getImageBase().getOffset();
        if (ib != BASE) { printerr("unexpected image base " + Long.toHexString(ib)); return; }
        AddressFactory af = currentProgram.getAddressFactory();
        FunctionManager fm = currentProgram.getFunctionManager();
        SymbolTable st = currentProgram.getSymbolTable();
        Listing lst = currentProgram.getListing();
        Map<String, Integer> rep = new LinkedHashMap<>();
        Map<String, Namespace> cache = new HashMap<>();
        int[] ncls = {0};
        List<String> problems = new ArrayList<>();

        // ---- 1. data-in-code first
        int dic = 0, dicFail = 0;
        for (List<String> r : readCsv(a[1])) {
            if (r.get(0).equals("start_rva")) continue;
            try {
                long s = Long.decode(r.get(0)), e = Long.decode(r.get(1));
                Address sa = af.getDefaultAddressSpace().getAddress(BASE + s);
                int len = (int) (e - s);
                lst.clearCodeUnits(sa, sa.add(len - 1), false);
                lst.createData(sa, new ArrayDataType(ByteDataType.dataType, len, 1), len);
                dic++;
            } catch (Exception ex) { dicFail++; if (problems.size() < 20) problems.add("data-in-code " + r + ": " + ex.getMessage()); }
        }
        rep.put("data_in_code_ranges_defined", dic);
        rep.put("data_in_code_ranges_failed", dicFail);

        // ---- 2. functions
        List<List<String>> rows = readCsv(a[0]);
        List<String> head = rows.get(0);
        int iRva = head.indexOf("rva"), iEnd = head.indexOf("end"), iCls = head.indexOf("class"), iName = head.indexOf("name"), iSub = head.indexOf("subsystem"),
            iConf = head.indexOf("confidence"), iTier = head.indexOf("tier"), iSrc = head.indexOf("source"), iEv = head.indexOf("evidence"), iKind = head.indexOf("kind"), iId = head.indexOf("id");
        int removedFrag = 0, created = 0, existed = 0, createFail = 0, renamed = 0, renameFail = 0, commented = 0, withClass = 0, disasmFail = 0, total = 0, accepted = 0;
        monitor.setMessage("importing functions");
        for (int ri = 1; ri < rows.size() && total < maxRows; ri++) {
            List<String> r = rows.get(ri);
            if (r.size() < head.size()) continue;
            total++;
            long rva = Long.decode(r.get(iRva)), end = Long.decode(r.get(iEnd));
            Address entry = af.getDefaultAddressSpace().getAddress(BASE + rva);
            Address last = af.getDefaultAddressSpace().getAddress(BASE + end - 1);
            AddressSet body = new AddressSet(entry, last);
            String name = r.get(iName);
            // the PE loader creates one function per RUNTIME_FUNCTION, i.e. chained fragments become separate functions inside our merged extent: remove them
            List<Address> frag = new ArrayList<>();
            for (Function g : fm.getFunctions(body, true)) if (!g.getEntryPoint().equals(entry)) frag.add(g.getEntryPoint());
            for (Address fa : frag) { fm.removeFunction(fa); removedFrag++; }
            Function f = fm.getFunctionAt(entry);
            boolean ok = false;
            if (f != null) {
                existed++;
                try { f.setBody(body); ok = true; } catch (Exception ex) { createFail++; if (problems.size() < 20) problems.add("setBody " + r.get(iId) + " " + ex.getMessage()); }
            } else {
                // disassemble the non-data part of the body linearly, then create the function with the merged body
                try {
                    Address cur = entry;
                    int guard = 0;
                    while (cur != null && cur.compareTo(last) <= 0 && guard++ < 100000) {
                        CodeUnit cu = lst.getCodeUnitContaining(cur);
                        if (cu instanceof Instruction) { cur = cu.getMaxAddress().next(); continue; }
                        if (cu instanceof Data && ((Data) cu).isDefined()) { cur = cu.getMaxAddress().next(); continue; }
                        DisassembleCommand dc = new DisassembleCommand(new AddressSet(cur, cur), body, true);
                        if (!dc.applyTo(currentProgram, monitor)) { disasmFail++; cur = cur.next(); continue; }
                        CodeUnit n = lst.getCodeUnitContaining(cur);
                        cur = (n == null ? cur.next() : n.getMaxAddress().next());
                    }
                } catch (Exception ex) { disasmFail++; }
                try {
                    f = fm.createFunction(null, entry, body, SourceType.DEFAULT);
                    created++;
                    ok = f != null;
                } catch (Exception ex) {
                    createFail++;
                    if (problems.size() < 20) problems.add("createFunction " + r.get(iId) + " " + r.get(iRva) + ": " + ex.getMessage());
                }
            }
            if (!ok || f == null) continue;
            accepted++;
            if (!name.isEmpty()) {
                try {
                    String cls = r.get(iCls);
                    if (!cls.isEmpty() && name.startsWith(cls + "::")) {
                        Namespace ns = getOrMakeClass(cls, st, cache, ncls);
                        String leaf = sanitize(name.substring(cls.length() + 2));
                        f.setParentNamespace(ns);
                        f.setName(leaf.isEmpty() ? "fn" : leaf, SourceType.IMPORTED);
                        withClass++;
                    } else {
                        List<String> parts = splitNs(name);
                        if (parts.size() > 1) {
                            String nsq = name.substring(0, name.length() - parts.get(parts.size() - 1).length() - 2);
                            Namespace ns = getOrMakeClass(nsq, st, cache, ncls);
                            f.setParentNamespace(ns);
                            f.setName(sanitize(parts.get(parts.size() - 1)), SourceType.IMPORTED);
                        } else f.setName(sanitize(name), SourceType.IMPORTED);
                    }
                    renamed++;
                } catch (Exception ex) {
                    renameFail++;
                    if (problems.size() < 20) problems.add("rename " + r.get(iId) + " '" + name + "': " + ex.getMessage());
                }
            }
            String ev = r.get(iEv);
            String cmt = "name_db_v2 " + r.get(iId) + " [" + r.get(iTier) + "/" + r.get(iConf) + "/" + r.get(iSrc) + "] kind=" + r.get(iKind) + " subsystem=" + r.get(iSub)
                + (ev.isEmpty() ? "" : "\n" + ev);
            if (!name.isEmpty() || !r.get(iSub).isEmpty()) {
                lst.setComment(entry, CodeUnit.PLATE_COMMENT, cmt);
                commented++;
            }
            if (total % 4000 == 0) println("rows " + total + " created " + created + " renamed " + renamed);
        }
        rep.put("rows", total);
        rep.put("functions_accepted", accepted);
        rep.put("loader_fragment_functions_removed", removedFrag);
        rep.put("functions_created", created);
        rep.put("functions_already_present_body_reset", existed);
        rep.put("function_create_or_body_failures", createFail);
        rep.put("disassemble_failures", disasmFail);
        rep.put("renamed", renamed);
        rep.put("rename_failures", renameFail);
        rep.put("with_class_namespace", withClass);
        rep.put("classes_created", ncls[0]);
        rep.put("plate_comments", commented);

        // ---- 3. vtable labels
        int vt = 0, vtFail = 0;
        if (new File(a[2]).exists()) {
            for (List<String> r : readCsv(a[2])) {
                if (r.get(0).equals("vtable_rva")) continue;
                try {
                    long v = Long.decode(r.get(0));
                    int n = Integer.parseInt(r.get(3));
                    Address va = af.getDefaultAddressSpace().getAddress(BASE + v);
                    Namespace ns = getOrMakeClass(r.get(1).replace("_base", "::base"), st, cache, ncls);
                    st.createLabel(va, "vftable", ns, SourceType.IMPORTED);
                    if (n > 0) {
                        lst.clearCodeUnits(va, va.add(8L * n - 1), false);
                        lst.createData(va, new ArrayDataType(PointerDataType.dataType, n, 8), 8 * n);
                    }
                    vt++;
                } catch (Exception ex) { vtFail++; if (problems.size() < 20) problems.add("vtable " + r + ": " + ex.getMessage()); }
            }
        }
        rep.put("vtable_labels", vt);
        rep.put("vtable_label_failures", vtFail);
        int dl = 0;
        if (new File(a[3]).exists()) {
            for (List<String> r : readCsv(a[3])) {
                if (r.get(0).equals("rva")) continue;
                try {
                    Address da = af.getDefaultAddressSpace().getAddress(BASE + Long.decode(r.get(0)));
                    st.createLabel(da, sanitize(r.get(1).replace("::", "_")), SourceType.IMPORTED);
                    dl++;
                } catch (Exception ex) { /* ignore */ }
            }
        }
        rep.put("data_labels", dl);
        rep.put("functions_in_program_after", fm.getFunctionCount());
        Set<Long> inCsv = new HashSet<>();
        for (int ri = 1; ri < rows.size(); ri++) if (rows.get(ri).size() >= head.size()) inCsv.add(Long.decode(rows.get(ri).get(iRva)));
        int notInUniverse = 0;
        StringBuilder sample = new StringBuilder();
        Map<String, Integer> blocks = new TreeMap<>();
        for (Function g : fm.getFunctions(true)) {
            long off = g.getEntryPoint().getOffset() - BASE;
            if (!inCsv.contains(off)) {
                notInUniverse++;
                ghidra.program.model.mem.MemoryBlock mb = currentProgram.getMemory().getBlock(g.getEntryPoint());
                String bn = mb == null ? "?" : mb.getName();
                blocks.merge(bn, 1, Integer::sum);
                if (notInUniverse <= 8) sample.append("0x").append(Long.toHexString(off)).append(' ').append(g.getName()).append(" [").append(bn).append("]; ");
            }
        }
        rep.put("functions_not_in_universe_after", notInUniverse);
        println("functions not in universe by block: " + blocks + " sample: " + sample);
        problems.add("not-in-universe by block " + blocks + " sample " + sample);
        int outside = 0;
        for (Function g : fm.getFunctions(true)) if (g.getBody().getNumAddressRanges() != 1) outside++;
        rep.put("functions_with_more_than_one_range_after", outside);

        try (PrintWriter pw = new PrintWriter(new FileWriter(a[4]))) {
            pw.println("{");
            int k = 0;
            for (Map.Entry<String, Integer> e : rep.entrySet()) pw.println("  \"" + e.getKey() + "\": " + e.getValue() + (++k < rep.size() ? "," : ","));
            pw.println("  \"problems\": [");
            for (int i = 0; i < problems.size(); i++) pw.println("    \"" + problems.get(i).replace("\\", "\\\\").replace("\"", "'") + "\"" + (i + 1 < problems.size() ? "," : ""));
            pw.println("  ]\n}");
        }
        println("report: " + rep);
    }
}
