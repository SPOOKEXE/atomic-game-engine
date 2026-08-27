
# gpt 5.6 sol in browser with ROADMAP.md file

### Core Engine / ECS

* [ ] **Entity lifecycle system** — spawn/despawn/deferred destruction, pooling, orphan handling
* [ ] **Archetype/query optimizer** — cached ECS queries, change filters, parallel query execution
* [ ] **Component change tracking** — dirty bits/version counters so systems only process changed data
* [ ] **Entity references/handles** — generation-safe references instead of raw entity IDs
* [ ] **World snapshots** — serialize/restore an entire world state
* [ ] **World cloning** — instant-ish duplicate world for testing, previews, server simulation
* [ ] **Rollback/snapshot system** — useful for networking and deterministic simulation
* [ ] **Fixed-timestep simulation** — independent simulation/render frequencies
* [ ] **System dependency graph** — explicitly declare `before/after`, parallelize independent systems
* [ ] **Frame scheduler** — CPU jobs, GPU jobs, async jobs and synchronization points represented together
* [ ] **Engine tick phases** — Input → Simulation → Physics → Animation → Replication → Render preparation → Render
* [ ] **Determinism mode** — detect nondeterministic simulation and optionally enforce deterministic ordering
* [ ] **Hot-reloadable components/systems**

This would complement your existing batched-compute/job-system work particularly well.

---

## Performance / Profiling

You have profiling mentioned, but I'd make **performance tooling itself a first-class engine feature**.

* [ ] **Built-in profiler**

  * CPU timeline
  * GPU timeline
  * ECS systems
  * render passes
  * jobs
  * network
  * memory
* [ ] **Live ECS statistics**

  * entity count
  * component counts
  * archetype sizes
  * query timings
* [ ] **GPU residency inspector**

  * resident bytes
  * upload/download traffic
  * dirty regions
  * update frequency
* [ ] **PCIe traffic monitor**
* [ ] **CPU ↔ GPU synchronization visualizer**
* [ ] **Frame debugger**

  * inspect every render pass
  * resources
  * barriers
  * shaders
  * draw/dispatch calls
* [ ] **Memory visualizer**

  * CPU allocations
  * GPU allocations
  * fragmentation
  * per-component memory
* [ ] **Asset memory profiler**
* [ ] **Network bandwidth profiler**
* [ ] **Performance budgets**

  * e.g. physics < 2 ms
  * scripts < 3 ms
  * render submission < 1 ms
* [ ] **Automatic performance regression tests**

The GPU-resident work in your roadmap makes this especially valuable.

---

# World / Scene System

I'd significantly expand this.

* [ ] **Scene streaming**
* [ ] **World partitioning**
* [ ] **Spatial database**

  * BVH
  * octree
  * grid
  * loose octree
* [ ] **Scene hierarchy**
* [ ] **Subscenes**
* [ ] **Scene references**
* [ ] **Additive scene loading**
* [ ] **Scene instancing**
* [ ] **World layers**
* [ ] **World regions**
* [ ] **Streaming priorities**
* [ ] **Distance-based activation/deactivation**
* [ ] **Server-side interest management**
* [ ] **Persistent world chunks**
* [ ] **World origin rebasing**
* [ ] **Floating-origin support**
* [ ] **Large-world coordinates**

That last group becomes important if you're serious about the Space Engineers-style gigantic-world demos already mentioned in the roadmap.

---

# Networking

You have WebSockets and platform connection keys, but there's a **big networking layer** missing.

I'd add:

* [ ] UDP transport
* [ ] Reliable/unreliable channels
* [ ] Packet fragmentation/reassembly
* [ ] Packet compression
* [ ] Delta compression
* [ ] Entity replication
* [ ] Component replication
* [ ] Replication priorities
* [ ] Replication frequency
* [ ] Network ownership
* [ ] Client prediction
* [ ] Server reconciliation
* [ ] Lag compensation
* [ ] Interpolation
* [ ] Extrapolation
* [ ] Network rollback
* [ ] Interest management
* [ ] Spatial replication
* [ ] Network object IDs
* [ ] Remote events/RPC
* [ ] Remote functions
* [ ] Network profiler
* [ ] Packet-loss simulation
* [ ] Latency simulation
* [ ] Bandwidth throttling
* [ ] Connection migration
* [ ] Server browser
* [ ] Matchmaking abstraction

I'd probably make this its own major milestone rather than burying it under miscellaneous infrastructure.

---

# Asset System

You have CDN/world export/import, but a proper **asset database** would be extremely useful.

* [ ] Asset dependency graph
* [ ] Asset GUIDs
* [ ] Asset versioning
* [ ] Asset hashing
* [ ] Asset deduplication
* [ ] Asset caching
* [ ] Import pipeline
* [ ] Import processors
* [ ] Import presets
* [ ] Asset reimport detection
* [ ] Asset dependency invalidation
* [ ] Asset bundles/packages
* [ ] Asset streaming
* [ ] Asset hot reload
* [ ] Asset thumbnails
* [ ] Asset metadata
* [ ] Asset validation
* [ ] Missing-reference detector
* [ ] Broken-asset detector
* [ ] Duplicate-asset detector
* [ ] Dependency viewer
* [ ] Asset migration system

And particularly:

**Asset cooking/build pipeline**

```text
Source Asset
     ↓
Importer
     ↓
Processor
     ↓
Optimizer
     ↓
Compressor
     ↓
GPU-ready representation
     ↓
Package / CDN
```

That would make your Blender/Roblox/Unity/RPG Maker import ambitions much cleaner.

---

# Input

You already have `UserInputService` and `ContextActionService`, but I'd go beyond Roblox compatibility.

* [ ] Input abstraction layer
* [ ] Keyboard/mouse
* [ ] Controller
* [ ] Touch
* [ ] Pen/tablet
* [ ] Gamepad remapping
* [ ] Input contexts
* [ ] Input chords
* [ ] Input buffering
* [ ] Input recording/replay
* [ ] Input visualization
* [ ] Input action assets
* [ ] Per-player input maps
* [ ] Device detection
* [ ] Deadzone configuration
* [ ] Haptics
* [ ] Controller vibration

**Input recording/replay** is particularly useful for automated gameplay tests.

---

# AI Framework

You have pathfinding, but I think the engine could have a **general AI framework**.

* [ ] Navmesh generation
* [ ] Dynamic navmesh
* [ ] Navmesh streaming
* [ ] Hierarchical pathfinding
* [ ] Flow fields
* [ ] Crowd simulation
* [ ] Steering behaviors
* [ ] Behavior trees
* [ ] Utility AI
* [ ] State machines
* [ ] GOAP
* [ ] Blackboard system
* [ ] Perception system
* [ ] Vision cones
* [ ] Hearing
* [ ] Threat detection
* [ ] Squad AI
* [ ] Formation AI
* [ ] Cover selection
* [ ] Tactical positioning
* [ ] AI LOD
* [ ] AI sleeping/deactivation
* [ ] Multi-agent job scheduling

Your advanced climbing/pathfinding idea would fit nicely as a capability-based navigation system:

```text
AgentCapabilities
 ├─ Walk
 ├─ Jump
 ├─ Climb
 ├─ Swim
 ├─ Fly
 ├─ Crawl
 └─ OpenDoors
```

Then navigation isn't just "find path"; it's **find path satisfying capabilities**.

---

# Physics

Your collider/constraint work could eventually become a much more complete physics layer.

* [ ] Broadphase
* [ ] Narrowphase
* [ ] Continuous collision detection
* [ ] Sleeping
* [ ] Physics islands
* [ ] Trigger volumes
* [ ] Collision layers/masks
* [ ] Material system
* [ ] Friction/restitution
* [ ] Character controller physics
* [ ] Ragdolls
* [ ] Vehicle physics
* [ ] Rope/chain physics
* [ ] Soft bodies
* [ ] Cloth
* [ ] Destruction
* [ ] Fracturing
* [ ] Buoyancy
* [ ] Wind forces
* [ ] Physics constraints editor
* [ ] Physics debug renderer

And eventually:

**GPU physics / batched physics**

for huge numbers of particles, debris, projectiles, crowds, etc.

---

# Audio

You have the future DAW idea, but the actual runtime audio system deserves its own section.

* [ ] 3D positional audio
* [ ] Distance attenuation
* [ ] Doppler
* [ ] Occlusion
* [ ] Reverb zones
* [ ] Audio buses
* [ ] Audio mixers
* [ ] DSP effects
* [ ] Low-pass/high-pass
* [ ] Compressor
* [ ] EQ
* [ ] Spatial audio
* [ ] Ambisonics
* [ ] Environmental audio
* [ ] Audio streaming
* [ ] Audio streaming cache
* [ ] Audio asset compression
* [ ] Music system
* [ ] Adaptive music
* [ ] Audio snapshots
* [ ] Footstep material system
* [ ] Voice chat abstraction

---

# Testing

This is one of the biggest things I'd add.

You have security/fuzz testing and rendering tests, but I'd create a full **Engine Test Framework**.

* [ ] Unit test runner
* [ ] ECS tests
* [ ] Physics tests
* [ ] Rendering tests
* [ ] Network tests
* [ ] Serialization tests
* [ ] Script tests
* [ ] Asset tests
* [ ] Determinism tests
* [ ] Multiplayer simulation tests
* [ ] Golden-image rendering tests
* [ ] Performance tests
* [ ] Memory tests
* [ ] Fuzz testing
* [ ] Stress testing
* [ ] Soak testing
* [ ] Automated gameplay tests
* [ ] Headless engine mode
* [ ] CI test runner
* [ ] Test recordings/replays

A really powerful feature would be:

### "Record → Replay → Compare"

```text
Gameplay
   ↓
Record inputs + world events
   ↓
Replay deterministically
   ↓
Compare:
  WorldState
  ECS state
  Render output
  Network state
```

That would make debugging enormously easier.

---

# Debugging

Beyond the debugger itself:

* [ ] Time-travel debugger
* [ ] Frame rewind
* [ ] World-state snapshots
* [ ] Entity history
* [ ] Component history
* [ ] Network packet inspection
* [ ] RPC inspector
* [ ] Remote debugger
* [ ] Live variable editing
* [ ] Live component editing
* [ ] Entity watch
* [ ] Conditional breakpoints
* [ ] Data breakpoints
* [ ] Event breakpoints
* [ ] Exception breakpoints
* [ ] Script execution profiler
* [ ] Lua/JS VM profiler
* [ ] "Why is this entity here?" inspector

The last one could be surprisingly useful:

```text
Entity 1837

Created by:
  SpawnSystem

Prefab:
  Enemy.Orc

Components:
  Transform
  CharacterController
  Health
  AIController

Currently modified by:
  PhysicsSystem
  AISystem
  AnimationSystem
```

---

# Editor

Your Studio replacement is already extensive, but I'd add some more **power-user editor functionality**:

* [ ] Command palette
* [ ] Global search
* [ ] Search objects by property
* [ ] Search components
* [ ] Multi-property editing
* [ ] Multi-object inspector
* [ ] Favorites
* [ ] Bookmarks
* [ ] Recent objects
* [ ] Recent assets
* [ ] Scene bookmarks
* [ ] Camera bookmarks
* [ ] Editor layouts/profiles
* [ ] Workspace presets
* [ ] Multi-monitor layouts
* [ ] Tabs that can become windows
* [ ] Floating dock widgets
* [ ] Editor scripting
* [ ] Editor macros
* [ ] Action history
* [ ] Undo/redo transactions
* [ ] Transaction inspector
* [ ] Dependency visualization
* [ ] Reference finder
* [ ] "Find all usages"
* [ ] Asset dependency graph

### Particularly: Command Palette

Something like:

```text
> Add Component
> Create Script
> Find Asset
> Focus Selection
> Toggle Wireframe
> Open Render Graph
> Profile Frame
> Spawn Entity
> Run World
> Export World
```

This is a massive productivity multiplier.

---

# Prefab / Template System

I don't see a proper prefab system in the roadmap.

I'd definitely add:

* [ ] Prefabs
* [ ] Nested prefabs
* [ ] Prefab overrides
* [ ] Prefab variants
* [ ] Prefab inheritance
* [ ] Prefab diff
* [ ] Prefab apply/revert
* [ ] Prefab dependency graph
* [ ] Prefab editing mode
* [ ] Runtime prefab instantiation
* [ ] Prefab version migration

This would pair extremely well with your ECS/Roblox Instance shim.

---

# Serialization / Migration

Your world format is getting substantial. Add:

* [ ] Schema versioning
* [ ] Automatic migrations
* [ ] Component versioning
* [ ] Backwards compatibility
* [ ] Forward compatibility where possible
* [ ] Binary serialization
* [ ] Streaming serialization
* [ ] Incremental saves
* [ ] Autosave
* [ ] Crash recovery
* [ ] Save corruption detection
* [ ] Save diff
* [ ] Save merge
* [ ] World patch files

Especially useful:

```text
world.aworld
world.aworld.patch
world.aworld.patch2
```

rather than rewriting huge worlds for every tiny change.

---

# Reflection / Metadata

You're already effectively building reflection through components, tags and Roblox properties.

I'd formalize it.

* [ ] Runtime reflection API
* [ ] Property metadata
* [ ] Component metadata
* [ ] Enum metadata
* [ ] Serialization metadata
* [ ] Editor metadata
* [ ] Replication metadata
* [ ] Security metadata
* [ ] Quantization metadata
* [ ] Network metadata
* [ ] Inspector metadata
* [ ] Deprecation metadata
* [ ] Experimental metadata

Then something like:

```lua
property("Health", {
    type = "float32",
    replicated = true,
    quantize = "uint16",
    editor = {
        min = 0,
        max = 1000
    }
})
```

becomes possible.

This could unify several seemingly separate roadmap items.

---

# Security / Sandboxing

Your script capabilities item is important enough to expand:

* [ ] Capability-based permissions
* [ ] VM resource limits
* [ ] CPU execution budgets
* [ ] Memory budgets
* [ ] File-system sandbox
* [ ] Network sandbox
* [ ] Plugin sandbox
* [ ] Server/client separation
* [ ] Trusted/untrusted asset distinction
* [ ] Script signing
* [ ] Plugin signing
* [ ] Permission prompts
* [ ] Capability audit log
* [ ] Restricted API profiles
* [ ] Crash isolation

---

# Procedural Generation

Beyond terrain:

* [ ] Procedural meshes
* [ ] Procedural materials
* [ ] Procedural textures
* [ ] Procedural foliage
* [ ] Procedural buildings
* [ ] Procedural roads
* [ ] Procedural caves
* [ ] Procedural planets
* [ ] Procedural weather
* [ ] Procedural biome system
* [ ] Seeded generation
* [ ] Generation graphs
* [ ] Deterministic generation
* [ ] Generation caching
* [ ] Background generation workers

This would fit your node-based philosophy extremely well.

---

# Environment System

I'd make environment its own subsystem:

* [ ] Day/night cycle
* [ ] Sun/moon
* [ ] Stars
* [ ] Atmosphere
* [ ] Clouds
* [ ] Weather
* [ ] Rain
* [ ] Snow
* [ ] Fog
* [ ] Wind
* [ ] Lightning
* [ ] Volumetrics
* [ ] Water
* [ ] Ocean simulation
* [ ] Waves
* [ ] Underwater rendering
* [ ] Weather zones
* [ ] Dynamic sky

Some of this overlaps with your render roadmap, but the **simulation/environment layer** should be separate from the renderer.

---

# Build / Release System

A proper engine needs a first-class build pipeline.

* [ ] Project build system
* [ ] Development build
* [ ] Shipping build
* [ ] Dedicated server build
* [ ] Headless build
* [ ] Client-only build
* [ ] Asset cooking
* [ ] Shader cooking
* [ ] Script compilation
* [ ] Dependency stripping
* [ ] Feature stripping
* [ ] Platform-specific builds
* [ ] Build cache
* [ ] Incremental builds
* [ ] Build profiles
* [ ] Release channels
* [ ] Versioning
* [ ] Crash reporting
* [ ] Symbol management

---

# Platform Layer

You already have `MOBILE.md`, so I'd expand platform abstraction:

* [ ] Windows
* [ ] Linux
* [ ] macOS
* [ ] Android
* [ ] iOS
* [ ] Web
* [ ] Steam Deck
* [ ] Console abstraction
* [ ] GPU capability detection
* [ ] CPU capability detection
* [ ] Dynamic quality scaling
* [ ] Device profiles
* [ ] Resolution scaling
* [ ] Battery/thermal awareness
* [ ] Mobile memory budgets

---

# One particularly interesting thing: an Engine Graph

Given how much of your roadmap is already becoming node-oriented, I'd seriously consider making **the engine itself graph-driven**.

Not just rendering.

You could have:

```text
                    ┌──────────────┐
                    │ Input Graph  │
                    └──────┬───────┘
                           ↓
┌───────────┐       ┌──────────────┐
│ AI Graph  │──────→│ World Graph  │
└───────────┘       └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │ Physics Graph│
                    └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │Animation Graph│
                    └──────┬───────┘
                           ↓
                    ┌──────────────┐
                    │Render Graph  │
                    └──────────────┘
```

And underneath:

```text
Engine Graph
    │
    ├── dependencies
    ├── scheduling
    ├── CPU jobs
    ├── GPU jobs
    ├── synchronization
    ├── resource lifetime
    └── profiling
```

That could become one of the **defining architectural features of Atomic Engine**, rather than it simply being "a Roblox-compatible engine with a good renderer."
