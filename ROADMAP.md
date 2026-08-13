
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

### v0.15

- [_] deferred `D00111`: listing an HTTP origin's contents — its tab names the address and says why it cannot enumerate
- [_] deferred `D00110`: a variety of default shaders, once something can select one
- [_] deferred `D00114`: no type inference — a local from `Instance.new("Part")` resolves because the class is on the line, one from `FindFirstChild` falls back to the union of every scriptable property
- [_] deferred `D00115`: a character's limbs pay wire for transforms the receiver overwrites, because replication filters by component and not by entity
- [_] svg rendering support
- [_] gif rendering support (and ensure cdn supports it too)
- [_] i also realised that i don't have a way to control whether changes are client-sided or server-sided when i edit in the explorer/properties. how could i do this considering multiple clients + server support - with run-mode only and play-mode.
- [_] fix grid projection in run/play mode - no longer shows up in the world on any viewport.
- [_] also extra prototype project for rendering pipeline.
- [_] thoroughly implement all user interface elements + surfacegui + billboardgui
- [_] thoroughly implement user input system
- [_] thoroughly implement common services
- [_] thoroughly implement extra functions like PlayerGui:...
- [_] add accessories support

### v0.16

- [_] animation handler
- [_] character controller + humanoid + character states + state controller, etc. More modular than roblox standard humanoid. state machine? node graphs? etc.

#### v0.17

- [_] add engine-level and cdn-level fast-flags for enable/disable types of content (mp4, gif, svg)
- [_] add more engine-level, cdn-level, client and server level configs.

### v0.?? (needs prototype project first)

- [x] ~/Documents/GitHub/node-graph-template
- [_] extended rendering pipeline (handle multiple worlds in parallel, handling gpu traffic)
- [_] rendering pipeline is a node system with a studio editor
- [_] surfaceapperance actually integrated with new pipeline
- [_] render pipeline is per world, not per process
- [_] render pipelines are saved in the world's export data
- [_] rendering pipeline debugger and profiler - shows a graph's per-step operations with their compute costs and wall clock, etc. also shows the images/masks/etc used for each step
- [_] rendering pipeline additions; ambient occulusion, emissive, pbr, default node setup with all these
- [_] https://www.youtube.com/watch?v=SnNm7rSSvlg (Threat Interactive Tutorial: How To Optimize Almost Every Step In Modern Game Rendering)
- [_] https://github.com/fini03/vkDuck

---

- [_] finish portals, lighting doesnt work through it properly and in the seam the character half disappears because geometry doesn't render completey render through them properly
- [_] add modulescript boundaries between luau and javascript VMs. moving values between vms.
- [_] consider adding C# as another scripting langauge?
- [_] constraints system
- [_] porting roblox games (DEFER THIS UNTIL LATER ONCE TYPES ARE BUILT UP) — untouched, and the trigger is unchanged: there are four instance classes in this engine and a Roblox place names hundreds
- [_] mip chains — `assets::Texture` is one image and has no place to put the levels, so a 2048 sheet minified onto forty pixels shimmers. `bake::ResizeImage` is already the box filter that would build the chain; it is a format change that should arrive with sampler work rather than ahead of it
- [_] skinning and animation — `bake` skips joints and weights and keeps the rest pose, because there are no skeletons in the engine yet
- [_] finish replication for server/client, server authoritative — the mechanisms are in place and what remains is a *game*: a client that sends inputs from real aim, a player entity per connection to give `SetClientViewpoint` something true to say, — `net::ConnectionStats::RoundTripMilliseconds` is measured now, which it had not been since it was declared at v0.3: `ReliableSender` samples the gap between a reliable packet and its acknowledgement, smoothed at RFC 6298's one eighth, and **skips any packet that was resent** because an acknowledgement of a resend does not say which transmission it answers. That is Karn's rule, and the first version of it was inverted — `Attempts` counts *re*sends and starts at zero, so a check for one measured precisely the packets the rule exists to exclude
- [_] deferred `D00104` — the last three rows of Rojo's file table. `.rbxmx` needs an XML parser and `.toml` a TOML parser, neither of which `mono.vendor` carries; `.rbxm` is a binary format reader that belongs beside the model decoders in `bake`.
- [_] deferred `D00106` — JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike.

