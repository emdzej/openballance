# Internals

How OpenBallance works, and the specifications its subsystems were ported from.

- [Architecture](/internals/architecture): the runtime, the Building Blocks, rendering, physics and the
  platform layer.
- [Virtools runtime (CK2)](/ck-runtime): the object model, file loading and the behaviour engine.

**Building Blocks**: [gameplay BBs](/gameplay_bbs), [fonts and 2D text](/fonts), [the sky](/sky),
[curves and animations](/animation), [particle systems](/particles).

**Physics**: [the physics BBs and manager](/physics), then the Ipion engine itself:
[core and dynamics](/ivp_core), [collision detection](/ivp_collision) and
[contact response](/ivp_contact).

**Design records**: the [initial analysis](/internals/analysis) and the
[gasm graphics requirements](/internals/gasm-gfx-requirements) written for gasm 0.4.0.
