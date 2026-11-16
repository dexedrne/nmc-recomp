# Gummi ship removal (owner: "not even in the game")

Findings (batch B1; low confidence where stated; needs a play-through test):
- `0x151a50` / `0x151980` are the Init / Exit callbacks of the gummi task descriptor at RVA 0x716c10 (stored by 0x1519f0, called once
  from boot function 0x14ed40). EVERY start of the gummi task funnels through `GAME_MODE::vfn1` at 0x1505a0 with rcx = 0x716c10: that is the
  robust host-side gate. The specific world-map function that decides to enter gummi was not isolated.
- `m_51` in `wm00.ard` = 9 copies of WM_GUMI_01 (objentry id 942, the route nodes); the spawn-type byte 1..9 is the course id.
- Data edit 1: remove the `Spawn('m_51')` op from 14 of the 15 `map` programs in wm00.ard (program 25 already omits it); keep the m_00, m_01,
  m_50, m_51 and evt subfiles (reparse round-trips). Tool: `tools/gummi/make_gummi_removal.py`.
- Data edit 2: `00progress.bin` WLDF id 53 cmd u16 5 -> 14 at file offset 36636 (world index 2 is Twilight Town). One byte.
- Course state is read by no LINK or script condition in 00progress, so no flag needs to mirror it. The final route to The World That
  Never Was is opened by flag-driven WLDF id 53 regardless of gummi. Lost: the gummi stage's arrival event and goal BGM. No hand-set flags
  or item grants were identified; flags raised inside the gummi overlay/GBX data were not decoded (play-through needed).
- Not done: the gummi menu/garage entry (separate task/mode; camp-menu entry not located). In the port, gate it host-side at 0x1505a0.
