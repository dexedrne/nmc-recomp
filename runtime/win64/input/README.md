Set `KHNMC_INPUT_SCRIPT=$PWD/input/advance.txt` to connect virtual XInput pad 0.
Without it, all pads remain disconnected. Script loading happens at startup.

```
START_FRAME END_FRAME BUTTONS [lx ly]
```

Frames are inclusive present counts (zero before the first Present). Gaps release
all buttons and centre both sticks; later lines override earlier overlapping
ranges. Blank lines and `#` comments are accepted. Invalid input is a named fatal.

Buttons: `A B X Y START BACK LB RB DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT
LSTICK RSTICK LT RT`, joined with `+`, or `-` for none. Optional left-stick
coordinates are signed integers from -32768 through 32767. Right stick stays
centred. Triggers send 255; the engine applies its own digital threshold and
stick shaping. A packet number changes only when a sampled state changes.

`advance-vkd3d.txt` is the same route for `KHNMC_GFX=vkd3d` (A at 200, Yes at 300, A at 360): the CPU renderer
presents fewer frames per second while loading takes the same wall-clock time, so the page appears at a lower
present count. Both scripts are plain present counts: on a heavily loaded host the page can appear after the
first scripted press (seen on the null backend at load average 25: page at present 184, press at 180-182).

`advance.txt` (the title route) confirms at frames 90, 180 and 270 and selects Yes at 240.
It stops after 272, before the title task, to avoid starting a new game.
Use a fresh `KH_DOCS` directory to reproduce the first-launch sequence;
completed Initial Settings writes configuration through the game's save code.

Every 60 presents, `KHNMC_PHASELOG` (default `phase.log`, append mode) records
render-counter deltas, first-seen CFileMan request names and eleven entry counters.
`initial-state` and `choice` are the last observed Initial Settings values.
Native archive aliases extend the existing link hooks to same-shard calls;
`lifted/` and `lifter-rt/` are untouched. File names repeat in `KHNMC_FILELOG`
when the engine asks again; the phase log lists each name only once per run.

`field-battle-d18.txt` (task 18) is the field route of `runtime/task6-field-input.txt` plus battle input: the same presses up to
script frame 2790 EXCEPT the one at frame 90 (it only did nothing because the Initial Settings page used to appear after present
92; with `KHNMC_CLOCK=frames` boot presents are fewer, the press opened the confirm dialog early and the route looped in the
settings menu), then from script frame 2800 on an attack every 20 game frames, lock-on every 300, a jump every 400 and the
reaction command in between. It is clocked by game frames (`@clock game-frames`), so it is the same on any backend and at any
host load; the field comes up near script frame 2600..2850.

## Route scripts: following the game's state (task 27)

A frame-numbered script breaks whenever boot or menu timing changes (host speed, load, renderer, frame time, KH2Moose
`fast_title`): the Mac mini runs of 2026-11-17 04:47 diverged because a press scripted for frame 90 met a settings page that
the Mac had already put up at present 44. A **route** script waits for the game's own state and presses relative to the game
frame in which that state was seen. `input/route-field-battle.txt` is the route (the default of `tools/route_batch.sh` and
`tools/route_one.py`). Everything is counted in game frames (calls of 0x105e10); nothing waits on wall time or presents.

```
@wait COND [timeout N]                         wait until COND holds (checked at the end of every game frame)
@press BUTTONS [lx ly] until COND [every P] [hold H] [timeout N]
                                               press BUTTONS for H of every P game frames (default 2 of 8) until COND
@select title-choice|newgame-choice N [timeout N]
                                               move that menu's cursor to row N with DPAD_UP / DPAD_DOWN, one row per press
@expect COND                                   COND must hold now, else a named fatal (route-expect-failed)
+A +B BUTTONS [lx ly]                          after a step: press during game frames A..B after the frame in which the
                                               step's state was seen (A >= 1; +1 = the next frame; later lines win)
@timeout N                                     time-out (game frames) of the following steps (default 3600)
@difficulty N                                  the route's difficulty (0 Beginner, 1 Standard, 2 Critical, 3 Less Than
                                               Nothing); KHNMC_ROUTE_DIFFICULTY=N overrides it; `difficulty` may be
                                               written wherever a condition takes N
```

COND is `[not] NAME [N]`:

| condition | true when (read where; runtime/route_state.c has the details) |
|---|---|
| `initial-state N` | the Initial Settings state machine is in state N ([this+0x48] at its vfn1 0x5122e0, while the object lives: ctor 0x5111d0 .. dtor 0x511300). 3 = the page takes input, 14 = "Begin with these settings?" takes input |
| `title-screen` | the title waits for a button to open its menu (title task 0x2b4590 running, layout [0x140b1d5d0] at sequence 12/13, mode [0x140b1d5ec] 0, no menu yet, MENU::isRunning [0x1409006b0] 0) |
| `title-menu` | the title menu takes input (menu [0x140b1d5d8] up, mode 0) |
| `title-choice N` | title-menu with its cursor [0x140b1d5e4] on row N (0 = New Game) |
| `newgame-dialog N` | page N of the New Game menu takes input (menu task 0x2b7420 running, title mode 1, context [0x140b1d800] page N and phase 1, layout [[0x140b1d808]+0x614] = 0x1d). 0 = difficulty, 1 = the second question, 2 = the confirmation |
| `newgame-choice N` | a New Game page takes input with its cursor (list [0x140b1d7e0]) on row N. Page 0: 0 Beginner, 1 Standard, 2 Critical, 3 Less Than Nothing |
| `game-level N` | the save's difficulty dword [0x1409abd48] is N (NewGameMenu::commit 0x2b88c0 writes it through GAME_SAVE::SetGameLevel) |
| `field` | the field is up and the player has control: FIELD_COMMAND.execute (0x3f29e0) ran in this game frame, a world is set ([0x140717008] != 0xff) and no message window waits |
| `battle` | a battle is in progress: BATTLE.calc_damage (0x3c12f0) computed a hit in the last 300 game frames |
| `hits N` | N hits computed by BATTLE.calc_damage since the step started (either side's) |
| `message` | a field message window (YS::WINDOW_BASE list [0x142a16310]) waits for a button: state [+0xa0] 2, mode [+0x260] 1 or 2 |
| `frames N` | N game frames since the step started (a delay in game frames) |

A step that does not see its state within its time-out ends the run with a named fatal:
`kh: FATAL: route-wait-timeout: <file> line N `<step>`: <state> never came within T game frames (step k of n, started at game
frame F, now game frame G, present P); game state now: <every state>`. The run's end prints
`kh: route report: k of n steps done; difficulty D; met at game frame: L<line>:<condition>=<frame> ...` and, if the run ended
first, `ROUTE INCOMPLETE ... waiting at line N` (route_batch.sh / route_one.py count that run as DIVERGED). Two steps' presses
are always separated by at least one released frame, so the game sees every press as a new one. `KHNMC_ROUTE_END=exit` ends the
run (exit 0, with all reports) when the last step is done; the default `idle` keeps the pad released until KH_RUN_GAME_FRAMES.
`KHNMC_ROUTE_TRACE=1` prints every state whenever one changes (`kh: route trace game-frame=N ...`): how the values above were
found, and the first thing to look at when a route stops somewhere new (a new host, the iOS Simulator, a device).

The difficulty is chosen in the game's own New Game dialog (page 0; the cursor starts on GAME_SAVE::GetGameLevel = Standard after
InitSave). `KHNMC_DIFFICULTY` still forces the save's dword after InitSave and SetGameLevel (task 7); with a route, the
`@expect game-level difficulty` step then fails loudly if the two disagree. `route_batch.sh -d` and `route_one.py --difficulty`
set KHNMC_ROUTE_DIFFICULTY for a route script and KHNMC_DIFFICULTY for a frame-numbered one.

A file is either frame-numbered (`FROM TO`, `@hold`, `@clock`) or a route; mixing the two is a named fatal at load.

## Route scripts, task 29: walking, the command menu, the player, the map, items

```
@goto X Z [radius R] [until COND] [stall N] [timeout N]
                                               walk the player toward the map point (X, Z) with the left stick until he is
                                               within R (default 100) of it, or until COND holds (e.g. `until not area 2`
                                               for a map exit: the transition starts before the point is reached)
@goto enemy [radius R] [until COND] [stall N] [timeout N]
                                               the same toward the nearest enemy object; met at once when there is none
@select command-row N | command-id N [timeout N]
                                               the field command menu: move the cursor of the list on top of its stack to
                                               row N, or to the row that holds command N (23 Potion, 21 Ether, 3 Items, 1 Attack)
```

`@goto` steers every game frame: heading to the target = atan2(dx, dz) of the player's position [obj+0x670]; the stick angle
atan2(lx, ly) = that heading minus the camera's yaw (eye record+0x48 -> look-at record+0x58 of the camera record 0x19ce40
returns): movement is camera-relative, traced in four directions. Full stick, 0.6 of it within 2R. A walk whose distance has not
shrunk by 10 map units for N game frames (`stall`, default 240) ends the run with the named fatal `route-goto-stalled` (where the
player is, how far, the closest he got, the camera yaw): a wall or a barricade, never a silent wait. `timeout` as everywhere.
There is no path finding: a route walks around walls with intermediate points (input/route-leave-eh02.txt).

| condition | true when (runtime/route_state.c has the details) |
|---|---|
| `hp-below-max`, `mp-below-max` | the field player's HP / MP is below its maximum (his status sheet: FIELD_COMMAND [0x142a10620+0xae0] -> [obj+0x5c0], a record of the sheet pool 0x142a17290; HP +0, max +4, MP +0x180, max +0x184) |
| `hp-at-most N`, `mp-at-most N` | HP / MP at most N |
| `command-depth N` | the field command menu's stack is N lists deep (1 the main list, 2 a sub-list such as Items is open, 0 none) |
| `command-row N`, `command-id N` | the cursor of the top list is on row N / on a row of command N |
| `command-has N` | the top list has a row of command N that can be chosen (state byte below 4; Items with an empty item list is state 5) |
| `world N`, `area N` | the current world / area ([0x140717008], [0x140717009]; the first field is world 18 (0x12, eh) area 2) |
| `menu-open` | a system menu runs (MENU::isRunning [0x1409006b0]): START opens the camp menu only when no mission runs, else the mission's pause screen |
| `mission` | a mission runs (0x3ad500: one of the six records at 0x142a114a0); the first field's EH_NOKILL mission bars its exits with a barricade |
| `enemies-at-most N` | at most N enemy objects (class records with the BTLOBJ vtables of YS::ZAKO / ENEMY / BOSS / LARGEZAKO / LARGEBOSS) are in the object list [0x142a171c8] |
| `item-uses N` | the mod's item handler (exe 0x3c4b80 = DLL 0x31260, kind 0) applied N items since the step started |
| `item-drain`, `item-heal` | an item applied since the step started lowered / raised the target's HP or MP (the sheet read right before and right after the DLL's adds) |

Every item application prints `kh: item apply #k ... HP a -> b of max (amount x) ...` with the command's +0x2f byte and the
multiplier, so a route's numbers can be checked against the formula. Every map change prints `kh: map report (map ww:aa -> ww:aa)`
with the id table's slots, the arena zones' high-water marks, RSS, files opened and threads (with `KHNMC_IDTABLE_REPORT=1` the
whole id table follows). `KHNMC_ROUTE_TRACE=2` adds the player's sheet, position, facing and the camera yaw every 8 game frames
and the object list with the running mission every 64: what the coordinates in a route come from.
