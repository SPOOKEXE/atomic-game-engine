#pragma once

// The fixtures every world has: a workspace and the services.
//
// **A service is an instance, for the same reason a camera is one.** Roblox's
// `game:GetService("Lighting")` hands back a thing in the tree — it has a
// parent, children, properties, and it is in the save file. The engine already
// had services as *script globals* (`RunService`, `MessagingService`), which is
// a different animal: those are engine surfaces a script calls, they hold no
// content, and an author cannot put anything in one. These are containers.
//
// **Per world, not per universe, and that is Roblox's arrangement rather than
// a shortcut.** An entity is a row in one `ecs::Store` and a store is a world,
// so a universe-level service would need a store of its own with nothing in it
// but nine rows. It would also be wrong: each place in a Roblox game has its
// own ServerScriptService and its own Lighting, and two places sharing one set
// is not a thing an author can express today or would want to.
//
// **What makes them different from an ordinary instance is a component, not a
// class check.** `ServiceComponent` is on every one of them, so "is this a
// fixture" and "show me this world's services" are queries. The alternative —
// comparing class names at each call site — is nine string comparisons that
// drift the first time somebody adds a tenth service.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/Enums.hpp>

#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// Marks an instance as made by whoever is looking, not by an author.
	//
	// **A camera is the reason this exists, and Team Create is the reason it is
	// a component rather than a special case in the writer.** The camera in
	// `Workspace` is the *viewer's*: the editor makes one to show its own point
	// of view, a client makes one for the player, and when several people edit
	// one game they each make their own. None of them is content — a game file
	// that carried somebody's camera would hand their viewpoint to everyone who
	// opened it, and a second person joining would add a second one to the file
	// forever.
	//
	// So this is skipped by `game::WriteWorldBody` and everything under it. The
	// instance is real in every other way: it is in the tree, scripts see it,
	// the properties panel edits it. It simply does not survive being written
	// out, because it was never the file's to keep.
	//
	// @since v0.7
	struct TransientComponent {
		// Explicit padding, so the object representation a snapshot writes holds
		// no uninitialised bytes. A snapshot *does* carry these — Stop has to
		// put the editor's camera back exactly as it was, and that is a
		// different question from what a game file holds.
		uint32_t Reserved = 0;
	};

	// What every service has, and nothing else does.
	//
	// A component rather than a class check: see the header comment.
	//
	// @since v0.7
	struct ServiceComponent {
		// Who may see this service's children.
		ServiceScope Scope = ServiceScope::Shared;

		// Whether an author may delete or reparent it.
		//
		// **A world with no `Workspace` is not a world an author meant to
		// build.** Roblox refuses the same operation and for the same reason:
		// the fixtures are what scripts resolve by name, so deleting one turns
		// every `game:GetService` in the place into a runtime error at a
		// distance from the delete that caused it.
		bool Fixture = true;

		// Explicit padding, so the object representation a snapshot writes
		// holds no uninitialised bytes. `ecs::WorldTime` learned this the
		// expensive way.
		uint8_t Reserved[2] = {0, 0};
	};

	// What `Lighting` holds that no other service does.
	//
	// **A component of its own rather than fields on `ServiceComponent`.** Nine
	// services carry that one; exactly one of them has a fog colour, and a
	// shared component with eight unused floats on every service is eight
	// floats in every snapshot of every world.
	//
	// Nothing reads these yet — the renderer's lighting is not driven from a
	// world's `Lighting` service. They are authored and they round-trip, which
	// is the half that has to exist before the renderer can read them.
	//
	// @since v0.7
	struct LightingServiceComponent {
		// The light bouncing around in shadow.
		core::Color3 Ambient{0.078f, 0.078f, 0.078f};

		// The ambient term outdoors, which a sky replaces indoors.
		core::Color3 OutdoorAmbient{0.502f, 0.502f, 0.502f};

		// What distance fades to.
		core::Color3 FogColor{0.753f, 0.753f, 0.753f};

		// How strong the sun is.
		float Brightness = 2.0f;

		// The time of day, in hours. 14 is Roblox's default afternoon.
		float ClockTime = 14.0f;

		// Where fog starts, in studs.
		float FogStart = 0.0f;

		// Where fog is total, in studs. The default is far enough out to read as
		// no fog at all.
		float FogEnd = 100000.0f;

		// Which latitude the sun's arc is computed for, in degrees.
		float GeographicLatitude = 41.733f;
	};

	// Registers `Service` and the nine classes under it.
	//
	// Idempotent and process-wide, like every other registration here. Calls
	// `PartClass` first: a service derives from `Instance`, which that
	// registers, and a second root would be a tree scripts cannot walk.
	//
	// @return The abstract `Service` base, which nothing can instantiate.
	ecs::ClassId ServiceClass();

	// Creates whatever fixtures a world is missing.
	//
	// **Idempotent, and that is what makes it safe to call on a world that came
	// out of a file.** A game saved before this existed has no services in it;
	// a game saved after has all of them. Calling this on either one leaves the
	// same nine roots, and calling it twice does nothing the second time —
	// which is what lets the studio run it after every load without checking
	// which kind of file it got.
	//
	// The arrangement is Roblox's: `StarterPlayerScripts` under `StarterPlayer`
	// rather than beside it.
	//
	// **No camera.** A camera belongs to whoever is looking rather than to the
	// game — see `TransientComponent` — so the viewer makes its own.
	//
	// @param store The world to furnish.
	// @return The `Workspace`, which is the one callers usually want next.
	ecs::Entity InstallServices(ecs::Store &store);

	// The world's `Workspace`, or a null entity when it has none.
	//
	// @param store The world.
	// @return The workspace instance.
	ecs::Entity WorkspaceOf(const ecs::Store &store);
}
