# Virtools runtime semantics (CK2 2.1)

What OpenBallance's `src/ck/` reproduces, from `CK2.dll` (image base `0x24000000`). Addresses are
function entry points in the Ghidra project `ballance`, program `CK2.dll`.

## Files

`.cmo`/`.nmo` layout, state chunks and identifiers: see `src/ck/ck_file.h`. Object references inside
chunks saved with option 8 are indices into the file's object table.

### Base object (`CKObject::Load` 0x24021053)

| Identifier | Meaning |
|---|---|
| `0x4` present | hidden (neither `VISIBLE 0x40` nor `HIERARCHICALHIDE 0x200`) |
| `0x18` present | hierarchically hidden (`0x200`) |
| neither | visible (`0x40`) |

### Behavioral objects (`CKBeObject::Load` 0x2401bf56)

| Identifier | Fields |
|---|---|
| `0x800` | scripts: count, behavior references |
| `0x10` | attributes: count, then per attribute `[class id]`, type name (string), category (string), GUID, parameter reference |
| `0x40` | priority word (data version >= 5: only if bit `0x10000000`) |

In `base.cmo` the `Level` object owns the nine top-level scripts: `Delete_All_Loaded_NMO`,
`Delete_Loaded_NMO`, `Loading_Manager`, `Default Level`, `Event_handler`, `Delete_LoadID`, `Level_Init`,
`Loading_Screen`, `Debug_Info`.

### Behavior (`CKBehavior::Save` 0x24004e1e; the load mirrors it)

Identifier `0x20`: `flags`; if `0x8000` prototype GUID + prototype version; if `0x4` priority; if `0x10`
compatible class id; if `0x40000` target object. Then a save mask and object arrays in this order:

| Mask bit | Array | Class |
|---|---|---|
| `0x100` | sub-behaviors | Behavior (8) |
| `0x80000` | sub-behavior links | BehaviorLink (6) |
| `0x4000` | parameter operations | ParameterOperation (4) |
| `0x200` | input parameters | ParameterIn (2) |
| `0x400` | output parameters | ParameterOut (3) |
| `0x20000` | local parameters | ParameterLocal (45) |
| `0x800` | inputs | BehaviorIO (9) |
| `0x1000` | outputs | BehaviorIO (9) |

- BehaviorIO: identifier `0x8`, flags (`1` input, `2` output).
- BehaviorLink: identifier `0x20`: delays (low word current, high word initial; both equal in the data
  seen so far), source IO, destination IO.
- ParameterIn: identifier `0x1000` (direct source) or `0x2000` (shared with another input): type GUID,
  source reference.
- ParameterOut / ParameterLocal: identifier `0x40`: type GUID, value mode, value (CKParameter load,
  FUN_2400867c): `0` type-specific sub-chunk, `1` buffer (u32 bytes + data; message and attribute types
  store names, parameter-type values store a GUID), `2` object reference, `3` none, other: skip one dword,
  value in the next. Four legacy GUIDs are remapped first. ParameterOut also has `0x20`: destinations.
- ParameterOperation: identifier `0x400`: operation GUID, argument count (3), input 1, input 2, output.

## Execution

### Frame (`CKBehaviorManager::Execute` 0x2400c40c)

1. For every object with scripts, in registration order: a script with a pending request
   (flags `0x10400000`) is `Activate(flags & 0x10000000, flags & 0x800000)` (activate, reset) and the
   request bits (`0x10c00000`) are cleared.
2. For every object: `CKBeObject::ExecuteBehaviors` (0x2401af67): each script that is active is
   `Execute`d, in script order.

### `CKBehavior::Activate(active, reset)` (0x24003957)

If `reset` or the flags are all zero: reset (FUN_24005dc8): every link's current delay = initial delay,
the delayed-link list is emptied, every sub-behavior is deactivated and reset recursively, every IO is
deactivated, flags `&= 0xef3fffbf`, `|= 0x80000000`, and if the behavior is a script (`0x2`) its input 0 is
activated. Then the active bit (`0x1`) is set or cleared.

### `CKBehavior::Execute` (0x24003806)

Building blocks (`flags & 0x8`, FUN_240048c4) call the prototype's function with the behavior context.
If bit 0 of the return value is clear (`CKBR_OK`), the behavior is deactivated; `CKBR_ACTIVATENEXTFRAME`
(1) keeps it active. The function itself reads and clears its inputs and activates outputs.

Graphs:

1. **Prepare** (FUN_240044ec). For each sub-link, clear its "fired" bit (`2`). If the link's source IO is
   active: remember the source; mark the link fired; if its initial delay is 0 the destination IO is
   collected for activation, else (current delay := initial delay if 0) the link is queued once
   (queued bit `1`) on the graph's delayed list. Then all remembered sources are deactivated, all collected
   destinations activated (and an input IO activates its owner behavior). Finally the execution stack is
   rebuilt: sub-behaviors from last to first, every active one (other than the graph itself) is pushed and
   marked "stacked" (`0x100000`), so the first sub-behavior is on top.
2. **Loop**. While the stack is not empty: pop the top, clear its "stacked" bit, `Execute` it, then
   **propagate** its outputs (FUN_2400477f): each active output IO is deactivated; each of its links is
   marked fired; a zero-delay link activates the destination IO immediately and, if that is an input of a
   behavior other than this graph, activates that behavior and inserts it into the stack by priority
   (above every entry with a priority <= its own, below higher ones) unless already stacked; a delayed link
   is queued as in Prepare. The loop runs at most `BehaviorMaxIteration` times; beyond that the original
   logs "Infinite Loop Detected" (FUN_24005e7c) and flags the result.
3. **End** (FUN_2400465b). Every queued link: fired; current delay decremented; at `< 1` it is reset to 0,
   unqueued, its destination IO activated and the destination's owner behavior activated; otherwise it
   stays queued. The graph is then deactivated unless links remain queued or a sub-behavior has flags
   `0x41` (active, or `0x40`).

So a link with delay 0 fires within the same frame, a link with delay *n* fires *n* frames later, and a
behavior activated through a delayed link runs in the frame after the one in which the link fired.

### Parameters

Reads are pulled. Copying a parameter's value from a source (FUN_2400824d) first runs the source's
operation (FUN_2400a7b5 → `CKParameterOperation::DoOperation` 0x240098bb) if the source is an operation's
output. Operations call their function with (context, output, input 1, input 2) (inputs swapped by a
flag), then mark the output changed. An operation without a function copies input 1 to the output.
