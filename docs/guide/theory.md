# How Ballance works

A theory of operation of Ballance (Cyparade / Atari, 2004): how the game is built, from the frame loop to
the twelve levels. It comes from reimplementing the Virtools runtime the game runs on, porting the Building
Blocks and the physics engine from the original DLLs, and reading the game's own behaviour graphs. It is
meant for anyone who wants to learn how a physics puzzle game of its time is put together, and in particular
how a game made in a visual-scripting tool works. The details, with the DLLs' addresses and layouts, are in
the specification pages of this site ([Internals](/internals/)) and in the
[repository](https://github.com/emdzej/openballance).

## The shape of the program

Ballance has almost no game-specific native code. It is a **Virtools 2.1** title: a generic player
executable hosts the Virtools engine, and the game is data that the engine runs.

| Layer | What it is | What it does |
|---|---|---|
| The player | A small generic executable | Opens the window and the render context, loads the first composition file, runs frames |
| The engine (CK2) | Virtools' runtime and its maths library | Object model, file loading, the behaviour scheduler, parameters, messages, data arrays |
| The render engine | Virtools' scene graph over Direct3D | Meshes, materials, textures, sprites, 2D entities, the fixed-function pipeline |
| Managers | Input, sound, parameter operations | Services the scripts call into |
| Building Blocks | About twenty DLLs | The scripts' instruction set: about 180 distinct blocks, from "send a message" to "physicalize an object" |
| **The game** | `base.cmo` and the `.nmo` files | **Behaviour graphs**: the menus, the gameplay, the balls, the moving modules |
| **The levels** | One `.nmo` per level | Pure data: meshes, materials, textures and named groups, with no scripts at all |

So the game's "source code" is a few thousand nodes of visual script, saved next to the 3D data. A small
number of blocks are specific to the game; they come from the developer's own toolbox DLLs (the "TT"
blocks: the sky, the ball's shadow, the extra-point sprites, the highscore database) and from
`physics_RT.dll`, which wraps the **Ipion** physics engine.

That is why OpenBallance does not translate the game into C. It reimplements the runtime and the blocks
and runs the original graphs, so the game's own logic, with its timing and its quirks, is preserved by
construction. The reasoning is in the [initial analysis](/internals/analysis).

### Start-up

The player loads `base.cmo` and activates its scene. One object in it, the level object, owns the
top-level scripts; only one of them is active at the start. It sets the background, reads the language and
the hardware class (a low-spec machine gets lower-quality sounds), loads the settings and highscore
database, plays the intro movie, and then asks a loading script to bring in the rest of the game: the menu,
the gameplay, the balls, the sounds, the camera rig. It then activates the global event handler and, two
frames later, sends it the message that opens the menu.

From then on the game is driven by that **event handler**: a state machine that waits for messages such as
"load level", "start level", "pause", "dead", "end level", "exit level" and "next level", and answers each by
loading or unloading files, activating or deactivating scripts and resetting objects.

## Time

### The frame

Every frame the engine does the same things in the same order:

1. Read the input (keyboard state as DirectInput scan codes, and the mouse).
2. Apply the scripts' pending activation requests, then **execute every active script** of every object, in
   the order the objects were registered.
3. Deliver the messages the scripts sent during the frame.
4. Step the **physics** and write the moving bodies' new poses into their 3D entities.
5. Update the sounds (fades), then render the frame and mix the audio.

The order has consequences a designer can feel. A script reads where the ball was at the end of the
previous frame. A force a script creates this frame acts in this frame's physics step. A message sent this
frame is seen by its receivers in the next one.

The port runs at a fixed 60 frames a second and hands every frame the same delta time. How the original
player measured its delta (fixed or wall-clock) is not documented in the port's notes.

### Script time: frames and milliseconds

Inside the graphs there are two clocks. **Link delays** count frames: a link with delay 0 fires within the
same frame, and a link with delay *n* fires *n* frames later. The game uses small delays everywhere as "wait
until the dust settles": one frame for polling loops, two after a message or a physics change, a few
between deactivating one sector and activating the next. **Timers, interpolators and "per second" blocks**
use the frame's delta in milliseconds, so animations and countdowns follow real time.

### Physics time

The physics has a clock of its own. The frame delta is smoothed (each frame keeps three quarters of the
previous value), multiplied by a time factor, and the engine then runs as many **fixed steps of 1/66 of a
second** as fit before the new target time. The game sets the time factor to 2 while playing and to 0 when
paused, so **the simulation runs at twice real time**. That is a design choice for feel: the world reads as
large, with gravity and the ball's mass tuned to it, while the action stays quick. At 60 frames a second that
is a little over two physics steps per frame. Between steps, the drawn pose is extrapolated from the last
step, so the ball moves smoothly.

### Randomness

There are two random generators. The engine's is the Microsoft C library's `rand()`, a linear
congruential generator seeded with 1. One stream serves the scripts' random blocks, the random switches and
the particle emitters, so the order in which blocks run decides which values each gets. Inside the physics
engine is a second, tiny generator (multiply the seed by 75), used only to vary when the engine checks
whether bodies can go to sleep. Neither is reseeded.

With a fixed delta, fixed seeds and input indexed by frame, a run of the port replays the same way, which the
headless tests rely on. Bit-exactness with the original is not possible: the original's physics ran on the
x87 FPU's extended precision (see [Physics](#the-physics)).

## The data

### Composition files

Everything the game loads is a Virtools composition file (`.cmo` for the first file, `.nmo` for the
others). A file has an object table (each object's class, name and index) and, for every object, a
**state chunk**: a self-describing block of 32-bit words grouped under identifiers, with references to other
objects given as indices into the file's table. Both parts are zlib-compressed. Loading is two passes: create
every object, then let each class read its chunk, then resolve the references and bind every Building Block
to its native implementation by its prototype GUID.

The format is generic: a texture, a mesh, a camera, a data array and a behaviour graph are all "objects
saved as chunks". This is what lets a tool like Virtools save a whole game, scripts and art together, and it
is the main idea behind the engine. The [Virtools runtime](/ck-runtime) page describes the chunks the port
reads.

### What is code and what is data

The split is unusual. Everything a level designer or game designer would touch is data: the graphs, the
parameter values on every block, the tables of ball properties, the key bindings, the list of levels. The
code is the engine and the blocks: generic machinery that knows nothing about balls or levels. Even most of
the game-specific blocks are generic in shape (a sky builder, a shadow projector, a spring follower), and
the game composes them.

Shared state lives in **data arrays**: small typed tables that any script can read and write by name. One
holds the game settings, one the current level's state (which ball is active, the current reset point,
whether a sector change is in progress), one the lives and points, one the list of levels with their
properties, and so on. Scripts also find each other, and the groups and arrays they need, **by name**:
loading a file called *X* creates a group of its objects and activates a script called *X*_Init. It is a
loose, string-keyed design that let the designers add files and scripts without touching any code.

### Finding the files

The game's data is not loose on the CD: it sits in InstallShield cabinets. The port reads an installed
folder, the CD's cabinets, or a disc image (ISO 9660, cooked or raw sectors) with its cabinets inside, and
resolves the game's Windows paths case-insensitively. Textures are BMP and TGA, sounds are PCM WAV, the intro
is a Microsoft Video 1 AVI, and the settings and highscores live in a small database file of saved arrays.

## The world

### A level is groups

A level file carries no scripts. It is meshes and materials sorted into **named groups**, and the names are
the level's meaning. When a level loads, the level-init scripts walk the groups by name:

- **Floors** and **rails** become fixed physics bodies, one concave surface each, with the floor material's
  friction and elasticity. "Stoppers" share the ball's collision group, and bodies of the same group
  never collide, so the player's ball passes through them.
- **Placeholders** stand in for interactive objects: the start, the checkpoints, the reset points, the end,
  the extras, the transformers, the loose balls and boxes, and the numbered **modules** (the moving
  contraptions). For each kind the matching prefab file is loaded once and copied as many times as the level
  needs; the copies take their placeholders' positions.
- **Sector** groups tag every placeholder with the sector it belongs to.
- **Sound** groups tag floors with the hit sound and the rolling sound they make.
- The **shadow** group marks the objects that receive the ball's shadow.
- The **depth-test cubes** are boxes around the level's playable volume, used to detect a fall.

The approach makes a level a pure art asset. The designers build the track in a 3D tool, drop marker objects
where the gameplay goes, name the groups, and the generic scripts do the rest.

### Sectors

A level is divided into **sectors**, the stretches between checkpoints. Only the current sector's objects
are live: when the ball reaches a checkpoint, the sector manager walks the table of placeholders, deactivates
everything in the old sector (resetting its scripts, or removing its bodies from the physics and hiding them)
and, a few frames later, activates everything in the new one: places the copies, shows them, physicalizes
the loose objects and starts the modules' scripts. Sectors bound the cost of the physics and the scripts,
and give the game a natural restart point: dying re-runs the current sector's reset.

### Resetting by initial conditions

Virtools objects can keep their **initial conditions**: a saved copy of their state that a script can
restore. The game restores initial conditions everywhere: modules after a death, ball pieces after an
explosion, the level's objects on exit. Because the physics engine never reads an entity's position back
(see below), every reset of a physical object is the same three steps: remove it from the physics, restore
its initial conditions, physicalize it again.

## The gameplay

### The ball

There are three balls, wood, stone and paper, and they are one ball with three sets of numbers. All are
spheres of the same size; they differ in mass, friction, elasticity, damping and the strength of the
steering force, held in one table the gameplay scripts read when the ball changes. Paper is very light,
bouncy and heavily damped: it drifts, catches the wind and stops quickly. Stone is heavy, barely bounces and
keeps its momentum; it is the only ball that breaks certain bridges. Wood sits in between and grips best.
The puzzles of the game are mostly about which ball a stretch of track needs.

**Steering** is four keys, each of which, while held, creates a constant physics force on the ball along one
horizontal axis of the camera's orientation frame, and destroys it on release; both also wake the ball, since
a sleeping body ignores forces. The force's direction is fixed to the world when it is created, so after the
camera turns, the steering is torn down and rebuilt in the new orientation. There is no speed control, only
force: the feel comes entirely from the physics.

**Transformers** swap the ball. Each frame the gameplay looks for the nearest transformer; if the ball is
close enough and of another type, it takes control away, plays the transformer's animation, pulls the old
ball onto it, blows it up into physical pieces with random impulses, and physicalizes the new ball in its
place. The pieces fade and reset after a while.

### The camera

The camera is two **spring followers** chained together: a frame that lags behind the ball, and the camera
position that follows that frame with different stiffness and damping per axis. That gives the camera its
soft, swinging motion with no camera logic beyond the springs. Holding Shift and pressing left or right turns
the orientation frame by a quarter turn about the vertical, eased over a fraction of a second; steering is off
while it turns. Holding Space lifts the view for an overview, by switching the follower's offset and
stiffness (which parameter it changes exactly is not established).

The camera starts each level at the start reset point. In level 1 that faces away from the track, and the
tutorial asks the player to turn around. It is deliberate: it is how the game teaches the camera turn.

### Lives, points and the clock

A level starts with a stock of points and three lives. A timer takes a point away at a fixed interval while
the ball is in play, and stops while paused or while the ball is respawning, so the score is mostly a measure
of speed. **Extra points** are clusters of small sprites orbiting a centre; when the ball comes near, they
burst away and then home in on the ball one by one, each hit adding points. **Extra lives** are collected by
touching them. At the end of a level the score screen counts up the points left, a bonus that grows with the
level number, and a bonus for each life left. The exact columns the tally reads are not fully established.

### Checkpoints, death and the end

A **checkpoint** is a proximity test: reaching it makes its reset point current, advances the sector and
shows the next checkpoint. The last one cues the end music.

**Falling off** is detected by geometry, not by height: each frame the gameplay tests the ball against one of
the level's depth-test cubes (an oriented-box intersection), cycling through them frame by frame. When the
ball overlaps one, it is deactivated: steering and camera are turned off, the clock stops, and the screen
fades. With lives left, a life is taken, the current sector is reset, and a new ball appears at the current
reset point inside a lightning sphere, becoming physical a few seconds later. With no lives left, the game is
over.

The **end** is a platform under a balloon. Touching it ends the level: the balloon, itself a small physics
rig of hinges, sliders and a spring with upward forces, flies away, the sky fades out, and the score screen
follows. Finishing a level unlocks the next and saves the database; a score good enough for the level's
table of ten asks for a name.

### Modules

The moving contraptions are prefab files with the same script shape. When their sector activates, they
physicalize their parts (usually frozen, so they do not move until the ball is near) and create their
**joints**: hinges for flaps, bridges, seesaws and swings, sliders for pushers and moving floors, ball joints
for things hanging on ropes, springs for things that return. Some are driven: a swing or a hanging sack is
pushed by a force that changes direction on a timer, a fan is a box volume that pushes anything inside it
upwards. A rope bridge of hinged planks tears only under the stone ball. When the sector deactivates, a
module removes its bodies and restores its initial conditions a frame later.

The modules show how far the game leans on the physics engine: none of them is animated. Every moving thing
in a level is a rigid body under gravity, joints and forces, and the puzzles come out of the simulation.

### The physics

The physics is **Ipion** (IVP), a commercial rigid-body engine of the late 1990s, compiled into
`physics_RT.dll` with a thin Virtools layer around it. The layer is a manager that owns the physics world,
maps entities to bodies and runs the simulation after the scripts each frame. Its blocks (physicalize,
force, impulse, hinge, slider, ball joint, spring, wake up, collision listeners) do not act at once: they
queue commands that are retried every frame until the bodies they need exist, so a script can create a joint
before its parts are physical. The layer only writes poses into the entities and never reads them back, which
is why resets go through the physics.

Inside, the engine is built around ideas worth knowing:

- **Fixed steps and controllers.** Each step runs a list of controllers in priority order on each group of
  bodies that move together: the friction system, the game's forces and springs, gravity with damping,
  friction, the joint solver, the contact forces. Then it integrates the velocities. The game's steering
  force is applied as a fixed impulse per step.
- **Sleep.** Bodies that have barely moved for a short while, or moved little for a few seconds, are frozen
  and cost nothing until something wakes them. A ball at rest falls asleep, which is why steering wakes it.
- **Event-driven collisions.** Instead of testing for overlaps at each step, the engine keeps, for each
  nearby pair of convex pieces, an object that tracks their closest features, and predicts *when* within the
  step they will touch. Far pairs sleep until one body has moved far enough. Floors are broken into one
  convex piece per triangle under a tree of bounding spheres, so the ball only ever talks to the few
  triangles near it.
- **Contacts with a gap.** Resting bodies are held a couple of centimetres apart by the normal force; an
  impact is resolved when the gap closes to about one centimetre. A quirk follows: when the ball rolls over a
  seam between two floor triangles it loses its contact, drops until the next triangle's impact catches it,
  and so rolls nearer the floor than it rests. The engine has a mechanism that hands a contact across a seam,
  but only its car-wheel code uses it, so it never runs in Ballance.
- **Materials.** Friction and elasticity of a pair are the products of the two bodies' values; bounces use
  the square root of the combined elasticity.

The port follows the disassembly constant for constant, but it has not yet been compared numerically with
recordings of the original game, and since the original ran on extended-precision x87 arithmetic the
trajectories cannot be bit-identical. The full story is in [Physics](/physics), [IVP core](/ivp_core),
[collision detection](/ivp_collision) and [contact response](/ivp_contact).

## How the scripts work

A **behaviour graph** is a set of blocks with inputs and outputs (the flow) and input and output parameters
(the data), joined by links. Each frame, an active graph puts its active blocks on a stack, runs them one at
a time, and follows their activated outputs: a zero-delay link activates the next block in the same frame,
inserted by priority; a delayed link is counted down over the next frames. A block either finishes or asks
to run again next frame, which is how waiting blocks (timers, key events, "wait for message") stay alive. A
safety limit stops a graph that loops without end.

**Parameters are pulled.** A block reading an input follows the link to its source; if that source is the
output of a **parameter operation** (add, multiply, get a row, compare), the operation runs first. The graphs
compute their values lazily, at the moment they are needed, like a spreadsheet.

**Messages** are queued and delivered after all scripts have run, to an object, a class or a group. The
scripts use them as the game's event bus: the gameplay tells the sound system a checkpoint was reached, the
ball's fall tells the energy script to take a life, and the event handler drives the levels.

The pattern for a designer is clear: small generic blocks, shared state in named tables, events by message,
and time by link delays. It is easy to extend and easy to break silently. One example the port met: a
parameter operation that the runtime cannot resolve falls back to copying its first input, and nothing
complains. The [Virtools runtime](/ck-runtime) page has the scheduler's exact rules.

## Saving

There are no saved games. What persists is the database: the highscore table of each level, the unlock flag
of each level, and the options (key bindings, volume and the like). It is a file of saved data arrays,
lightly obfuscated byte by byte, loaded at start-up and written by the scripts, for example when a level is
finished. The port keeps it in the platform's storage.

## Presentation

### Rendering

The game targets the Direct3D fixed-function pipeline, and its look is that pipeline's: per-vertex lighting
with up to eight lights, linear or exponential fog, alpha blending and alpha testing, colour-keyed textures
(one colour made transparent), and mipmapped textures. The port reproduces those rules in shaders.

The frame is drawn in a fixed order. First the **sky**: a cube built at run time by a block from a few
textures, centred on the camera and drawn behind everything with its own, wider projection. Then the opaque
objects, each followed by its extra **material channels**, the same faces drawn again with another texture
and blend; this is how the **ball's shadow** works, a darkening texture laid onto the floors under the ball
with coordinates recomputed every frame. Then the blended objects, the particles (camera-facing quads,
triangles or lines for the flames, sparks and dust), the 3D sprites, and last the 2D layer: the HUD, the menus
and the text, laid out in a 640×480 screen and drawn with a bitmap font.

### Sound

Sound is simple: a mixer of looping and one-shot wave players with volume, pitch and pan. The rolling sound
is the most interesting part. Collision listeners on the ball report contacts with floors by their sound
tag: a continuous contact loops the rolling sound of that floor's material for the current ball, with its
volume and pitch following the ball's speed; an impact above a minimum speed plays a hit sound scaled by
the impact's strength. Distance is done in script: a proximity block lowers a sound's volume with distance.
The music is a random choice among a level's themes over an ambient loop, with random pauses, and switches
to an ending theme near the finish.

## Design choices and quirks

- The physics never reads entities back, so every teleport and reset removes a body, moves it and
  physicalizes it again.
- Shared collision surfaces are cached by name, so a second object using one keeps the first one's scale.
- The fall test checks one cube per frame, so how quickly a fall is noticed depends on how many cubes the
  level has.
- One collision listener reports the impact position with its third coordinate always zero; the port keeps
  the bug.
- The data still carries the developers' working paths, German names and debug keys (a "god mode" with an
  upward force, a key that finishes the level).

## Lessons for building such a game

- **Make the level a pure asset.** Named groups and marker objects let designers build levels in a 3D tool
  while generic scripts turn the names into gameplay.
- **One mechanic, three parameter sets.** Three balls that differ only in numbers, plus a way to switch
  between them, give the whole game its puzzles.
- **Let the physics do the animation.** Every moving piece is a body under joints and forces; the game
  designs situations, not motions.
- **Divide the level into sectors.** Only what is near the player is live, which keeps the simulation cheap
  and gives a natural respawn point.
- **Keep reset simple.** Initial conditions plus "remove, restore, re-add" handle every restart the same way.
- **Springs make a camera.** Two chained spring followers give a soft, readable camera without camera code.
- **Small delays are a scheduling tool, and a trap.** Frame delays and queued messages make order explicit
  in a visual script, but a silently failing block or a missing wait shows up as a gameplay bug.

## Further reading

- [Architecture](/internals/architecture) and the [initial analysis](/internals/analysis): the layers and
  why the port runs the original graphs.
- [Virtools runtime (CK2)](/ck-runtime): files, chunks, the scheduler, parameters.
- [Gameplay Building Blocks](/gameplay_bbs): extras, the shadow, spring followers, intersections.
- [Sky](/sky), [curves and animations](/animation), [particle systems](/particles),
  [fonts and 2D text](/fonts).
- [Physics](/physics), [IVP core](/ivp_core), [IVP collision detection](/ivp_collision),
  [IVP contact response](/ivp_contact).
- [Reverse-engineering workflow](/howto/reverse-engineering).
