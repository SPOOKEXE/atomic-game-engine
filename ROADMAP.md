
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

### v0.12

- [x] expose ECS underlying to luau and typescript (query engine for entities using direct and returns instances, create custom components and create custom entities and attach components, set component values, etc).
- [x] rojo folder syncing + tests, support multi-world by subfoldering (main.universe.json for universe mapping, main.default.json accepted beside Rojo's own default.project.json in each subfolder)
- [x] world => rojo sync, universe => multi-rojo sync (each world syncs separately, so one bad project file costs its own world and the rest still sync)
- [x] the rest of Rojo's file table: .meta.json and init.meta.json patch properties onto whatever the file of that stem built, .model.json builds a class with its properties and children, .json becomes a ModuleScript with generated source, .txt and .csv become a StringValue and a LocalizationTable over the new scene::TextContent, and a nested .project.json is followed with a cycle check.
- [x] room in scene::Visual again — Surface, CastShadow and Locked used the original three padding bytes, so it was widened once on purpose and the next four one-byte fields are free. engine.scene.components pins the size.
- [x] expanded ecs::Schemas from 256 to 2048 described components. The cap was never the real constraint: the generated hook bodies were inlinable, so every thunk carried a copy of the PropertyType switch — 113 MB of object at 4096 slots. Held out of line it is about 192 bytes of .text a slot, and the measurements are in Schema.cpp.
- [x] update built-in MCP integrations (component_list, entity_query, component_get, component_set over the v0.12 storage surface; first suite for `control`), add a studio ui button to enable/disable and information panel
- [x] walked the deferred register. Every open entry's reopen trigger is unfired: `D00102` wants the `bake` module split, `D00039` wants a world that ticks physics (v0.13), `D00038`/`D00046`/`D00103` want a render pass executor, `D00030`/`D00031` are tooling gaps with working workarounds, and `D00014`/`D00015`/`D00018`/`D00019` are net and replication decisions waiting on a deployment.
- [_] deferred `D00104` — the last three rows of Rojo's file table. `.rbxmx` needs an XML parser and `.toml` a TOML parser, neither of which `mono.vendor` carries; `.rbxm` is a binary format reader that belongs beside the model decoders in `bake`.
- [x] create a api for setting up configs in the studio and saving them to the ~/Documents/atomic-game-engine/studio folder. Save for preferences.json, cdn.json, recent.json (last 5 projects), keybinds.json. Also materials preview now renders without a hover and fetches a missing colour map through the delivery client.
- [x] studio editing tools; select, move, rotate, 3d scene interactable gimbals, grid step amount input, rotation amount input, anchor toggle, "lock" toggle, pivot editor mode, reset pivot button.
- [x] studio tooling tabs (Home, Model, Script, View)
- [x] improved physics pipeline with spatial optimisations — the broad phase's grids size themselves from the colliders they hold (spatial::SuggestCellSize), which lands on the hand-picked optimum at every density: 22% off the default at 4000 colliders, 17% at 1000. An author who names a size still gets it.
- [x] plugin system — a plugin is a folder in the studio config directory holding a plugin.json and a script, run as its own script::Runtime against the world being edited. It reaches everything a game script does, including World; the selection crosses as the studio.Selected component rather than as an API. One runtime each and one failure each: a plugin that will not start, or that throws three beats running, is switched off and named while the rest keep running.
- [x] plugin editor API — script::HostSurface is the seam (one virtual, a value tree, no lua_State crossing a module boundary) and script::HostValue carries an Instance and a Callback where ScriptValue carries neither. A plugin creates toolbar sections and buttons with real handlers, dock widgets that draw immediate-mode like every other panel, and reads or writes the source of scripts in the scene. The selection is `game:GetService("Selection")` with `:Get`, `:Set`, `:Add` and `:Remove`, matching Roblox — a dotted host name installs a service global, which `GetService` already resolved. The three writers take an array of Instance: the wrong shape is refused with a message saying what to write instead, an item that is the wrong type or has been destroyed is skipped with one warning naming which, and an empty array means "deselect everything". `D00105` is closed.
- [x] breakpoints in Luau, in the script editor and in a tab window — a clickable gutter beside the code toggles one, the Debugger panel is a capture list beside a Call Stack / Locals / Upvalues tab bar, and upvalues are captured per frame and kept apart from locals. `BreakpointService` is the same debugger reached from a script, high level over a script instance and low level over a chunk name, and it is a studio's alone.
- [_] deferred `D00106` — JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike.
- [x] asynchronous mesh loading — two halves, and the second was the one that interrupted. `scene::KeepLoaded` drops an instance naming a mesh the renderer does not hold while keeping one naming no mesh at all, so a `MeshPart` is invisible until its geometry lands instead of coming up as the default cube; `delivery::IntakeBudget` caps what one frame decodes and uploads at two megabytes, deferring the rest to the next frame rather than draining a forty-mesh burst in the frame that noticed it. The first arrival of a frame is always admitted, so an asset larger than the whole budget loads in one long frame instead of never. A part naming a texture the renderer does not hold now draws render::MissingTexture — a purple-and-black checkerboard, with the base colour neutralised so it cannot be tinted into looking deliberate — rather than the default plastic a part nobody textured gets. `D00107` records that the renderer cannot yet tell a streaming sheet from one that will never arrive.

### v0.13

- [x] create a unified networking system for LAN, Peer2Peer and remote connections — `mono.network`, a `shared`-tier member above `Engine::net` and below every program, imported by the client, the server, the studio and the cdn. **It ends at an `engine::net::Endpoint` and deliberately opens no connection**: `replication::Connector` already takes one, a `delivery::Source` already names one, and a second answer to "what is a connected session" would be the most expensive kind of debt here. What is unified is the table above it — a session heard on the subnet, one listed by a rendezvous point and one typed into a config file are three rows differing in a `Reach` and nothing else, so no caller has an "is discovery enabled" branch. A host announces once a second from an ephemeral port to a well-known one, rather than answering probes, so a machine can host as many sessions as it likes and a beacon answers nothing a stranger sends it. Peer-to-peer is a real punch: the point introduces two peers with a nonce it drew, both poke until one gets through, and there is **no relay** — a pair of routers that will not cooperate produces `ReachState::Failed` rather than a hidden bandwidth commitment. `Access::Private` is a pre-shared `SessionKey` — 64 hex characters or a passphrase, PBKDF2 over a fixed salt, and the vector is locked in a suite because changing the derivation silently invalidates every key anybody was given. It authenticates and does not hide: a private session on a subnet is visible to that subnet and joinable by nobody without the key, which is why a browser lists it as locked rather than dropping it. The rendezvous point holds no keys and must not, so a private registration is unlisted rather than gated and its 128-bit id is what stands in for a listing.
- [x] the engine's half — `server --advertise --session-name --session-key --rendezvous`, `client --browse --session-id --session-key --rendezvous`. **The punch happens on the socket the session uses**, because a NAT mapping belongs to a port: `replication::Listener::SetForeign` and `Connector::SetForeign` hand a datagram that is not a `net::Packet` back to the discovery layer, and the three formats route by magic (`ATN1`, `ATNA`, `ATNR`). `engine::net` gained `TransportSettings::Broadcast` and `ReuseAddress` and `Endpoint::BroadcastIPv4` for it — and the loopback honours a broadcast, which is what makes the whole discovery protocol a path a suite exercises with real encoding, no socket and no sleeping.
- [x] the cdn's half — `cdn::Stream` and `cdn::StreamFinder`. The four distribution streams are two settings rather than four code paths, because the origin cannot tell them apart: LAN is `--advertise`, peer-to-peer and public are `--rendezvous`, private is `--stream-key`, and `--rendezvous-listen` makes the origin the meeting place other people's sessions use rather than shipping a fifth executable. A discovered stream becomes a `delivery::Source` nearest-first, read-only — uploading to whatever answered a broadcast is how content reaches a machine nobody meant to publish from. `--stream-key` gates *discovery*; a `cdn::Gate` grant still gates delivery, and collapsing the two would make the key that finds a stream the key that draws from it.
- [x] the studio's half — `studio::TeamCreate` and a View → Team Create panel at `Purpose::Studio`, which is the same `network::Presence` the client browses servers with. It holds no socket until somebody opens the panel, and hosting produces a session id and a key to hand over. **Sessions only, and the panel says so**: two editors can find each other, and editing one place together needs a change model with an ordering that this repository has only for a server's world.
- [x] `ChangeHistoryService`, Roblox's shape method for method — `TryBeginRecording`, `FinishRecording` with `Commit`/`Cancel`/`Append`, `IsRecordingInProgress`, `GetCanUndo`/`GetCanRedo`, `Undo`/`Redo`, `SetWaypoint`, `ResetWaypoints`, `SetEnabled`, and the four events. It is backed by the waypoint layer added to `CommandLog` rather than by a history of its own, because a second undo stack beside the editor's would be two answers to what Ctrl+Z does. A recording is a **named, atomic group**: one undo reverses all of it, and one message carries all of it — which is the same primitive the shared document needs, and the reason these two arrived together. `Enum.FinishRecordingOperation` is registered, and an `EnumItem` now crosses the host seam as its member's name, which is the latitude `ReadEnumValue` already gives everywhere else. Two differences from Roblox and both are the seam: `GetCanUndo` returns a table because a host call answers one value, and the events take a handler because the seam has no `RBXScriptSignal`. `PluginSurface` has no unit suite, matching the note already in `tests/Plugins.cpp` — the semantics it wraps are covered by twelve cases over `CommandLog`.
- [x] studio replication for team create — `studio::EditStream`, and it is the third of three pieces rather than a thing of its own. The unit was settled by `CommandLog`: a committed waypoint, handed over whole, because a peer that applied half of a group would show a state the author never saw. The far end was settled by `ApplyForeign`: somebody else's edit lands without entering this author's undo stack. **The middle is an identity and a bus.** The identity is an *instance path* — a list of names from the world's root — because an `EditId` is one log's own name for an instance and both editors issue `1` for their first, so the collision is not a remote possibility but what happens immediately; the suite asserts that collision rather than describing it. Every id on the wire is the receiver's own, minted as it applies. Paths stay consistent because the stream is ordered: a rename is itself a replicated write, so everybody applies it at the same point, which is why the relay goes through the host rather than peer-to-peer among the guests. The bus is `MessageKind::User` on `replication` — a widening of the connected, admitted, encrypted, reliable session that already exists, because a fourth session type beside it is the thing to refuse. No conflict resolution and no locking: last write wins, which is what Roblox's own team create does, and locking fails in the ordinary case rather than the rare one. `D00108` is closed.
- [x] two gaps `replication` had all along, found by the first caller that goes quiet. **Nothing ever called `Link::NeedsKeepAlive`** — every caller published a world every tick, so a packet always went out and carried the acknowledgement with it. A session that only carries occasional messages sends nothing between them, so the far side's reliable sender resent to its limit and gave up on a link that was up and healthy, with no symptom but sends beginning to return false. The keep-alive timer alone does not close it, because a sender reaches `MaximumResends` before a one-second keep-alive comes round — so an accepted-and-unacknowledged reliable payload now makes an acknowledgement due immediately. And an empty unreliable payload is that acknowledgement rather than a message: surfacing it as inbound handed every reader a zero-length message it could not parse, which each counted as a refusal.

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

