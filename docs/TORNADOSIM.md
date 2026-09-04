# TornadoSim pure Luau port

This demo ports TornadoSim's arcade result, not its C++ implementation. One
analytical wind field drives every moving thing. It is cheap, deterministic and
good enough for a game scene without a fluid solver.

## Source scope

The source lab has one useful core and several costly presentation systems.

| Source feature | Pure Luau demo | First pass |
|---|---|---|
| Tangential wind, inflow, lift and turbulence | One `SampleField` function | Yes |
| EF-style storm presets and lifecycle | Tables and script clock | Yes |
| Condensation funnel and dust | `ParticleEmitter` layers | Yes |
| Rain shaft and cloud deck | Particle emitters, `CloudCompute`, atmosphere | Yes |
| Loose debris and tree bend | Kinematic parts with script-held velocity | Yes |
| Lightning and hazard readout | Beam, point light and script UI | Yes |
| Buildings losing pieces | Script-controlled break groups | Yes |
| Full rigid-body debris collision | Engine physics body control | No |
| 1M to 50M custom compute particles | Native render compute path | No |
| Sparse cloud density octree | Native volume renderer | No |
| Procedural spatial audio | Authored audio content and mixer work | No |

## Existing engine doors

The first pass needs no engine library or component.

- `RunService.Heartbeat` runs the field, lifecycle, moving debris and controls.
- `Part`, `Model`, `CFrame`, `Vector3` and `math.noise` are enough for the
  analytical field and a bounded kinematic scene.
- `ParticleEmitter` already supplies disc and cylinder shapes, inward and
  outward emission, tangential and radial acceleration, noise, drag, size and
  transparency curves. GPU simulation owns the visual particles.
- `SkyboxCompute`, `AtmosphereProcedural`, `CloudCompute`, `PointLight`,
  `Beam`, `Trail` and `Sound` cover the weather presentation surface.
- Screen GUI controls and labels are available from Luau.

## First demo slice

`mono.engine/examples/TornadoSim.luau` will contain all gameplay state. It
will build a storm plain, a single travelling vortex, a few structures and a
camera. It will expose a compact preset selector and a live hazard display.

The authoritative function samples an offset from the storm centre and returns
the local velocity and intensity. It uses the same stable shape as TornadoSim:

1. A softened maximum-wind ring.
2. Outer radial inflow with a smooth influence falloff.
3. Strong central lift and weak distant circulation.
4. Height-based upper wind and deterministic turbulence.

Every consumer calls that one function. Loose debris stores position, velocity,
mass response and age in one Luau table. The script integrates it at a bounded
step, gives it a ground bounce, and writes its `CFrame`. Trees bend visually;
small building groups release when their local hazard crosses their declared
strength. The field itself remains the only source of wind truth.

Visual particles stay GPU-owned. A small number of stationary and travelling
emitters make the dust base, condensation funnel, rain curtain and upper cloud
deck. They use the same storm centre and preset values, but do not copy a CPU
particle simulation into Luau.

## Boundaries for the first pass

- Keep moving scripted debris bounded. It is a readable gameplay layer, not a
  CPU particle system.
- Do not add `ApplyImpulse`, velocity properties, a tornado component or a
  native field library before the scene proves they are needed.
- Do not claim rigid collisions, fluid behaviour, volumetric clouds or
  million-particle custom compute from this scene.
- Use no external assets for the first runnable version. A later sound pass
  needs published wind and thunder content.

## Proof gates

- Luau typecheck passes for the new example.
- A headless client run loads the script and advances the world without errors.
- A visual run shows the moving funnel, rain, debris, bend and lightning.
- The status panel reports the sampled wind and hazard at the camera.
- A release profile records the scripted debris count and particle count before
  any performance claim.

## Engine work only after evidence

Add an engine door only when the first demo hits one of these walls:

| Proven wall | Smallest follow-up |
|---|---|
| Kinematic debris is not enough | Expose velocity and impulse through existing physics bodies. |
| Particle emitters cannot follow the field shape | Add a generic field-sampling particle force, not a tornado-specific renderer. |
| The scene needs dense cloud self-shadowing | Add a generic volume resource and render node. |
| The scene needs large destruction | Build reusable script-facing break groups before a damage system. |

This keeps the demo a script consumer first. The engine earns new code only
after the script has shown the missing cut point.
