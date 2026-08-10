
# ROADMAP

## Editing

Do NOT add new deferred work as a roadmap item. Place it in
`docs/DEFERRED.md`. If a TODO item is not FULLY completed, split the
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

### v0.14

- [_] rojo test project, e.g. ~/Documents/GitHub/raceapet and see if code properly syncs. maybe setup env variable for it (you just set the rojo project json for any path and it auto checks), only check code
- [_] proper plugins tab
- [_] convert topbar tools to tab-based
- [_] also add default assets into engine/resources/[meshes|textures|etc] folder that are within the engine. Put shape meshes in there and a pink and gray checkerboard texture. ensure its registered in assets menu under "engine" tab, put each cdn as a separate tab as well. add a "all" tab.
- [_] support raw cdn folder without any processing (cache temporarily in memory instead, add a checkbox option for "memory-only" which is enabled by default)
- [_] create a C++ node library suite via imgui for ~/home/declan~/Documents/GitHub/node-graph-template.
- [_] setup local cdn tutorial in SETUP-CDN.md. folder-based + setup cdn server on localhost (of which you expose for remote).
- [_] investigate what it takes to support Non-Euclidean Worlds Engine (transcript for https://www.youtube.com/watch?v=kEB11PQ9Eo8), potentially add? it seems to be possible with just cameras + portal parts
- [_] variety of default shaders - engine/resources/shaders. Move all of them to here from render/shaders/*.frag *.vert.

### v0.15

- [_] blocky character spawning
- [_] animation handler
- [_] character controller + humanoid + character states + state controller, etc. More modular than roblox standard humanoid. state machine? node graphs? etc.
- [_] local server (one server multiple clients on same machine)
- [_] check teleportservice works between worlds and it works in studio natively
- [_] also extra prototype project for rendering pipeline

### v0.16

- [_] thoroughly implement all user interface elements + surfacegui + billboardgui
- [_] thoroughly implement user input system
- [_] thoroughly implement common services
- [_] thoroughly implement extra functions like PlayerGui:...
- [_] add accessories support

### v0.?? (needs prototype project first)

- [_] ~/Documents/GitHub/node-graph-template
- [_] extended rendering pipeline (handle multiple worlds in parallel, handling gpu traffic)
- [_] rendering pipeline is a node system with a studio editor
- [_] surfaceapperance actually integrated with new pipeline
- [_] render pipeline is per world, not per process
- [_] render pipelines are saved in the world's export data
- [_] rendering pipeline debugger and profiler - shows a graph's per-step operations with their compute costs and wall clock, etc. also shows the images/masks/etc used for each step
- [_] rendering pipeline additions; ambient occulusion, emissive, pbr, default node setup with all these
- [_] https://www.youtube.com/watch?v=SnNm7rSSvlg (Threat Interactive Tutorial: How To Optimize Almost Every Step In Modern Game Rendering)
- [_] https://github.com/fini03/vkDuck
