#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Enums.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Interpolation.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/Runtime.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>

namespace engine::examples {

	namespace {
		using core::CFrame;
		using core::Vector3;
		using ecs::Components;
		using ecs::Entity;
		using ecs::Phase;
		using ecs::Scheduler;
		using ecs::Store;
		using scene::Bounds;
		using scene::PreviousTransform;
		using scene::Transform;
		using scene::WorldBounds;

		// --- systems ---------------------------------------------------------
		//
		// Plain functions capturing nothing, because there is nothing outside
		// the world for them to capture — which is what makes them replayable
		// from a recording and reusable by a second world.

		void MoveOrbits(Store &store) {
			// Simulated seconds, from the world's clock. Nothing accumulates
			// wall time here: the scene has to be in the same place after one
			// second whether that second took 30 frames or 600.
			const auto now = static_cast<float>(store.Time().Elapsed);

			store.Each<Transform, const Orbit>([now](Entity, Transform &transform, const Orbit &orbit) {
				const float angle = orbit.Phase + now * orbit.RadiansPerSecond;
				transform.Frame.Position = orbit.Centre + Vector3{
															  std::cos(angle) * orbit.Radius,
															  orbit.Height,
															  std::sin(angle) * orbit.Radius,
														  };
			});
		}

		void ApplySpin(Store &store) {
			// The tick delta, which is fixed. There is no way to reach the frame
			// delta from here by accident — it is a different field with a
			// different name.
			const float delta = store.Time().Delta;

			store.Each<Transform, const Spin>([delta](Entity, Transform &transform, const Spin &spin) {
				// Rotation composes on the right, so the spin is applied in the
				// cube's own space and the orbit position is untouched.
				transform.Frame =
					transform.Frame *
					CFrame::Angles(spin.Rate.X * delta, spin.Rate.Y * delta, spin.Rate.Z * delta);
			});
		}
	}

	namespace {
		// Where the staged Luau libraries are, resolved the same two ways
		// `ExamplePath` resolves a scene — and for the same layout mismatch,
		// which is written out in full there.
		std::filesystem::path LibraryRoot() {
			const std::filesystem::path preferred = core::Paths::Assets() / "lib";
			if (std::filesystem::exists(preferred)) {
				return preferred;
			}
			return core::Paths::Base().parent_path() / "assets" / "lib";
		}

		// Mirrors every staged library into `ReplicatedStorage`.
		//
		// **Idempotent by name.** A world that came out of a file already has
		// its tree, and mounting a second one beside it would give `require` two
		// copies of every module — which is worse than either, because a module
		// is cached per instance and two instances share no state.
		void MountSharedLibraries(Store &store) {
			const Entity replicated = store.FindFirstRoot("ReplicatedStorage");
			if (replicated == ecs::NULL_ENTITY) {
				return;
			}

			const std::filesystem::path root = LibraryRoot();

			std::error_code failure;
			if (!std::filesystem::is_directory(root, failure)) {
				// Not an error: a program staged without the libraries still
				// runs every scene that does not require one.
				return;
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

			for (const std::filesystem::path &directory : directories) {
				const std::string name = directory.filename().string();
				if (store.FindFirstChild(replicated, name) != ecs::NULL_ENTITY) {
					continue;
				}
				script::MountModuleTree(store, directory, name, replicated);
			}
		}
	}

	void RegisterExampleComponents() {
		// The shared set first, and under `scene`'s names. Every program
		// registers the same strings, which is what lets a snapshot resolve on
		// the far side with no translation layer.
		scene::RegisterSceneComponents();

		Components::Register<Orbit>("examples.Orbit");
		Components::Register<Spin>("examples.Spin");
	}

	void InstallMotionSystems(Scheduler &scheduler) {
		scheduler.Add("capture-previous", Phase::PreSimulation, scene::CapturePreviousTransforms);
		scheduler.Add("orbit", Phase::Simulation, MoveOrbits);
		scheduler.Add("spin", Phase::Simulation, ApplySpin);
	}

	bool LoadScene(Store &store, Scheduler &scheduler, const std::string &path, std::string &error) {
		// The class trees a script names, and this module's own components for
		// the C++ path. A script builds out of `Part`; nothing it touches is
		// registered here.
		scene::PartClass();

		// **Both trees, because there are two and a scene may use either.**
		// `Interface.luau` is built entirely out of `ScreenGui` and `Frame` and
		// touches no `Part` at all — so registering only the 3D tree made a
		// shipped example unloadable through the one entry point every program
		// shares, with "'ScreenGui' is not a registered class" as the whole
		// diagnosis. It loaded in the editor because `Editor::NewGame`
		// registers the gui classes on its own path, which is exactly the shape
		// of gap that survives: it works everywhere somebody looked.
		//
		// Idempotent, and the cost of a scene that uses neither is one
		// already-registered check.
		gui::RegisterGuiClasses();

		RegisterExampleComponents();

		// **The service instances, not just the classes above.** A scene that
		// parents a `ScreenGui` into `StarterGui` needs one to exist, and
		// registering the class only makes the name resolvable — `GetService`
		// looks for a *root* of that name and there was none, so a scene got
		// "'StarterGui' is not a service this engine provides" from a loader
		// that had just registered the class.
		//
		// Idempotent, which is what lets it sit beside the registrations rather
		// than behind a check: a world that came out of a file already has its
		// nine roots and this leaves them alone.
		scene::InstallServices(store);

		// **The shipped Luau libraries, mounted before anything runs.**
		//
		// A Rojo place maps `shared/MagicCore` to `ReplicatedStorage.MagicCore`
		// and a script just finds it there. This is that mapping: every
		// directory under the staged `lib/` becomes a tree of `ModuleScript`s in
		// the same place under the same name, so a file that came from such a
		// project requires its way around unchanged.
		//
		// Before `RunWorldScripts`, because a scene's first line is a `require`.
		// Idempotent by name — a world that already has the tree keeps it.
		MountSharedLibraries(store);

		// The extension picks the VM. `Rings.luau` and `Rings.js` build the
		// same world through the same bindings, and this loader never learns
		// which language it ran.
		//
		// **Kept alive rather than run and discarded**, which is the difference
		// between a scene format and a scripting layer. A script that connects
		// to `RunService.Heartbeat` *is* the simulation for what it built, so
		// the VM has to outlive the call that loaded it — the scheduler holds
		// the last reference and drops it with the world.
		std::shared_ptr<script::Runtime> runtime = script::MakeRuntime(store, script::LanguageOf(path));

		// **The scene's script is an instance in the scene**, which is what v0.6
		// made structural. `RunFile` still exists and still works; what this
		// buys is that the world now *contains* what animates it, so a save file
		// could write the pair out together — and the chunk gets a `script`
		// global naming itself, which is the difference between one file that
		// builds a world and a game made of many.
		// **Relative to the assets root when it is under it, absolute when it is
		// not.** `std::filesystem::relative` happily produces `../../../..` for a
		// file outside the root, and re-joining that to the root works by
		// accident rather than by design — until a `--script` naming somewhere
		// else entirely resolves to a path that does not exist. A `Source` that
		// is already absolute is left alone, which is what makes a script loaded
		// from anywhere behave the same as one staged.
		const std::filesystem::path absolute = std::filesystem::absolute(path);
		const std::filesystem::path relative = std::filesystem::relative(absolute, core::Paths::Assets());

		// The first component rather than a text prefix, and `native()` is why.
		// `path::native()` returns the platform's own string type — `std::wstring`
		// on Windows, `std::string` everywhere else — so `rfind("..", 0)` against
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

		if (runtime->RunWorldScripts() == 0) {
			error = runtime->LastError().empty() ? "the scene script did not run" : runtime->LastError();
			return false;
		}

		// **One beat with a zero delta before measuring, and that is what makes
		// the measurement mean anything.**
		//
		// A script sets a part's `Position` from inside its heartbeat, so
		// straight after loading every part is still at the origin and the
		// scene reads as one unit across — which is what the camera would then
		// frame. Pumping once places everything at its starting angle without
		// advancing the script's own clock, so what gets measured is the scene
		// as it will first be drawn.
		//
		// A zero delta rather than a tick's worth: this must not move the
		// simulation forward, only settle it.
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

		// Interpolation, and then the script. Not `InstallMotionSystems`: a
		// scripted scene moves itself, and installing the C++ orbit and spin
		// beside it would be two things driving one `Transform` — the second
		// one winning, silently, on whichever ran last.
		scheduler.Add("capture-previous", Phase::PreSimulation, scene::CapturePreviousTransforms);

		scheduler.Add("script-heartbeat", Phase::Simulation, [runtime](Store &world) {
			// **The fixed tick delta, never a frame time.** A script
			// integrating against wall time puts the scene somewhere else
			// on a busy machine, and the recording stops replaying — which
			// is the desync rule 5 names arriving through the call a script
			// uses most.
			if (!runtime->Heartbeat(world.Time().Delta)) {
				// Logged once per tick rather than swallowed. A scene that
				// silently stopped animating is a bug report with nothing
				// in it.
				ENGINE_ERROR("heartbeat: {}", runtime->LastError());
			}
		});

		ENGINE_INFO("scene '{}': {} entities, reaching {:.1f}", path, built, extent);
		return true;
	}

	std::string ExamplePath(const std::string &name) {
		// Under the assets root, so `--assets` moves the examples with
		// everything else and a program started from any directory finds the
		// same file.
		const std::filesystem::path preferred = core::Paths::Assets() / "examples" / name;
		if (std::filesystem::exists(preferred)) {
			return preferred.string();
		}

		// **The staged sibling, and this fallback is a layout mismatch rather
		// than a convenience.** Shaders stage into each *program's* directory —
		// `client/shaders/render/` — and `Paths::Assets()` defaults to that same
		// directory, so those line up. The example scenes stage into
		// `<stage>/assets/examples/` instead, which is a sibling of it, so they
		// do not.
		//
		// Making them agree means either staging the scenes per program the way
		// shaders are, or changing what `Paths::Assets()` defaults to — and both
		// are changes to how every program finds its data, which is more than
		// this function gets to decide. So it looks in both and says why.
		const std::filesystem::path sibling =
			core::Paths::Base().parent_path() / "assets" / "examples" / name;
		if (std::filesystem::exists(sibling)) {
			return sibling.string();
		}

		// Neither exists. The preferred one is returned so the error names the
		// path somebody meant rather than the fallback they have never heard of.
		return preferred.string();
	}
}
