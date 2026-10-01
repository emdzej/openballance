# Run the tests

The tests need the original game data, which can't be distributed, so CI only compiles them. Point
`OPENBALLANCE_DATA` at your copy (an installed folder, the CD folder or a disc image); the default is
`./data`.

| Test | What it does |
|---|---|
| `build/ck_test` | loads every `.cmo` / `.nmo` of the game and checks the object counts |
| `build/cab_test` | with the CD or image: every file read from the InstallShield cabinets equals the installed folder |
| `build/run_test N` | boots the game headless for N frames and lists the Building Blocks that ran (and any that are missing) |

`run_test` takes its input and probes from environment variables:

| Variable | Effect |
|---|---|
| `KEYS="from-to:hexDIK,..."` | hold DirectInput keys over frame ranges (1c Return, 01 Esc, c8 Up, d0 Down, 10 Q …) |
| `TRACE_FROM=f` with argument `-vN` | print every Building Block executed in frames f..N |
| `PHYSDEBUG=n` | the physics world every n frames (`PHYSCONTACTS`, `PHYSALL` for more) |
| `PHYSPROBE=1` / `2` | the flat-ground ball probe (2: on a 4000 m floor) |
| `DUMPARRAY="name:f"`, `DUMPGROUP="name:f"` | a data array's rows / a group's members at frame f |
| `WATCHPOS=name`, `WATCHPARAM=name` | log an entity's matrix / a parameter's value when it changes |
| `BOXES="prefix:f"` | world bounding boxes of entities by name prefix |
| `SETCELLS="array:col:value"` | force a column (e.g. `DB_Levelfreischaltung:0:1` unlocks every level) |
| `MEMSTAT=n` | heap use every n frames (macOS) |

Example: level 1, dismiss the tutorial prompt, roll down the ramp:

```sh
KEYS="1000-1005:1c,1150-1155:1c,1750-1755:10,1900-2500:d0" PHYSDEBUG=20 ./build/run_test 2500
```
