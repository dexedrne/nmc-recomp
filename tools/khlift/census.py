"""VEX census over the universe: which ops/exprs/stmts/ccalls/dirty helpers/jumpkinds occur."""
import sys, collections, json, multiprocessing as mp
import pyvex, archinfo
from khlift import universe as U, BASE
ARCH = archinfo.ArchAMD64()
IMG = U.image()
FS = U.load()
def work(i):
    f = FS[i]
    C = collections.Counter(); first = {}
    if f.nep: return i, C, first
    ranges = [(f.lo, f.hi)]
    def inf(a): return f.lo <= a < f.hi
    work = [f.lo]; done = set()
    while work:
        a = work.pop()
        if a in done or not inf(a): continue
        done.add(a)
        data = IMG[a - BASE:f.hi - BASE]
        try: irsb = pyvex.lift(data, a, ARCH, max_bytes=len(data), opt_level=1)
        except Exception as e:
            C['LIFTERR:' + type(e).__name__] += 1; first.setdefault('LIFTERR:' + type(e).__name__, f.rva); continue
        def note(k):
            C[k] += 1; first.setdefault(k, f.rva)
        for ty in irsb.tyenv.types: note('ty:' + ty)
        def walk(e):
            note('ex:' + e.tag)
            if e.tag in ('Iex_Unop', 'Iex_Binop', 'Iex_Triop', 'Iex_Qop'): note('op:' + e.op)
            if e.tag == 'Iex_CCall': note('ccall:' + e.cee.name)
            for ch in getattr(e, 'args', []) or []:
                walk(ch)
            for nm in ('addr', 'cond', 'iftrue', 'iffalse'):
                if hasattr(e, nm): walk(getattr(e, nm))
        for s in irsb.statements:
            note('st:' + s.tag)
            if s.tag == 'Ist_Dirty': note('dirty:' + s.cee.name)
            for nm in ('data', 'addr', 'guard', 'expd', 'dataLo', 'dataHi', 'expdLo', 'expdHi'):
                e = getattr(s, nm, None)
                if e is not None and hasattr(e, 'tag'): walk(e)
            if s.tag == 'Ist_Dirty':
                for e in s.args: walk(e)
            if s.tag == 'Ist_Exit':
                note('exitjk:' + s.jk)
                if inf(s.dst.value): work.append(s.dst.value)
        note('jk:' + irsb.jumpkind)
        nx = irsb.next
        if nx.tag == 'Iex_Const':
            if inf(nx.con.value): work.append(nx.con.value)
        else: walk(nx)
        if irsb.jumpkind == 'Ijk_Call':
            last = 0
            for s in irsb.statements:
                if s.tag == 'Ist_IMark': last = s.addr + s.len
            work.append(last)
        # jump tables
        last = 0
        for s in irsb.statements:
            if s.tag == 'Ist_IMark': last = s.addr + s.delta
        for t in f.jtabs.get(last, []): work.append(t)
    return i, C, first
if __name__ == '__main__':
    tot = collections.Counter(); first = {}
    with mp.Pool(int(sys.argv[1]) if len(sys.argv) > 1 else 6) as p:
        for i, C, fi in p.imap_unordered(work, range(len(FS)), chunksize=64):
            tot.update(C)
            for k, v in fi.items(): first.setdefault(k, v)
    json.dump({'count': tot, 'first': {k: hex(v) for k, v in first.items()}}, open('../work/census.json', 'w'), indent=0, sort_keys=True)
    for k in sorted(tot): print(k, tot[k], hex(first[k]))
