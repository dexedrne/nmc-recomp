#!/usr/bin/env python3
"""Keep archive relocations on the hooks, and alias the untouched lifted bodies."""
import pathlib
import subprocess
import sys
import tempfile

source, output, *hooks = sys.argv[1:]
hooks = {'f_' + rva for rva in hooks}
subprocess.run(['objcopy', *('--weaken-symbol=' + h for h in sorted(hooks)), source, output], check=True)
with tempfile.TemporaryDirectory(prefix='khnmc-alias-') as tmp:
    member = None
    aliases = {}
    for line in subprocess.check_output(['objdump', '-t', source], text=True).splitlines():
        if ':     file format ' in line:
            member = line.split(':', 1)[0]
        fields = line.split()
        if len(fields) == 6 and fields[-1] in hooks and fields[3] != '*UND*':
            aliases.setdefault(member, []).append((fields[-1], fields[3], fields[0]))
    assert {name for group in aliases.values() for name, _, _ in group} == hooks
    for member, group in aliases.items():
        path = pathlib.Path(tmp) / member
        path.write_bytes(subprocess.check_output(['ar', 'p', source, member]))
        args = ['objcopy']
        for name, section, offset in group:
            args += ['--weaken-symbol=' + name,
                     '--add-symbol=orig_' + name + '=' + section + ':0x' + offset + ',global,function']
        subprocess.run([*args, str(path)], check=True)
        subprocess.run(['ar', 'r', output, str(path)], check=True)
