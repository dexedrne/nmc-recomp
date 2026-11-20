"""Check controller choice, refill, protected lifetime, menu edit and native expiry."""
from pathlib import Path
import re
import sys

route = Path(sys.argv[1])
form, tag = map(int,sys.argv[2:])
log = (route/'r01/run.out').read_text(errors='replace')
def require(condition, reason):
    if not condition:
        raise SystemExit(f'{route.name}: {reason}')
require('\tok\t' in (route/'summary.tsv').read_text(), 'route failed')
command={2:7,3:673,5:12}[form]
require(re.search(rf'@select command-id {command} timeout 120` met',log), 'native menu choice missing')
if tag:
    require('room=12:09 look=xion' in log,'Xion appearance inactive in the field')
    require(re.search(r'xkb-card-read: .* XKB '+str(tag)+r'\b',log), 'campaign marker missing')
spent=re.search(r'T91 spent two bars frame=(\d+) before=7 after=5',log)
require(spent,'native Drive charge missing')
require(f'T91 frame={int(spent[1])+1} form=0 mode=1 drive=7/7' in log,'next frame did not refill Drive')
require('kh: RMC KHNMC_CHEAT_INF_DRIVE=0' in log,'menu did not disable Infinite Drive')
require('env KHNMC_CHEAT_INF_DRIVE=0' in (route/'docs/khnmc.conf').read_text(), 'toggle did not persist')
def samples(s):
    return [(int(f),int(i),int(m),float(t),float(mx)) for f,i,m,t,mx in
            re.findall(r'T91 frame=(\d+) form=(\d+) mode=(\d+) drive=\d+/\d+ timer=([\d.]+)/([\d.]+)',s)]
before,after=map(samples,log.split('kh: RMC KHNMC_CHEAT_INF_DRIVE=0',1))
held=[r for r in before if r[1]==form and r[2]==2]
require(held and all(r[3]==r[4]>0 for r in held),'active form timer was not full')
require(all(r[1]==form for r in before if r[0]>=held[0][0]),'early revert')
draining=[r for r in after if r[1]==form and r[2]==2 and 0<r[3]<r[4]]
require(len(draining)>=2 and draining[-1][3]<draining[0][3],'timer did not drain after the menu edit')
revert=next((r for r in after if r[0]>draining[-1][0] and r[1]==0 and r[2]==1),None)
require(revert,'native reversion missing')
held_frames=held[-1][0]-held[0][0]
require(held_frames>revert[0]-held[-1][0], 'protected observation shorter than unprotected lifetime')
print(f'{route.name}: 7→5→7 bars, form {form} held at {held[0][4]:g} for at least {held_frames} frames, then drained to {draining[-1][3]:g} and returned to base at {revert[0]}.')
