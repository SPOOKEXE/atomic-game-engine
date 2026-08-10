
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

