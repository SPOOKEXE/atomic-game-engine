
### v0.26.1

- [_] ```const char *CameraModeName(scene::CameraMode mode) {
	switch (mode) {
	case scene::CameraMode::Classic:
		return "classic";
	case scene::CameraMode::LockFirstPerson:
		return "lock_first_person";
	case scene::CameraMode::ShiftLock:
		return "shift_lock";
	case scene::CameraMode::Scriptable:
		return "scriptable";
	}
	return "unknown";
}``` move all character management code to a "PlayerModule" script called CameraController.
Same with movement system, needs to be server authoritive but pure-lua so it can be changed.
When you create a new world/scene, it auto appends the scripts in.

- [_] breakpoint history list per-script (show each iteration of breakpoint, can see change overtime)
- [_] expand breakpoint system to also include profilers like the heap allocation and timed flamegraph, you can see bottlenecks per iteration then (e.g. we can setup a "total compute", "total memory alloc", "total memory release", etc)
- [_] expose automation tools (AutomationService) like mouse clicks and keyboard inputs to luau scripts (so we can create ai that plays for you)

- [_] /docs/future-work/physics-expansion.md
- [_] /docs/future-work/world-streaming.md

### FUTURE

- [_] Navigation and AI: bounded navigation, perception, named inspectable AI
  states, behaviour plans, and driver-safe character actions in the
  [navigation and AI plan](docs/future-work/navigation-ai-system.md).
- [_] pathfinding
- [_] more advanced pathfinding where you can specify wall climbing and stuff, like a "can climb" zone or stuff lik that for ai too

- [_] gtlf default character (unreal style)
- [_] project demos:
* space engineers asteroids + planets full demo (`docs/FULL-PLANET-DEMO.md`)
* huge medieval battle full ai war, ai magic battle with tons of particles and explosions and whatnot
* ai village with daily tasks, occupations, relationships, and things like that (dwarf fortress style - personality, occupation, etc).
* floating islands with village houses on them with bridges connecting them together, floating above clouds, minecraft-like

- [_] localization support
- [_] /docs/future-work/terrain-system.md
- [_] Player-independent characters, controller state, animation graphs, and
  player roster/possession, plus calibrated body/hand tracking mapped to rig
  bones, in the [character system plan](docs/future-work/character-system.md).
- [_] /docs/future-work/vfx-system.md
- [_] Input actions, normalized devices, and a bounded `VirtualDeviceService`
  for custom controllers and external ingress. See the [input system
  plan](docs/future-work/input-system.md) and [virtual device service
  plan](docs/future-work/virtual-device-service.md).
- [_] /docs/future-work/camera-and-cinematics.md
- [_] /docs/future-work/prefab-package-system.md
- [_] /docs/future-work/procedural-generation.md
- [_] /docs/future-work/session-and-social.md
- [_] /docs/future-work/audio-system.md
- [_] go through docs/future-work/REVISIT_IDEAS.md for things we can do sooner.
- [_] maybe consider converting a bunch of custom tools to plugins and have them built-in to studio, or make a plugin pack as a extra release file you can import to a plugins/ folder in ~/Documents/atomic-game-engine/studio/plugins
- [_] (procedural, node-based) terrain generator (refer to discord references) - editablemesh, greedymesh, noise layers, node graph with previews, chunk-based, etc. Add voxel mode (which separates cardinal facing direction Fnt/Bk/Lft/Rgt/Top/Bott faces into groups - only renders the two groups it can see). Expand with surfacecameras, portals, etc, so it culls, occulusion culls, etc.
- [_] full procedural terrain studio tools
- [_] unity porting tools / unity shop
- [_] consider adding C# as another scripting langauge?
- [_] constraints system
- [_] deferred `D00106` - JavaScript and TypeScript breakpoints. The vendored QuickJS exposes no line hook and no debugger API at all, so this is a submodule decision rather than a feature. Asking for one on a .js/.ts chunk is refused with the reason, at the service, the gutter and the panel alike. **The TypeScript half of the entry shipped at v0.15 and is not part of this** - source maps are emitted and read, so the lines a debugger would land on are already the right ones.
- [_] full audio DAW (digital audio workbench) system
- [_] built-in whiteboxing tools (planning) for building (plugin)
- [_] full ui feature buildout + custom
- [_] ui creation tool, full aspect ratio scaling, select how it scales, how panels scale, etc. easier version of tooling than manually building them out
- [_] html-based ui creation (html-script?) => auto handles aspect constraints and whatnot as well, css as well. "virtual container" that makes/simulates the instances?
- [_] figma import tools
- [_] import blender files in asset explorer natively (drag .blend files on engine)
- [_] rpg maker port tool
- [_] photoshop file reader and import tool
- [_] docs/MOBILE.md implementation
- [_] concept idea: setup a public mcp repository in python, add .mcp.json in project folder that loads it, it watches forums channels in the discord server for new/existing bugs. agent writes a message in the channel stating you're fixing it, other agents work on other bugs. agents can write that "this bug is a big rewrite" in the channel too which could be helpful. as a custom plugin? maybe just consider as a separate project.
- [_] could we try some minecraft shaders / pbr texture packs as test items? maybe upload to my cdn and then load it and ill check if it works
- [_] add modulescript boundaries between luau and javascript VMs. moving values between vms. add a container component flag to enable it. add a [experiment] marker.
- [_] VR support, with Rift S as a requested validation target: OpenXR stereo
  views, in-game per-eye image-clarity calibration, and calibrated glasses-style
  magnification. Capability-based controllers, arbitrary tracked sources, body
  and hand calibration, and character bone/IK mapping share the input and
  character paths. Add Luau-defined virtual controllers and reusable tracking
  modules, with optional computer vision adapters feeding the same samples.
  Hardware support depends on provider integration; physical
  optical focus depends on headset support. Expand the
  Far Cry VR and Space Marine VR references from Zero Latency Adelaide when
  POVs arrive. Check OpenXR profiles for Vive wands, Rift S Oculus Touch, Valve
  Index, and the simple-controller fallback; targets remain unverified and
  runtime-dependent. Check Cosmos and Focus separately. See the [VR system
  plan](docs/future-work/vr-system.md), [input system
  plan](docs/future-work/input-system.md), and [virtual device service
  plan](docs/future-work/virtual-device-service.md).
- [_] setup a studio permissions system for: microphone, camera, etc
- [_] setup a example plugin for mocap with camera point track
- [_] ECS driven RL agent environments
- [_] Move "roblox files to atomic game files" to a external program - the port tool?

- [_] idea: for the LOD system, could we move mesh details into a normal map as part of the LOD? this way we can have a 'performance mode' that focuses on using this method instead of pure mesh data to show the details (even if it looks slightly uglier)
