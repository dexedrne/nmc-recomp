# T100 pack: the 0.1.0 files the 334-file overlay never carried

`pack/` is the complete private overlay: all 334 files of `/home/user/work/khnmc-packs/rmc` byte for byte, plus 64 files
(398 total). Point `KHNMC_RMC_DIR` at its absolute path on the port; for the PC zip use `RMC_PACK=<this pack>` with
`rmc-zip/build_layer.py` (every file is a plain copy at its game path).

| Added | Files | Source |
|---|---:|---|
| Marluxia story arena: map/ca26.map, map/us/ca26.bar, remastered/map/ca26.map/ (12), remastered/map/us/ca26.bar/ (1) | 15 | R01: hb38 donors, digests pinned |
| Larxene story arena: map/nm11.map, map/us/nm11.bar, remastered/map/nm11.map/ (41), remastered/map/us/nm11.bar/ (1) | 44 | R03: nm07 donors, digests pinned |
| ard/us/ca26.ard, ard/us/nm11.ard | 2 | R04: the intro scenes bind the staff and shield props directly |
| ard/us/tr01.ard | 1 | R27: the two unresolved partner placements removed (Space Paranoids half of the Day 96 fix) |
| msg/us/sys.bar, msg/us/sys.text | 2 | items + R08: Less Than Nothing preview text, Battle Items hints (3 messages) |

Rebuild into a fresh directory (read-only inputs: the public PC loose files, the current overlay, the published recipes):

```sh
python3 -B tools/build_t100.py data-full /home/user/work/khnmc-packs/rmc /home/user/work/rmc/roxas-may-cry pack-new
python3 -B run-T100/check-pack.py pack-new        # 334 inherited identical, 64 added equal to the 0.1.0 build
```

The builder pins every input digest, refuses a destination the overlay or the game data already has (an earlier repair is
never replaced; applying it to its own output stops at `the overlay already carries map/ca26.map`), and publishes the pack
only when all 64 files are built. The five modified files are built from the public mod's copies (the same bytes as
`data-full`); if the PC's installed copies of ard/us/ca26.ard, nm11.ard, tr01.ard or msg/us/sys.bar differ from those, the
pack's copies still replace them whole. Results and the route evidence: [STATUS.md](STATUS.md).
