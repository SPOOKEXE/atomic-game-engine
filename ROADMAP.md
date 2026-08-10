
# ROADMAP

## Editing

Do NOT add new deferred work as a roadmap item. Place it in
`docs/DEFERRED.md`. The existing `Deferred:` blocks below are version history
and pointers to that register. If a TODO item is not FULLY completed, split the
TODO item, keeping the concise short dash point list with block infront and
split it as another item under the same version.

For example, if we complete A and B but not C and D:
```
### v0.5
- [x] do: A1, A3, A5
- [_] do: A, B, C, D
- [x] do: G, H, I
- [x] do: K, K2, K3
```

becomes:
```
### v0.5
- [x] do: A1, A3, A5
- [x] do: A, B
- [x] do: G, H, I
- [x] do: K, K2, K3
- [_] do: C, D
- [_] deferred `D0001` for later.
```

or defer to another version.

## VERSIONS

The milestone headings below are development labels. Not in line with project versioning.

- [x] the ribbon, and the gizmo work that came with it. The Tools panel became the toolbar's second row — it floated over the viewport it was editing and carried a second copy of the manipulators the toolbar already had, so the same three buttons existed twice and were free to disagree. Four tool actions are registered commands now (`Select Tool`, `Move Tool`, `Rotate Tool`, `Scale Tool`), which makes the ribbon button, the palette and a binding one answer rather than three; they ship unbound and viewport-scoped, because `tests/Keybinds.cpp` holds the rule that a default nobody asked for is how one key came to mean two things in three files, and a viewport scope is what lets them be plain digits at all. **A move drag flickered between where the part was and where it was being dragged to**, and the cause is worth writing down: `Grabbed` is a distance along the axis from the selection's centre at grab time, and the centre was being read live — so once a frame applied the delta, the next frame measured from a centre that had already moved, got zero, and put everything back. Alternating. The fix is one captured `Centre` and every ray measurement taken from it; the handles are still drawn at the live centre, because a manipulator left behind is the next thing that looks broken. Move and Scale grew an arm on both ends of each axis, since one arm is a handle that cannot be reached from half the angles somebody orbits to. And a resize now says which faces it moves: `Side` holds the far face still (the default, and what "drag this face" means), `Both` moves both by the step, `Both Half` moves both by half of it — the last two differ in a way a checkbox could not say, which is why it is a list. `Side` is the only drag that writes a size *and* a placement, so the release opens a recording: one waypoint for the whole drag, which also fixed a multi-part drag taking one press of Ctrl+Z per part.

- [x] Select drags, and two buttons about what a drag does. **Select stops being "no tool"**: press on a part and pull, and it goes where the pointer does — resting on whatever the cursor is over, as far into it as the box's own corner reaches, which is `SupportAlong` and is why a turned part sits on a slope instead of sinking into it. Dragging something unselected selects it first; dragging something already selected carries the whole selection by one rigid transform taken from the part that was grabbed, because moving each member to the cursor would pile a built thing into one place. The raycast leaves the dragged parts out of the index rather than filtering them from the hit — the same argument locking already made, one door along: a part that swallowed its own ray would rest on itself and never reach the floor. **Align** turns a dropped part onto the surface it lands on, projecting the old facing onto the new plane rather than discarding it, so parts dropped on one wall do not all end up pointing the same way; `AlignedTo` is a named function with a suite over it because `LookVector` is `-Z` and a mirrored basis is invisible on anything symmetrical. **Facing** draws the look out of the front face to a ball with a ring round it and an arrow at the point that is up — a box says nothing about its orientation, and two parts sitting identically may be turned a quarter apart. Both toggles and the scale-side list ride in `preferences.json`.

- [x] surface effects — `Enum.SurfaceEffect` on a `SurfaceCamera`, and the mirrors example gives each of its four walls one: `NightVision`, `Thermal`, `Cctv`, `Swirl`. **A grade on the way out, not a second render**, which is the whole reason four of them are affordable: the surface pass draws the world exactly as it would have, and the effect is applied in `opaque.frag` where the pane samples the texture — no extra target, no extra pipeline, no extra bind, and switching one at runtime redraws nothing. It also means a reflection of a reflection is graded once, since the texture holds an ungraded picture and only the pane showing it grades. **A closed list rather than a shader name, and that is a limit rather than a stub**: this pipeline has one fragment program for opaque geometry, so a mirror naming an arbitrary one would need a pipeline per program, a compilation path and an answer for a file missing on somebody else's machine — the render graph below, not a field. `Thermal` reads luminance as temperature, which is a lie an engine with no thermal model cannot avoid and `scene::SurfaceEffect` says so out loud. `Swirl` is the one entry that moves texels, so it warps the coordinate before the fetch while the other three grade the colour after it — doing either at the other's moment is doing it to the wrong thing. The uniform got a field of its own rather than the spare lanes in `Surface` or `Flipbook`, because `DrawSlots` rewrites both wholesale per submesh and anything parked there would present as "the effect only works on some parts". The component's byte came out of `SurfaceCamera::Reserved`, which is what a named reserve is for, and `tests/Components.cpp` holds the sum so the day it runs out is a failing case rather than a hole in a snapshot.

- [x] deferred `D00107` closed — a streaming texture and one that will never arrive were the same picture. A sheet lands at least a frame after the mesh that names it, because the intake loop asks for a mesh's own sheets *while decoding the mesh*, and `delivery::IntakeBudget` may spread them over several more — so every imported model wore the purple marker on a scene load, indistinguishable from forty misspellings. **The marker now means only "nothing is coming"**, which is the only meaning that is useful: `render::ChooseTexture` is the rule and a name still in flight draws the default white plastic. Not a timer, and the deferred entry was right that this was the temptation — a grace period hides a genuinely missing texture for exactly as long as it hides a streaming one, and with a byte budget in the path there is no N right for both a small scene and a large one. The renderer is *told* rather than asking, because what is in flight belongs to the content pump and `render` must not reach up into it. **The load-bearing half is the unmark**, and the entry warned that skipping it would be worse than not doing this at all: a host that unmarked only on arrival leaves a misspelled sheet expected for ever and the marker never appears for the one case it exists for. A failed request carries no name — `Take` answers nothing — so `delivery::AssetClient::NameOf` was added for it, and both hosts read the name before taking because a take is what destroys the record. `MeshGrid.luau` and `Meshes.luau` each gained a part naming a sheet nobody published, so both scenes show all three answers side by side and the timing distinction is visible without reading a log.

### v0.14

- use tools and see what needs fixing
- use tools and see what to improve

### v0.?? (needs prototype project first)

- [_] extended rendering pipeline (handle multiple worlds in parallel, handling gpu traffic)
- [_] rendering pipeline is a node system with a studio editor
- [_] surfaceapperance actually integrated with new pipeline
- [_] render pipeline is per world, not per process
- [_] render pipelines are saved in the world's export data
- [_] rendering pipeline debugger and profiler - shows a graph's per-step operations with their compute costs and wall clock, etc.
- [_] rendering pipeline additions; ambient occulusion, emissive, pbr, default node setup with all these

