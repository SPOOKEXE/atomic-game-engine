#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Enums.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>

namespace engine::examples {

	namespace {
		using core::Vector3;
		using ecs::Entity;
		using ecs::Phase;
		using ecs::Scheduler;
		using ecs::Store;
		using scene::Bounds;
		using scene::PreviousTransform;
		using scene::Transform;
		using scene::WorldBounds;

	}

	ecs::Entity MountDemoCamera(Store &store, std::string_view builderPath) {
		const std::filesystem::path builder(builderPath);
		const std::string extension = script::LanguageOf(builder.string()) == script::Language::JavaScript
										  ? ".client.js"
										  : ".client.luau";
		const std::string filename = builder.stem().string() + extension;
		auto path = builder.parent_path() / "client" / "DemoCameras" / filename;
		std::error_code failure;
		if (!std::filesystem::is_regular_file(path, failure)) {
			path = DemosLoader().Directory(DemoKind::Script) / "client" / "DemoCameras" / filename;
			if (!std::filesystem::is_regular_file(path, failure)) return ecs::NULL_ENTITY;
		}
		const auto starter = store.FindFirstRoot("StarterPlayer");
		const auto scripts = store.FindFirstChild(starter, "StarterPlayerScripts");
		if (scripts == ecs::NULL_ENTITY) return ecs::NULL_ENTITY;
		const auto absolute = std::filesystem::absolute(path);
		const auto relative = std::filesystem::relative(absolute, core::Paths::Assets(), failure);
		const bool underAssets = !failure && !relative.empty() && *relative.begin() != "..";
		const auto demoRelative =
			std::filesystem::relative(absolute, DemosLoader().Directory(DemoKind::Script), failure);
		const bool underDemos = !failure && !demoRelative.empty() && *demoRelative.begin() != "..";
		const std::string source = underDemos ? "examples/scripts/" + demoRelative.generic_string()
											  : (underAssets ? relative.generic_string() : absolute.string());
		std::string text;
		std::string error;
		if (!script::ReadSource(store, core::Name(absolute.string()), text, error)) return ecs::NULL_ENTITY;
		script::SourceCache cache;
		if (const auto *existing = store.Resource<script::SourceCache>()) cache = *existing;
		cache.Set(core::Name(source), std::move(text));
		store.SetResource(cache);
		if (const auto old = store.FindFirstChild(scripts, "DemoCamera"); old != ecs::NULL_ENTITY)
			store.DestroyInstance(old);
		const auto camera = script::MakeScript(store, source, "DemoCamera", true);
		if (camera != ecs::NULL_ENTITY) store.SetParent(camera, scripts);
		return camera;
	}

	namespace {
		bool StartDemoCamera(Store &store, script::Runtime &runtime, ecs::Entity source, std::string &error) {
			if (source == ecs::NULL_ENTITY || !runtime.Role().Client) return true;
			const auto *local = store.Resource<scene::LocalPlayer>();
			const auto player = local != nullptr && store.Alive(local->Instance)
									? local->Instance
									: scene::AddPlayer(store, "Player", true, 1);
			const auto scripts = store.FindFirstChild(player, "PlayerScripts");
			if (scripts == ecs::NULL_ENTITY) {
				error = "the local player has no PlayerScripts for the demo camera";
				return false;
			}
			// The player can precede the scene builder. Clone the completed config,
			// rather than running the template or retaining an earlier empty copy.
			if (const auto old = store.FindFirstChild(scripts, "DemoCamera"); old != ecs::NULL_ENTITY) {
				store.EachChild(old, [&](ecs::Entity marker) {
					const auto name = store.InstanceNameOf(marker);
					if ((name != core::Name("PublishedCamera") && name != core::Name("PublishedRoot")) ||
						store.ClassOf(marker) != ecs::Classes::Find(core::Name("ObjectValue")))
						return;
					ecs::Entity published{};
					if (!store.GetProperty(marker, core::Name("Value"), &published, sizeof(published)) ||
						!store.Alive(published) || !Store::IsPredicted(published) ||
						!ecs::IsClientLocalInstance(store, published))
						return;
					if (name == core::Name("PublishedCamera") &&
						store.ClassOf(published) != scene::CameraClass())
						return;
					store.DestroyInstance(published);
				});
				store.DestroyInstance(old);
			}
			const auto camera = store.ClonePredictedInstance(source);
			if (camera == ecs::NULL_ENTITY) {
				error = "the world refused the local demo camera script";
				return false;
			}
			store.Set(camera, ecs::ClientLocal{});
			store.EachDescendant(camera, [&](ecs::Entity child) { store.Set(child, ecs::ClientLocal{}); });
			store.SetParent(camera, scripts);
			if (!store.HasResource<scene::CameraController>()) store.SetResource(scene::CameraController{});
			if (runtime.RunNewScripts(std::array{camera}) != 1) {
				error = runtime.LastError();
				return false;
			}
			return true;
		}

		// Where a scene's own Luau modules are staged.
		//
		// `Magic.luau` and `Magic` both answer the same directory, so a
		// caller holding either spelling needs no rule of its own.
		std::filesystem::path SceneLibraryRoot(std::string_view scene) {
			std::string stem(scene);
			if (const size_t dot = stem.rfind('.'); dot != std::string::npos && dot > 0) {
				stem.erase(dot);
			}

			return DemosLoader().Directory(DemoKind::Script) / stem;
		}
	}

	size_t MountSceneLibraries(Store &store, ecs::Entity script, std::string_view scene) {
		if (script == ecs::NULL_ENTITY || !store.Alive(script)) {
			return 0;
		}

		const std::filesystem::path root = SceneLibraryRoot(scene);

		std::error_code failure;
		if (!std::filesystem::is_directory(root, failure)) {
			// Not an error, and it is the ordinary case: three scenes have a
			// directory and every other one does not.
			return 0;
		}

		std::vector<std::filesystem::path> directories;
		for (const auto &entry : std::filesystem::directory_iterator(root, failure)) {
			if (entry.is_directory()) {
				directories.push_back(entry.path());
			}
		}

		// Sorted, so two runs mint the same instance ids. `MountModuleTree`
		// sorts within a directory for the same reason.
		std::sort(directories.begin(), directories.end());

		size_t mounted = 0;
		for (const std::filesystem::path &directory : directories) {
			const std::string name = directory.filename().string();
			if (store.FindFirstChild(script, name) != ecs::NULL_ENTITY) {
				continue;
			}
			if (script::MountModuleTree(store, directory, name, script) != ecs::NULL_ENTITY) {
				mounted++;
			}
		}

		return mounted;
	}

	bool LoadScene(
		Store &store,
		Scheduler &scheduler,
		const std::string &path,
		std::string &error,
		std::shared_ptr<script::Runtime> *out,
		const script::RuntimeLimits *limits
	) {
		// Register the class trees and component types needed by a scripted scene
		// before loading it.
		//
		// **`RegisterSceneClasses`, not `EnsureClassTree` alone - found the
		// same way the `gui` gap below was.** `ShaderScript` and
		// `EditableMesh` register through their own accessors, which
		// `RegisterSceneClasses` calls and `EnsureClassTree` does not; a
		// scene loaded through this one entry point got "'EditableMesh' is
		// not a registered class" from `Instance.new`, while the same class
		// worked from `Game.cpp`'s path and from the studio's - exactly the
		// "works everywhere somebody looked" shape the comment two lines
		// down already names.
		scene::RegisterSceneClasses();

		// **Both trees, because there are two and a scene may use either.**
		// `Interface.luau` is built entirely out of `ScreenGui` and `Frame` and
		// touches no `Part` at all - so registering only the 3D tree made a
		// shipped example unloadable through the one entry point every program
		// shares, with "'ScreenGui' is not a registered class" as the whole
		// diagnosis. It loaded in the editor because `Editor::NewGame`
		// registers the gui classes on its own path, which is exactly the shape
		// of gap that survives: it works everywhere somebody looked.
		//
		// Idempotent, and the cost of a scene that uses neither is one
		// already-registered check.
		gui::RegisterGuiClasses();

		// **The third tree, and it went missing the same way the two above did.**
		// `ParticleEmitter`, `Beam` and `Trail` live in `effects` and were
		// registered by `client::BuildScriptedWorld` alone - so a scene that made
		// one loaded in the client and refused everywhere else that shares this
		// entry point, with "'ParticleEmitter' is not a registered class" thrown
		// out of a heartbeat rather than out of the load. `Magic.luau` is what
		// found it; `Particles.luau` and `StressParticles.luau` had the same hole
		// under them and no test to fall in it.
		//
		// Registered here rather than left to each program for the reason the
		// paragraph above gives: one entry point that every program shares is the
		// only place a class tree can be registered once and be true for all of
		// them.
		effects::RegisterEffectClasses();

		scene::RegisterSceneComponents();

		// **The service instances, not just the classes above.** A scene that
		// parents a `ScreenGui` into `StarterGui` needs one to exist, and
		// registering the class only makes the name resolvable - `GetService`
		// looks for a *root* of that name and there was none, so a scene got
		// "'StarterGui' is not a service this engine provides" from a loader
		// that had just registered the class.
		//
		// Idempotent, which is what lets it sit beside the registrations rather
		// than behind a check: a world that came out of a file already has its
		// nine roots and this leaves them alone.
		scene::InstallServices(store);

		// **And `gui`'s, which is a second call because it has to be.** `scene`
		// may not link `gui` - `gui/AGENTS.md` refuses the edge in both
		// directions - so `GuiService` cannot come from the line above it and a
		// host calls both. This loader is that host for every `--script` world.
		//
		// **Nothing in the shipped tree called this, and the symptom was the
		// keyboard.** `gui::Focus` refuses a world with no `GuiService` because
		// there is nowhere for the fact to rest, so clicking a `TextBox` in a
		// real client took no focus, `GetFocusedTextBox` answered nil forever and
		// every character typed went nowhere. The router was right and the
		// service it writes to had never been installed - the same shape as the
		// router that was never `Update`d one version earlier.
		//
		// Idempotent, for the reason the call above it is.
		gui::InstallGuiServices(store);

		// **The shipped Luau libraries, mounted before anything runs.**
		//
		// A Rojo place maps `shared/MagicCore` to `ReplicatedStorage.MagicCore`
		// and a script just finds it there. This is that mapping: every
		// directory under the staged `lib/` becomes a tree of `ModuleScript`s in
		// the same place under the same name, so a file that came from such a
		// project requires its way around unchanged.
		//
		// Before `RunWorldScripts`, because a scene's first line is a `require`.
		// Idempotent by name - a world that already has the tree keeps it.

		// The extension picks the VM, so callers may load their own scripts in
		// any supported language. Shipped examples are authored in Luau.
		//
		// **Kept alive rather than run and discarded**, which is the difference
		// between a scene format and a scripting layer. A script that connects
		// to `RunService.Heartbeat` *is* the simulation for what it built, so
		// the VM has to outlive the call that loaded it - the scheduler holds
		// the last reference and drops it with the world.
		std::shared_ptr<script::Runtime> runtime =
			limits != nullptr ? script::MakeRuntime(store, script::LanguageOf(path), *limits)
							  : script::MakeRuntime(store, script::LanguageOf(path));

		// **The scene's script is an instance in the scene**, which is what v0.6
		// made structural. `RunFile` still exists and still works; what this
		// buys is that the world now *contains* what animates it, so a save file
		// could write the pair out together - and the chunk gets a `script`
		// global naming itself, which is the difference between one file that
		// builds a world and a game made of many.
		// **Relative to the assets root when it is under it, absolute when it is
		// not.** `std::filesystem::relative` happily produces `../../../..` for a
		// file outside the root, and re-joining that to the root works by
		// accident rather than by design - until a `--script` naming somewhere
		// else entirely resolves to a path that does not exist. A `Source` that
		// is already absolute is left alone, which is what makes a script loaded
		// from anywhere behave the same as one staged.
		const std::filesystem::path absolute = std::filesystem::absolute(path);
		const std::filesystem::path relative = std::filesystem::relative(absolute, core::Paths::Assets());

		// The first component rather than a text prefix, and `native()` is why.
		// `path::native()` returns the platform's own string type - `std::wstring`
		// on Windows, `std::string` everywhere else - so `rfind("..", 0)` against
		// it does not compile with MSVC at all. `string()` would transcode and
		// build, but a prefix test is the wrong question regardless: it calls a
		// directory honestly named `..config` an escape from the assets root.
		// Comparing components asks what the paragraph above says we are asking.
		const bool underAssets = !relative.empty() && *relative.begin() != "..";

		const ecs::Entity program = script::MakeScript(
			store, underAssets ? relative.string() : absolute.string(), absolute.stem().string()
		);
		if (program == ecs::NULL_ENTITY) {
			error = "the world refused a script instance";
			return false;
		}

		// **The scene's own modules, under the scene's own script, before it
		// runs.** A scene's first line is a `require` and what it requires is
		// `script.MagicCore` - see `MountSceneLibraries` for why they hang off
		// the script rather than off `ReplicatedStorage`.
		(void)MountSceneLibraries(store, program, absolute.filename().string());
		const auto demoCamera = MountDemoCamera(store, absolute.string());

		if (runtime->RunWorldScripts() == 0) {
			error = runtime->LastError().empty() ? "the scene script did not run" : runtime->LastError();
			return false;
		}
		if (!StartDemoCamera(store, *runtime, demoCamera, error)) return false;

		// **One beat with a zero delta before measuring, and that is what makes
		// the measurement mean anything.**
		//
		// A script sets a part's `Position` from inside its heartbeat, so
		// straight after loading every part is still at the origin and the
		// scene reads as one unit across - which is what the camera would then
		// frame. Pumping once places everything at its starting angle without
		// advancing the script's own clock, so what gets measured is the scene
		// as it will first be drawn.
		//
		// A zero delta rather than a tick's worth: this must not move the
		// simulation forward, only settle it.
		// Physics remains optional for bare loads; when a host prepared it,
		// refresh authored colliders before startup heartbeat queries run.
		if (physics::PhysicsClockOf(store) != nullptr) physics::SyncBroadphase(store);
		if (!runtime->Heartbeat(0.0f)) {
			error = runtime->LastError();
			return false;
		}

		// Measured from what the script actually built rather than declared by
		// it. A scene that set its own bounds would be two sources of truth for
		// one fact, and the camera frames from this.
		float extent = 1.0f;
		size_t built = 0;
		store.Each<const Transform, const Bounds>(
			[&](Entity, const Transform &transform, const Bounds &bounds) {
				const Vector3 position = transform.Frame.Position;
				const float reach = std::max(std::abs(position.X), std::abs(position.Z)) +
									std::max(bounds.HalfExtent.X, bounds.HalfExtent.Z);
				extent = std::max(extent, reach);
				built++;
			}
		);

		store.SetResource(WorldBounds{extent});

		// Interpolation and portal transit support run beside the scene's
		// scripted heartbeat. The script alone authors motion.
		scheduler.Add("capture-previous", Phase::PreSimulation, scene::CapturePreviousTransforms);

		// **And the snap that cancels it across a hole** - see
		// `scene::SnapPortalTransit`. `PreRender`, because the serial it reads
		// arrives with a replication delta and a delta lands after the tick's
		// `PreSimulation` has run.
		scheduler.Add("snap-portal-transit", Phase::PreRender, [](Store &world) {
			(void)scene::SnapPortalTransit(world);
		});

		scheduler.Add("script-heartbeat", Phase::Simulation, [runtime](Store &world) {
			if (runtime->Role().Client) runtime->RunNewScripts(script::ClientScriptsIn(world));
			// **The fixed tick delta, never a frame time.** A script
			// integrating against wall time puts the scene somewhere else
			// on a busy machine, and the recording stops replaying - which
			// is the desync rule 5 names arriving through the call a script
			// uses most.
			if (!runtime->Heartbeat(world.Time().Delta)) {
				// Logged once per tick rather than swallowed. A scene that
				// silently stopped animating is a bug report with nothing
				// in it.
				ENGINE_ERROR("heartbeat: {}", runtime->LastError());
			}
		});

		// **Handed back only on success**, so a caller cannot be given a VM for
		// a world this function has just emptied.
		if (out != nullptr) {
			*out = runtime;
		}

		ENGINE_INFO("scene '{}': {} entities, reaching {:.1f}", path, built, extent);
		return true;
	}

	std::string ExamplePath(const std::string &name) {
		const DemosLoader demos;
		const std::filesystem::path resolved = demos.Resolve(DemoKind::Script, name);
		return (resolved.empty() ? demos.Directory(DemoKind::Script) / name : resolved).string();
	}

	std::vector<std::string> ExampleScenes() {
		std::vector<std::string> found;
		for (const DemoEntry &entry : DemosLoader().List(DemoKind::Script)) {
			found.push_back(entry.Name);
		}
		return found;
	}
}
