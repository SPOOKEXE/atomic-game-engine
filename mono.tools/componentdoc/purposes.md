# What each ECS component is for

**This is the file to edit. `docs/ECS_COMPONENTS.md` is output.** `just
components` regenerates the catalogue from `ecs::Components` and merges these
lines into it; `just components-check` fails when a registered component has no
line here, or a line here names something nothing registers.

One component per line, `name | purpose`. A blank line or a line starting with
`#` is prose and is skipped, so this file reads as a document as well as
parsing as a table.

Keep a purpose to one sentence where it fits, two at most, and under forty
words. It lands in a markdown table cell, so no newlines and no bare `|`.

Say what the component **is**, and where it matters say what reads or writes
it. Do not restate the size, the alignment or the serialisation - those are
columns the tool fills in, and a hand-written copy of a generated fact is the
one that goes stale.

Where a row is a per-world singleton rather than per-entity data, say so. It is
the fact a reader most needs and the one the table cannot show.

## `ecs`, `world`, and the modules above `scene`

ecs.WorldTime | Per-world singleton clock: simulated seconds elapsed, the fixed tick delta, completed ticks, and the frame's wall delta and interpolation alpha.
ecs.DirtyBits | One row's changed-component bits, one bit per column position in its archetype's sorted set, so marking a write is an index the store already holds.
ecs.NotArchivable | A tag meaning this instance is left out of a save. `Archivable` is `!Has<NotArchivable>`, which is the same fact read the cheap way round.
ecs.AttributeTable | Per-world singleton holding every instance attribute a game has set, as a map per entity keyed by interned attribute name.
ecs.Hierarchy | Parent, first and last child and both sibling links for one instance, which is how the instance tree is stored rather than as a child vector per node.
ecs.InstanceClass | Which registered class an entity was created as, so `ClassName` and `:IsA` are a column read rather than a lookup in a side index.
ecs.InstanceName | An instance's name, interned, so a thousand parts called "Part" cost one string and comparing two names is an integer compare. Names are not unique.
effects.Beam | An authored beam drawn between two attachments: colour and transparency along its length, the texture and its scroll, end widths and curve control.
effects.Decal | A single image projected onto one face of its parent BasePart, with colour, transparency and draw order.
effects.EmitterSlot | Which row of the particle pool's block table an emitter owns, kept on the emitter's own row so the per-frame passes read a column instead of a hash map.
effects.ParticleEmitter | The authored settings of one particle emitter: size, colour, transparency and squash over a particle's life, the spawn shape and rate, and the material and flipbook facts.
effects.ParticleSystem | Per-world singleton particle pool: the particle slots a step writes, the per-emitter blocks, the free lists that hand slots and blocks out, and last step's statistics.
effects.RibbonBuffer | Per-world singleton holding the vertices and per-ribbon runs that this frame's beams and trails were built into, ready for the renderer.
effects.Trail | A trail following two attachments: its authored colour, transparency, lifetime and texture, plus the ring of recorded edge points it is drawn from.
effects.Texture | A tiled image projected onto one face of its parent BasePart, including tile size, offset, colour, transparency and draw order.
examples.Orbit | Demo-only component that carries an entity round a fixed centre at a set radius, height, starting phase and angular speed.
examples.Spin | Demo-only component that turns an entity at a fixed rate about its local X, Y and Z axes.
graph.PipelineSet | Per-world singleton holding the named render pipeline documents a game file carried. A legacy load path: `game` reads it once and then removes it.
physics.PoppercamState | Per-world singleton holding the blocker the camera pass last faded, so the next call clears exactly that one and nothing else.
physics.PhysicsClock | Per-world singleton physics clock: the step rate, simulated time owed but not yet spent, the running step's length, and which step of the tick it is.
physics.PhysicsWorld | Per-world singleton holding the broadphase grids, collider proxies, contact manifolds and solver arrays that one physics step builds and walks.
replication.SnapshotBuffer | Per-world singleton on a replicated world: a ring of received poses per entity plus the render clock, sampled at a fixed delay behind the newest tick.
script.CodeSourceContainerSelector | Which language container the script actually runs, and the one part of the script trio a game may set at run time. Absent means Luau.
script.Disabled | A tag: the host must not run this script. Presence moves it to a different archetype so the run loop never visits the row at all.
script.JavaScriptSourceContainer | Where a script's JavaScript program is read from, as an asset-relative path. A separate component, so a world of Luau scripts pays nothing for the column.
script.LuaSourceContainer | Where a script's Luau program is read from, as an asset-relative path. Deliberately not scriptable, which is the sandbox boundary rather than a preference.
script.Program | The mirrored text of the source a client-runnable script points at, with the path it was read for as the freshness key. Written only by the mirror pass.
script.SourceCache | Per-world singleton table of script text keyed by asset path, in the order programs were first set, with a write counter that makes noticing a change cheap.
world.BusBudget | Per-world singleton capping bus traffic: how many requests this world may make per tick, and how many it has spent since the last barrier.
world.Inbox | Per-world singleton holding what reached this world at the last barrier, sorted by sender and sequence, and replaced wholesale each barrier rather than appended to.
world.Outbox | Per-world singleton holding bus requests this world has made and not yet handed to the driver, in order, with the ticket and sequence counters that number them.
world.Replica | Marks a world as a mirror of one the server owns, naming the world it mirrors and whose copy it is. A replica may read its inbox but must never write to a bus.

## `scene`

scene.ActiveCamera | Resource: which entity the world is currently looked through, and the aspect ratio of whatever is drawing it. The matrices are not here: every consumer builds them against its own target with `ResolveCamera`.
scene.AnimationClip | On an `Animation` instance: which clip and which `Skeleton::Rig` its channels were authored against, so playing a fox's walk on a dragon is refusable.
scene.AnimationTrack | One clip playing on one animator: its play head, speed, current and target weight, fade time, priority, loop flag and whether it is running. Storage for the v0.24 animation handler.
scene.Animator | On an `Animator` instance: which rig it poses, whether the root channel moves the body and by how much, and whether the pose may be evaluated less often at distance.
scene.Atmosphere | Per-world scattering authored on an `Atmosphere` instance under `Lighting`: the air's colour and decay, its density and offset, and the sun's glare and horizon haze. Presentation only.
scene.AtmosphereProcedural | Extra scattering controls on an `AtmosphereProcedural` instance: planet and atmosphere scale, Rayleigh and Mie strength, and bounded integration quality for the resident environment compute pass.
scene.Attachment | A named point on a part: the authored local `Frame` plus the `WorldFrame` every host recomposes each tick. The cache puts an emitter and a lamp where their part is, and its reported write is what signals a change.
scene.AudioState | Resource: the world's one ear and master gain - listener mode, listener instance and volume, set through `SoundService` and consumed by the client mixer.
scene.AwakeWorld | Held by an entity that wants the world to keep ticking, with a required `Reason` naming why. `world::DecideLifecycle` walks these rows.
scene.Bounds | Half the extent of a part on each local axis. Render culling reads it every frame, the broad phase every tick, and the `Size` property writes it.
scene.Bone | One joint of a rig on a `Bone` instance: its rest frame, the animated offset on top of it, its inverse bind frame, its resolved world frame, and its palette slot and parent slot.
scene.Camera | The lens: vertical field of view, near plane and far plane. It deliberately holds no aspect ratio, because that is a fact about a window and not about the world.
scene.Clouds | A cloud layer authored under `Lighting`: its lit colour, how much sky it covers and how opaque that is, and the speed and heading it drifts at. Presentation only.
scene.CloudCompute | Voxel-like cloud generation controls on a `CloudCompute` instance: cell and layer dimensions, fractal detail, deterministic seed and bounded ray-march quality for the resident environment texture.
scene.Constraint | A generic six-degree-of-freedom joint between two attachments: a motion mode and a limit per axis, plus the drive target, stiffness, damping and force caps. Each Roblox constraint class is a prototype of this one row.
scene.CameraController | Resource: how this viewer's own eye is driven - subject, orbit angles and distance, zoom and sensitivity limits, camera mode, and the poppercam distance override.
scene.Character | On a character `Model`: handles to its root part, its `Humanoid` and the owning `Player`, null for an NPC. Controls, tools and camera code all start here.
scene.CharacterChanges | Resource: the ordered queue of character arrivals and departures since the last drain, emptied into the `CharacterAdded` and `CharacterRemoving` script signals.
scene.CharacterLimb | On a rig limb or an equipped tool's handle: which root part it hangs off and its rest pose in that root's own frame, posed every tick.
scene.Collider | The collision shape: kind, extent or baked geometry name, layer and mask, and whether contacts are only reported rather than solved. Read by both physics phases every tick.
scene.CollisionShapes | Resource: the world's table of baked convex hulls and triangle meshes, looked up by the name a `Collider::Geometry` field carries.
scene.ControllerState | Resource: this host's mapped gamepad and raw joystick state for up to eight local devices, including connection changes and sticky button edges consumed by gameplay and scripts.
scene.EditableImage | Script-drawable RGBA8 pixels with their width and height, plus a revision the client watches to know when to re-upload the texture.
scene.EditableMesh | Script-built geometry: positions, normals, UVs, colours, alphas and indices, plus a revision the client watches to know when to re-upload the mesh.
scene.EditableMeshCollision | Resource: which revision of each `EditableMesh` already has a collision shape baked for it, so a mesh a script is still editing is baked once per change and not once per tick.
scene.Humanoid | The character controller's state: move direction, walk and jump speed, capsule size, health, and the grounded, jump-requested and enabled latches the movement pass reads every tick.
scene.InputState | Resource: this host's keyboard, mouse and focus state for the current frame, with last-frame copies and sticky press edges. It is a machine's own input, never another's.
scene.Light | A point, spot or surface light: colour, brightness, range, cone angle, face and enabled flag. The client walks these rows and fills its lighting uniforms.
scene.LightingService | On the single `Lighting` service instance: ambient and outdoor ambient colour, fog colour and range, sun brightness, time of day and geographic latitude.
scene.LevelOfDetail | The coarser versions of a part's geometry: up to three extra mesh names, the triangle fraction each keeps, how the levels were produced, and the projected area per triangle `SelectLevel` targets.
scene.LocalPlayer | Resource: the `Player` this host is looking through, or null on a server. It backs the `Players.LocalPlayer` property.
scene.LocalTransparency | A per-viewer override of `Visual::Transparency`, written only through `SetLocalTransparency`, that fades a part standing between the camera and what it is watching.
scene.MaterialCatalogue | Resource: the derived table of texture sets per material name, filled by the content pump and read by `ResolveMaterials`. It is not authored and not saved.
scene.MaterialRef | On a `Material` instance: which material asset it names and which shader draws the parts wearing it. `ResolveMaterials` reads it onto every such part.
scene.MeshCatalogue | Resource: what the content pump learned about each loaded mesh - triangle count and the texture sheets its submeshes name. It backs `MeshPart.TrianglesCount`.
scene.Motion | Linear and angular velocity in world space. Physics integrates it every tick for every body carrying `Simulated`; gravity and the control pass write it.
scene.NetworkOwner | Which `Player` simulates this body; a null handle means the server does. `ReclaimAbandonedOwnership` scans it every tick and clears owners that have gone.
scene.PhysicsProperties | Per-part density, friction and elasticity overrides, used by the narrow phase and by the mass computation only when the `Custom` flag is set.
scene.Pivot | The handle an instance is posed about, stored in its own frame and composed as `Transform::Frame * Offset`. `PivotOf`, `PivotTo` and the editor gizmo read it.
scene.PlayerCharacter | On a `Player`: the character `Model` it currently owns, or null between death and respawn. It backs the `Player.Character` property.
scene.PlayerIdentity | On a `Player`: the stable `UserId` a game keys saved data by, the display name shown instead of the instance name, and this player's own respawn delay.
scene.PlayerNetworkComponent | On a `Player`: extra artificial one-way latency in milliseconds, so a host can test under a worse connection than the one it actually has.
scene.PlayerRespawn | Present only between losing a character and gaining the next, and holds the tick `UpdateRespawns` will spawn the replacement on.
scene.PlayerTeam | On a `Player`: which `Team` instance it belongs to. A player on no team simply has no row.
scene.PlayersService | On the single `Players` service instance: the admission cap, the next auto-assigned user id, the default respawn delay, and whether characters load automatically.
scene.Portal | On a portal pane: the part it leads to, which world's contents it shows, and whether it is on. A missing destination falls back to behaving as a mirror.
scene.PortalProxy | A piece of the far room, made and unmade inside a single tick, so a body standing in a portal has the other side's floor under it. Never replicated.
scene.PortalTransit | How many times a body has been through a portal seam and what yaw the last crossing turned it by. `CrossPortals` writes it and it travels with the body.
scene.PortalTransitSeen | Which `PortalTransit::Serial` this viewer has already snapped its interpolation for, so one crossing is corrected once and never twice.
scene.PostProcessing | Resource: the fragment shader that replaces the engine's own tonemap for this world. An invalid name leaves the default pass in place.
scene.PreviousTransform | Where `Transform::Frame` stood when the current tick began. The presentation pass blends between the two so drawing stays smooth between ticks.
scene.PublishedCatalogue | Resource: the published mesh names in manifest order, as the content pump saw them. It backs `ContentService:GetPublishedMeshes`.
scene.Rendered | Marks exactly the entities a draw list should contain, added and removed only by `SyncRendered`; the `Mark` byte is that walk's own scratch and is zero between passes.
scene.RenderedSignature | Resource: a rolling hash of the instance tree `SyncRendered` last ran against, so the walk can early-out on a frame where nothing structural moved.
scene.RigidBody | Mass, linear and angular damping, and body kind for a physics body. Gravity queries it every tick and the contact solver reads it per contact.
scene.Service | On each service instance: who may see its children, and whether an author is allowed to delete or reparent it. Checked at install and at lookup.
scene.ShaderSource | The fragment-stage GLSL a `ShaderScript` holds, verbatim and not interned, with a revision bumped on every write so a compiler knows when to rebuild.
scene.Simulated | Tag meaning physics owns this body's motion. `Anchored = false` adds it and `Anchored = true` removes it; every dynamic query filters on its presence.
scene.SkyboxCompute | Procedural sky controls on a `SkyboxCompute` instance: zenith, horizon and ground colours, deterministic stars and sun size, generated into one resident environment texture.
scene.SkyboxTextures | Six CDN texture names on a `SkyboxTextures` instance, one per cube face. Only the first such instance below `Lighting` is selected and demanded.
scene.Skeleton | On a skinned drawable: what the file called the rig, and how many palette slots the mesh's vertex joint indices may name. `Bone` rows under it are the joints.
scene.Sound | What a sound is rather than a sound playing: asset name, volume, roll-off distances, looped and playing. The client's mixer walks these rows every frame.
scene.SpawnLocation | On a spawn pad: which team colour it serves, whether it takes anyone regardless, and whether it is a spawn at all. `FindSpawn` reads all three.
scene.Surface | The physical material name a part feels like, resolved against the world's `SurfaceTable` once per contact. Separate, on purpose, from what the part looks like.
scene.Sun | Per-world singleton directional light: the direction it shines and the ambient standing in for sky on the faces it misses.
scene.SurfaceAppearance | The seven texture maps, shader name, alpha mode and cutoff a drawable is rendered with. `ResolveMaterials` writes it and the PBR paths read it.
scene.SurfaceBounces | Resource: how deep a mirror may show another mirror, or zero to let the engine decide. Set through the `workspace.SurfaceBounces` property.
scene.SurfaceCamera | On a mirror or portal pane: render-texture size, redraw cap, tag filter, post-grade, which face it projects off and which surface slot it writes.
scene.SurfaceLens | The off-axis frustum, oblique clip plane and pane mapping `AimSurfaceCameras` fits to a mirror or portal every frame. Derived from where the local eye stands, never authored.
scene.SurfaceLimit | Resource: how many surface panes may be drawn at once, from zero upward. Set through the `workspace.MaxSurfaces` property.
scene.SurfaceTable | Resource: the world's material table, mapping a `Surface::Material` name to the friction and restitution the narrow phase combines with.
scene.TagTable | Resource: the registered tag names, at most thirty-two, whose index is the bit `Tags::Mask` sets. A mask means nothing without the table beside it.
scene.Tags | One bit per registered tag, named by the world's `TagTable`. Read by tag-filtered surface cameras and by every `CollectionService:GetTagged` call.
scene.Team | On a `Team` instance: the side's colour, which is the thing spawn pads are matched against. Deliberately nothing else.
scene.TextContent | The text a `StringValue` or `LocalizationTable` holds, verbatim and not interned. Written through the `Value` property and by Rojo `.txt`/`.csv` sync.
scene.Terrain | Resource: how a world's ground is generated - the node graph, the seed, chunk extent and resolution, vertical extent and how far chunks are kept. The recipe is stored and the ground it makes never is.
scene.TextureCatalogue | Resource: the flipbook facts - grid, frame count and rate - the content pump learned about each loaded texture.
scene.Tool | On a `Tool` instance: where its handle sits relative to the grip point. `EquipTool` and the grip pose read it, and it decides where a held handle is drawn.
scene.Transform | Where a thing is: a world-space CFrame, never relative to a parent. The component almost every system reads.
scene.Transient | Marks an instance made by whoever is looking rather than by the world's author, so the game-file writer leaves it out of a saved `.agame`.
scene.Visual | What a drawable looks like: mesh, tint, transparency, visibility, shadow casting, editor lock, and which mirror surface it shows. The draw-list walk reads it every frame.
scene.WorldBounds | Resource: how far the world reaches from the origin on each axis. Camera framing, the bounce loop and wire quantisation all read it.

## `gui`

gui.Adornment | The half of a 3D adornment this module owns: which instance a `SelectionBox` or handle adornment is drawn around, in what colour, and whether it draws.
gui.AspectRatio | `UIAspectRatioConstraint`: forces the parent element's resolved size to a width-over-height ratio, derived from whichever axis dominates.
gui.Background | The box a `GuiObject` draws for itself: fill colour and transparency, plus the border's colour, thickness and inset mode.
gui.Billboard | What a `BillboardGui` adds: the adornee it hangs off, its stud and extents offsets, lighting, and the distance past which it stops drawing.
gui.Button | What makes a `GuiButton` a button: whether the fill shifts under the pointer and further on press, plus two reserved flags.
gui.Canvas | The screen-sized rectangle a `ScreenGui` collects onto, in pixels. Derived, because a screen gui authors no canvas: its canvas is the viewport.
gui.Corner | `UICorner`: the radius the parent element's corners are rounded by, resolved against the parent's smaller axis.
gui.DragDetector | `UIDragDetector`: makes the parent element draggable, deciding what a pointer drag does to `Element::Position` and how far it may move.
gui.Element | The base row every `GuiObject` has: position, size, anchor, rotation, draw order and the visibility, clipping and input flags the layout pass reads.
gui.Entry | What makes a `TextBox` editable: the placeholder text and colour, the multi-line and editable flags, and the caret and selection offsets.
gui.FlexItem | `UIFlexItem`, on the flexed child rather than on the layout: how one element grows into or shrinks out of a flexed list's spare room.
gui.Gradient | `UIGradient`: a colour and transparency ramp multiplied into whatever the parent already draws, at an authored angle and size-relative offset.
gui.GridLayout | `UIGridLayout`: places the parent's children in equal cells on a grid, with a cell size, cell padding, fill direction and start corner.
gui.Group | What a `CanvasGroup` composites its subtree with: one colour and one transparency applied to the whole group rather than to each child.
gui.GuiServiceState | `GuiService`'s own state: the selected element, the focused `TextBox`, whether a platform menu covers the game, and whether selection may seed itself.
gui.HandleShape | Where a `HandleAdornment` sits relative to its adornee and how big it is, as an offset `CFrame` and a stud extent.
gui.Label | The text a `TextLabel`, `TextButton` or `TextBox` shows: the string, font, size, colour and alignment, with the wrap, scale and rich-text flags.
gui.Layer | What every `LayerCollector` shares: display order, whether it is enabled, `ZIndex` behaviour, whether it resets on spawn, and the top-bar inset.
gui.ListLayout | `UIListLayout`: stacks the parent's children along one axis, with padding, alignment, sort order, flex behaviour and wrapping.
gui.Padding | `UIPadding`: space held back inside the parent element on each of its four edges before its children are placed.
gui.PageLayout | `UIPageLayout`: shows one of the parent's children at a time and slides the rest aside, with a tween time, easing curve and circular wrap.
gui.PageMotion | Engine state for a sliding `UIPageLayout`: which pages it is between, when the slide began, and how far along the eased curve it is.
gui.Picture | The image an `ImageLabel` or `ImageButton` shows: the asset name, tint, scale mode, slice and tile settings, and the hover and pressed swaps.
gui.Resolved | Where the layout pass actually put a 2D element: absolute position, size and rotation, the clip rectangle, the drawn text size and the paint order.
gui.Scale | `UIScale`: a factor multiplied into the parent's resolved size and text size after layout, so scaling a container does not re-flow its contents.
gui.ScrollMotion | Local overscroll state for a `ScrollingFrame`: how far a drag has pulled the canvas past its end, and the spring returning it after release.
gui.ScrollState | What the layout worked out about a `ScrollingFrame`: the pixel canvas extent, the visible window after any bar inset, and the two thumb rectangles.
gui.Scrolling | What a `ScrollingFrame` authors: the canvas size and position, which axes scroll, and the bars' thickness, colour, images and inset.
gui.Selection | Where a gamepad's selection goes from a `GuiObject` in each direction, which object is drawn over it while selected, and its seeding order.
gui.SelectionOutline | The box a `SelectionBox` or `SelectionSphere` draws: the line thickness, and the filled face's colour and transparency.
gui.SettingsMenuExtensions | Resource: script-authored actions appended to the local escape menu, with stable ids, labels and the callback token dispatched when a player activates one.
gui.SizeLimits | `UISizeConstraint`: clamps the parent element's resolved size between a minimum and a maximum, in pixels.
gui.SpatialCanvas | Where a `SurfaceGui` or `BillboardGui` was resolved to for the display looking at it: its pixel size, world plane, lighting and draw distance.
gui.Stroke | `UIStroke`: an outline drawn around the parent outside its own border, with its own colour, thickness, transparency, join and sizing.
gui.Surface | What a `SurfaceGui` adds: the adornee part and which face, how the pixel canvas is sized, its lighting, z-offset and draw distance.
gui.TableLayout | `UITableLayout`: lays the parent's children out as rows and their children as cells, so one column is the same width in every row.
gui.TextSizeLimits | `UITextSizeConstraint`: clamps the pixel size a scaled label may pick between a floor and a ceiling.
gui.Viewport | What a `ViewportFrame` renders into itself: the camera to render from, the frame's own ambient and directional light, and a tint over the result.
