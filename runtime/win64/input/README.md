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
@expect COND                                   COND must hold now, else a named fatal (route-expect-failed; route-condition-
                                               unreadable when its reader cannot read the state: see task 31 below)
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

"Since the step started" (`frames N`, `hits N`, `item-uses N`, `item-drain`, `item-heal`): every step takes the baseline of these
counters when it starts, EXCEPT `@expect`, which reads in the very tick it starts and so keeps the baseline of the last step before
it that is not an `@expect` (task 31, the 2026-11-17 review: before, `@expect item-drain` could never pass and `@expect not
item-drain` never fail).  `@press A until item-uses 1` then `@expect item-drain` checks the use that press brought about; with no
such step before it, the baseline is the start of the run.

A step that does not see its state within its time-out ends the run with a named fatal:
`kh: FATAL: route-wait-timeout: <file> line N `<step>`: <state> never came within T game frames (step k of n, started at game
frame F, now game frame G, present P); game state now: <every state>`. The run's end prints
`kh: route report: k of n steps done; difficulty D; met at game frame: L<line>:<condition>=<frame> ...` and, if the run ended
first, `ROUTE INCOMPLETE ... waiting at line N` (route_batch.sh / route_one.py count that run as DIVERGED). Two steps' presses
are always separated by at least one released frame, so the game sees every press as a new one: a step without relative lines
hands over in the tick it is met, unless its last frame had a button or trigger down; then one released frame comes first (review
U35, routes n=4: `@press A until X` then `@wait Y` + `+1 +1 A` used to be one continuous A).  The stick alone does not count, so
a chain of `@goto` steps keeps walking. `KHNMC_ROUTE_END=exit` ends the
run (exit 0, with all reports) when the last step is done; the default `idle` keeps the pad released until KH_RUN_GAME_FRAMES.
`KHNMC_ROUTE_TRACE=1` prints every state whenever one changes (`kh: route trace game-frame=N ...`): how the values above were
found, and the first thing to look at when a route stops somewhere new (a new host, the iOS Simulator, a device).

The difficulty is chosen in the game's own New Game dialog (page 0; the cursor starts on GAME_SAVE::GetGameLevel = Standard after
InitSave). `KHNMC_DIFFICULTY` still forces the save's dword after InitSave and SetGameLevel (task 7); with a route, the
`@expect game-level difficulty` step then fails loudly if the two disagree. `route_batch.sh -d` and `route_one.py --difficulty`
set KHNMC_ROUTE_DIFFICULTY for a route script and KHNMC_DIFFICULTY for a frame-numbered one.

A file is either frame-numbered (`FROM TO`, `@hold`, `@clock`) or a route; mixing the two is a named fatal at load.

**An input script that is not played to its end is no success (task 31, the 2026-11-17 review).** When the run ends with status 0
while a route has steps left, a `@hold` was never released, or a frame-numbered script's clock never reached its last frame, the
status becomes 3 with `kh: FATAL: input-script-unfinished: <why>` (not for `--selftest`).  `@hold FRAME until initial-state N
[timeout T]` has a time-out: a hold not released by present FRAME-1+T (default 6,000 presents) ends the run with
`kh: FATAL: input-hold-timeout: <file> line L ...: the Initial Settings screen did not reach state N within T presents ...`
(before, it waited for ever and the run still exited 0).  A state already seen counts as the release even before the next poll.

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
whole id table follows). Task 31: each map report and the exit add `kh: file buffers: K of the N file and HD-asset buffers
handed to the game are still held by it: X MiB of Y MiB handed out (MiB by kind: obj/hd ..., map/hd ..., ...)` (the file and
GetRemasteredAsset buffers the game has not freed, by directory, HD asset or plain file); the exit lists the 40 largest by name (all
of them with `KHNMC_IDTABLE_REPORT=1`, which also lists the game's resource cache, `kh: moose cache: entry ...`). `KHNMC_ROUTE_TRACE=2` adds the player's sheet, position, facing and the camera yaw every 8 game frames
and the object list with the running mission every 64: what the coordinates in a route come from.

## Route scripts, task 30: fighting until something happens, the place's programs

```
@fight until COND [radius R] [every P] [hold H] [stall N] [timeout N]
                                               fight until COND holds: every game frame the left stick walks the player to
                                               the nearest enemy (camera-relative, as @goto enemy); within R (default 150) of it
                                               he stands and presses A for H of every P game frames (default 3 of 20, counted
                                               from the step's start); with no enemy in the object list he stands still and
                                               presses nothing (A near an NPC would talk), and waiting for the next wave is not
                                               a stall.  Walking toward an enemy without getting 10 map units closer for N game
                                               frames (default 600) is the named fatal `route-goto-stalled` (a distance 50 above
                                               the closest one, after a knock-back, starts a new approach); `until` not holding
                                               within the time-out is `route-wait-timeout` (with the enemies seen at the last
                                               look and the attacks pressed).  `until` is required.
```

| condition | true when (runtime/route_state.c has the details) |
|---|---|
| `door N` | the current place's door (the entrance whose spawn points the player takes) is N: [0x14071700a] |
| `map-program N`, `btl-program N`, `evt-program N` | the program of the area's `map` / `btl` / `evt` script that runs now: words [0x14071700c] / [0x14071700e] / [0x140717010] (0x152be0 writes the place: world, room, door, the three programs; their defaults are the save's three words per room at 0x1409a98c0 + 6 * (world * 64 + room)). A program decides the spawn files, the mission and the event; an event names the program that follows it |

```
@skip until COND [every P] [hold H] [timeout N]
                                               skip the story's events through the game's own pause screen until COND: while
                                               no pause screen is up, START for H of every P game frames (default 2 of 30);
                                               when it is up (`pause-menu`), DPAD to the row with id 5 ("skip") and A.  A pause
                                               screen without that row (a mission's pause has no rows) is the named fatal
                                               `route-skip-no-skip-row` with the rows it has.  Typical: `@skip until field`
@talk until COND [every P] [hold H] [stable S] [timeout N]
                                               answer field message windows: A for H of every P frames (default 2 of 20), only
                                               while a message waits; the step ends when COND has held S game frames in a row
                                               (default 30), so the one-frame gaps between pages and the pauses between two
                                               windows of one scene do not end it.  Typical: `@talk until field stable 120`
```

| condition | true when |
|---|---|
| `pause-menu` | the pause screen (system menu 50: MENU::isRunning, menu id [0x140900724] = 50) takes input: its row list ([[0x140bef538]], u16 ids, 0xffff = none) has a row. START opens it instead of the camp menu while a mission record exists; the story's events set one, and then its rows are 6 (continue) and 5 (skip the event) |
| `pause-id N` | the pause screen's cursor ([[0x140bef528]]) is on the row with id N; `@select pause-id N` moves it there (DPAD_DOWN = next row) |
| `pause-has N` | the pause screen has a row with id N |

A condition's number may be written in hex with `0x` (`@wait btl-program 0x1301`), as the program ids appear in the area data
(`ard/us/<map>.ard`, entries `map` / `btl` / `evt`). The map report and the `KHNMC_ROUTE_TRACE` state line print door and programs (the state line also the pause screen's rows and the
first message window's state and mode).
`KHNMC_TRACE_PLACE=1` (diagnostic) prints every object initialisation (0x3b3200 and its wrappers 0x3da940 / 0x419230: object
id, position, facing) and every set_pos / set-rotation call during the 600 game frames after a map change; `KHNMC_TRACE_MENU=1`
(diagnostic) prints every dword of the menu globals and of the camp menu's two blocks that changed during a game frame while a
system menu runs. Both only read.

## Route scripts, task 31: what the game tells the player, reaction commands, named objects

A route is now written from the game's own words.  The runtime prints them as they come (readers only; `runtime/route_objective.c`):

| line | when | from |
|---|---|---|
| `kh: objective: game frame N: mission NAME (id, flags, pause kind): objective message 0x..: "..."; INFORMATION message 0x..: "..."` | a mission starts, ends (`no mission`), its texts change, or one of its two messages becomes readable (at a mission's start its table may not be loaded yet: the line says `(message 0x.. is in no loaded message table)` and comes again with the text) | the mission object `[0x142a0ff68]`, its header `[obj+8]` (id word +2, flags +4, INFORMATION message +6, pause kind byte +8, objective message +0xa = the text of the mission's pause screen), the message through the three tables `[0x142a11668 + 0x10*i]` (the lookup of 0x3ad9a0) |
| `kh: objective: ... mission id 0x.. counters: a of A, b of B` | a counter of the mission changes | up to three gauges of 0x50 bytes at mission+0x160 (layout +8, target +0x24, value +0x38) |
| `kh: information: game frame N: "..." (message 0x..[, the id the game looked up; the same text is message 0x.. = 0x..])` | a line goes into the INFORMATION bar | a pass-through hook on 0x17b710 (Makefile `INFO_HOOKS`); the id is the one the game looked up with 0x3ad9a0 right before (same thread, same game frame, same pointer: the mission's own line, 0x3a3f60 / 0x3a41c1). Without such a lookup and a text several ids share, all of them are named (`message 0x3f56 = 0x3f59: ... which one it named is not known`) |
| `kh: message: game frame N: "..." (message 0x.., page at +K[; a choice of R rows, cursor on row C])` | a field message window starts to wait with another page | the window list (as `message`); text object at window+0x10: the message's first byte +0x10, the page shown +0x18, choice rows +0x3c; a page ends at byte 0x10 |

In the texts a colour change is `<<`...`>>` (the game highlights what to do that way), a button icon `(icon 0xNN)`: seen so far 0xda
attack (A), 0xdb jump (B), 0xde guard (X), 0xdf the reaction button (Y), 0xe1 lock-on (RB), 0xe3 the modifier of Reversal (RT),
0xe5 camera reset, 0xe7 START, 0xe8 / 0xe9 the left / right stick.  Texts are deduplicated: several ids may share one text
(msg/us/eh.bar 0x3f56 = 0x3f59, tt.bar 0x375b = 0x375c = 0x375e); where a line can only see a text pointer it names every id of it.

```
@react [id N] [on NAME] until COND [every P] [hold H] [timeout N]
                                               answer the reaction command the command menu offers: Y for H of every P game
                                               frames (default 2 of 20), only while a command is offered and can be answered
                                               (and it is command N / belongs to the object with the model name NAME, when the
                                               step says so: an NPC who walks by offers "Talk" too); until COND.  The game picks
                                               its entry again at the start of the frame the press lands in: when the entry of
                                               that frame is another answerable command than the step names, the press may have
                                               answered it: the named fatal `route-react-other-command`.  The time-out says how
                                               many frames an offer was answerable / seen but not answerable, and why not
@goto object NAME [radius R] [until COND] [stall N] [timeout N]
                                               walk to the nearest object whose objentry model name is NAME (the names are in
                                               `kh: route names` of KHNMC_ROUTE_TRACE=2, e.g. N_EX690_TT_TUTOR_RTN, SAVE_POINT).
                                               No such object: he stands, and the stall is the named fatal `route-goto-stalled`
                                               ("no object named ..."); an object that moves on is followed (a guide who keeps
                                               his distance is no stall)
@press BUTTONS [lx ly] held BUTTONS until COND [every P] [hold H] [timeout N]
                                               as @press, with the `held` buttons down for the whole step (a modifier: the mod's
                                               Reversal is R2 held and then X; both pressed in the same frame is a Guard)
@select message-choice N [timeout N]           a choice window: its cursor to row N (DPAD_DOWN = next row), then press A
@if-difficulty N STEP                          STEP exists only in a run of difficulty N (what a use of an item does differs)
```

`@skip` and `@fight` now answer a message PAGE that waits (START does nothing while one waits; a tutorial page can come in a
fight after the last enemy fell) with A; a CHOICE window is the named fatal `route-skip-choice` / `route-fight-choice`: the route
answers choices itself (`@talk until message-choice 0 stable 1`, `@select message-choice N`, `+1 +3 A`).

| condition | true when |
|---|---|
| `mission-id N`, `objective N`, `information N` | the running mission's id / objective message / INFORMATION message is N |
| `mission-count1 N`, `mission-count2 N` | the running mission's first / second counter is at least N |
| `reaction`, `reaction-id N`, `reaction-on NAME` | the command menu offers a reaction command that can be answered (/ command N / of the object named NAME): the entry the reaction handler 0x408520 keeps at FC+0xb40 (command word +2, state byte +5 below 4, parameters at FC+0xb28: the target's id first).  NOT the seven rows at FC+0x9b0: the field command's update 0x3b2340 empties them every frame.  "Can be answered" also reads the gates of 0x3b2340 / 0x408520 (review of task 31): the pad record `[FC+0xae8]` set, FC+0x24 bits 0 and 1 clear, `[0x142a10500]` 0, and the entry fresh (0x408520 picks one only while 0x39a8c0 is true, i.e. not progress flag 0x817 without 0x81c, and `[FC+0xb3c]` bit 0 is set; otherwise the entry is stale).  Not read: the player class's virtual +0x78 (code). The state line shows `reaction=N(not-answerable)`, KHNMC_ROUTE_TRACE=2 the reason |
| `lock-on` | the player is locked on to a target (his target record `[player+0xdc8]` is of kind 2) |
| `message-choice N` | a message window asks for a choice (mode 2, input phase `[w+0x408]` 1) and its cursor `[w+0x40c]` is on row N |
| `menu-id N` | a system menu runs and its id `[0x140900724]` is N: 50 the pause screen, 3 the save menu, 8 the camp menu (the shell 0x2e4950 starts for menu 0) |
| `menu-page N` | the camp menu takes input on its page N (`[0x140bee64c]`, phase `[0x140bee650]` 1): 0 = its main list |
| `menu-locked` | START would not open the camp menu: a mission record is set, or 0x3ad540 / 0x3abbb0 say no (bit 25 of `[0x140749804]`; the save's progress flag 0x817 set while 0x834 is clear: the prologue before the main-menu unlock; a weapon not ready; ...).  The state line names the reason: `camp-menu=closed(<reason>)` / `camp-menu=open` |
| `item-slot N` | one of the player's item slots holds item N (1 Potion, 3 Ether); the state line shows them: `item-slots=3:0,0,0` |
| `menu-text N` | the running menu looked up message N on the page it shows now (`kh: menu text:` lines, a pass-through hook on 0x3ad9a0): the labels of the rows it shows (0x44b Items, 0x44d Abilities, 0x451 Customize, 0x44f Status, 0x4539 Roxas's Journal, 0x3617 Config) |
| `progress-flag N` | the save's progress flag N is set (N = world << 10 \| index, as 0x39afc0 sets it: bit N&31 of the dword 0x1409ab540 + 4*(((N&0x3ff)>>5) + 8*(N>>10))); 0x817 = TT #23, 0x834 = TT #52 (the menu tutorial), 0x84a = TT #74 |
| `menu-flag N` | the save's menu flag N is set (bit N&31 of the dword 0x1409abc40 + 4*(N>>5), as 0x3e38f0 tests it): 0x70 makes the camp menu show Items, 0x72 Customize, 0x71 Party |

**What the camp menu shows** (exe 0x304c40 over the eight row descriptors at 0x1405badb0, 36 bytes each: label +0x12, help +0x14,
two lists of menu flags at +0x1c and +0x20): a row is shown when every flag of its first list is set and none of its second list:
Items needs menu flag 0x70, Party 0x71, Customize 0x72, Jiminy's Journal 0x73, Roxas's Journal needs 0x73 clear; Abilities is decided
apart (0x304cbc: a party member with abilities, progress flag 0x817 and not 0x849); Status and Config always.  Menu flags are set by
the progress scripts of `00progress.bin` (per world: u16 offsets by flag index, then `{u8 opcode, u8 count, u16 args}` up to opcode
0; opcode 8 sets menu flags, 9 clears them, 12 sets further progress flags), which 0x39afc0 runs when a progress flag is set.  In
Twilight Town only the script of 0x84a sets 0x70 and 0x72, and only `ard/us/tt32.ard`'s evt program sets 0x84a (the Station of
Awakening, the second day after the odd jobs, the station and the old mansion; `ard/us/tt08.ard` sets the next flag, 0x835).

**A state no reader can read is neither true nor false** (review of task 31).  `hp-below-max`, `mp-below-max`, `hp-at-most N`,
`mp-at-most N` without the player's status sheet, `command-row N`, `command-id N`, `command-has N` without the command menu, and
`item-slot N` without the item slots are UNREADABLE: then neither the condition nor its `not` holds.  A waiting step goes on waiting
(its time-out says `its reader cannot read the state now`), an `@expect` is the named fatal `route-condition-unreadable` (before,
`@expect not item-slot 1` passed when the slots could not be read).  Every `@expect` that passes prints what it read:
`kh: route: @expect line L read at game frame N: <the state line>` (player HP / MP, item slots, command menu rows, ...).

Idiom for "the mission is over, go on to the next control": `@skip until <the next mission, area or program>` and then
`@wait field` (right at a mission's end the field is still up for a few frames: `@skip until field` would be met at once).

Diagnostics (read-only): `KHNMC_ROUTE_TRACE=2` also prints the reaction command and the lock-on when they change, the pad records'
held words, and every 64 game frames the objects with their model names (`kh: route names`); `KHNMC_TRACE_MISSION=1` prints every
dword of the mission object that changed (how the counters were found); `KHNMC_SELFTEST_ONLY=route31` runs the task 31 suite
alone (said to be a PARTIAL RUN; not a pass of the selftest).

`input/route-camp.txt` is the route from a new game to the first camp menu (the second day of the Twilight Town prologue,
Station Heights): 164 steps; START opens the camp menu at game frame 22,378 on difficulty 3 and 24,363 on difficulty 1, the route
ends at 22,624 / 24,609 (run-Q31/last-camp-d3, last-camp-d1; the frames vary by a few hundred from run to run).  Its main list has no Items and no Customize row there (menu flags
0x70 / 0x72 clear: the route checks them), so a Potion can neither go into an item slot nor be used from the menu yet.

## Route scripts, task 33: the DEBUG WARP (a test tool, not play)

**A result after a warp says "this room loads from this state", not "the game reaches it".** The warp puts the game in a room
the story may never send it to with these programs, and everything the room expects (story flags, party, items, the room
before it) is whatever the run had when it warped. No story route and not the default route use it; a route that does prints
`kh: warp:` lines for every warp and its route report counts `debug warps`.

```
@warp WORLD ROOM [door N] [map N] [btl N] [evt N] [settle S] [timeout T]
                                               ask the GAME for a map jump to WORLD ROOM through its own function, the way the
                                               mod's script call MapJump and the camp menu's place jump do: the 10-byte place
                                               record {world, room, door, 0, map program, btl program, evt program} handed to exe
                                               0x152990(record, 1, 0, 0, 0) on the game thread at the end of a game frame, only
                                               when the field has been up S game frames in a row (default 60) with no system menu,
                                               no message window and no jump pending.  WORLD is a name of the exe's table
                                               0x140717210 (zz es tt di hb bb he al mu po lk lm dc wi nm wm ca tr eh) or 0..18;
                                               ROOM 0..63; door default 0; a program left out is 0xffff = the save's program for
                                               that room (0x152be0).  The step is met when the jump has run and the field has
                                               stayed up S game frames in the new room (or in the room its own program jumped on
                                               to); not within T game frames of the request (default: the @timeout) is the named
                                               fatal `route-warp-failed` with the outcome below.  Nothing else is written to the
                                               game: the record is copied by 0x152990 at once.
@warp list FILE [settle S] [timeout T]
                                               the same for every line of FILE (relative to the route file): `WORLD ROOM [door N]
                                               [map N] [btl N] [evt N]`, `#` comments.  A place that does not come back to the
                                               field ends the run (exit 6, `kh: warp: list stopped at entry K of N ...`, then the
                                               route report); tools/room_sweep.sh starts the next process with the next entry.
```

Every warp prints `kh: warp: #K DEBUG WARP (a test tool, not play) to ...` when it is asked for and one
`kh: warp: #K result ROOM OUTCOME: ...` line (requested / jump done / field at game frame, the place and programs now, the
player's position, what `field` is made of at the end: `at the end: field commands this frame N, message windows W (waiting X),
menu-open M, jump pending J`, the mission file, scene files, the map files opened with their source (the first 12 names), every
file MISS that looks like game data with the return sites on the loading thread's stack, files opened), then a map report (id
table slots, id-zone and far-zone marks, RSS: the PROCESS's marks, which a list of rooms in one process carries from room to room). A fault or a named fatal during a warp also prints `kh: warp: FAULT OR FATAL DURING DEBUG WARP #K ...`.

| outcome | means |
|---|---|
| `FIELD`, `FIELD+MISSION` | the field is up in the room and stayed up S frames (a mission file was opened: the btl program started a mission) |
| `JUMPED-ON` | the room loaded and its own program (an event, a door, a jump of its programs) moved the game on to another room, where the field came up; the result names that room |
| `MAP-MISSING` | no field within T frames, and the room's map file `map/<world><room>.map` (the exe names it after the room, 0x152ed0 + "map/%s.map" at 0x161c50) was asked for and is in neither the loose tree nor the packages |
| `SCENE` | no field within T frames; event data was read (a scene ran or runs) |
| `JUMP-PENDING`, `MESSAGE-WAITING`, `MENU-OPEN`, `LOADING` | no field within T frames, and at the time-out: a jump the room's program asked for is pending / a field message window waits for a button (the warp presses nothing) / a system menu runs / none of these (still loading, or no player control). Logs before the review of task 33 say `STILL-LOADING` for all four |
| `JUMPED-ON-NO-FIELD` | the room's own program moved on to another room and no field came there within T frames |
| `JUMP-NOT-DONE`, `JUMP-NOT-STARTED`, `NO-SAFE-MOMENT` | the jump task did not finish / never appeared / no safe moment came within T frames |

`tools/room_sweep.sh` (task 33) runs the sweep over every area file with these lists (see its header); `tools/room_sweep.py
--selftest` checks its log parsing, its table and the runner's bookkeeping. Its table adds `NO-PROGRAM` from the data: a `LOADING`
whose place names no map, btl or evt program of its room's area file (nothing of the room runs).

With `KHNMC_XION_SWAP=1`, click both sticks together at a save point to switch between Roxas and rescue Xion (P_EX220). Release both before switching again. Xion has her own Attack, Jump and Laser Rain commands. Her command list omits the normal save reaction. Stand within 60 game units of the point to switch back, or press Y there to hand control to Roxas and use the save reaction. The choice lasts for the session and resets on New Game or Load Game. Field support is restricted to the area programs listed in implementation notes.

`route-xion-fight.txt` checks the controller with `player-char 4` (Xion) and `player-char 1` (Roxas), checks canonical EXP with `save-exp-at-least N`, and writes a slot in the run's private Documents directory. Start a fresh process with that same `KH_DOCS` and `route-xion-reload.txt` to check native loading. `route-xion-event.txt` exercises the automatic return before the armor-shop scene.
