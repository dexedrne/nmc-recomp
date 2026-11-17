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

The default script confirms at frames 90, 180 and 270 and selects Yes at 240.
It stops after 272, before the title task, to avoid starting a new game.
Use a fresh `KH_DOCS` directory to reproduce the first-launch sequence;
completed Initial Settings writes configuration through the game's save code.

Every 60 presents, `KHNMC_PHASELOG` (default `phase.log`, append mode) records
render-counter deltas, first-seen CFileMan request names and eleven entry counters.
`initial-state` and `choice` are the last observed Initial Settings values.
Native archive aliases extend the existing link hooks to same-shard calls;
`lifted/` and `lifter-rt/` are untouched. File names repeat in `KHNMC_FILELOG`
when the engine asks again; the phase log lists each name only once per run.
