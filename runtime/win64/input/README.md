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
