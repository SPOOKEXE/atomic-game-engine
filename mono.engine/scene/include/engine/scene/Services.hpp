#pragma once

// The fixtures every world has: a workspace and the services.
//
// **A service is an instance, for the same reason a camera is one.** Roblox's
// `game:GetService("Lighting")` hands back a thing in the tree - it has a
// parent, children, properties, and it is in the save file. The engine already
// had services as *script globals* (`RunService`, `MessagingService`), which is
// a different animal: those are engine surfaces a script calls, they hold no
// content, and an author cannot put anything in one. These are containers.
//
// **Per world, not per universe, and that is Roblox's arrangement rather than
// a shortcut.** An entity is a row in one `ecs::Store` and a store is a world,
// so a universe-level service would need a store of its own holding nothing but
// the service rows. It would also be wrong: each place in a Roblox game has its
// own ServerScriptService and its own Lighting, and two places sharing one set
// is not a thing an author can express today or would want to.
//
// **What makes them different from an ordinary instance is a component, not a
// class check.** `ServiceComponent` is on every one of them, so "is this a
// fixture" and "show me this world's services" are queries. The alternative -
// comparing class names at each call site - is one string comparison per
// service, and they drift the first time somebody adds another one.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/scene/Enums.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

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
	// one game they each make their own. None of them is content - a game file
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
		// no uninitialised bytes. A snapshot *does* carry these - Stop has to
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
	// **A component of its own rather than fields on `ServiceComponent`.** Every
	// service carries that one; exactly one of them has a fog colour, and a
	// shared component with eight unused floats on every service is eight
	// floats in every snapshot of every world.
	//
	// `LightingOf` resolves these into the directional, ambient and fog values
	// consumed by every view of this world.
	//
	// @since v0.7
	struct LightingServiceComponent {
		// The light bouncing around in shadow.
		core::Color3 Ambient{0.078f, 0.078f, 0.078f};

		// The ambient term outdoors, which a sky replaces indoors.
		core::Color3 OutdoorAmbient{0.058f, 0.058f, 0.058f};

		// What distance fades to.
		core::Color3 FogColor{0.0f, 0.0f, 0.0f};

		// How strong the sun is.
		float Brightness = 0.440f;

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

	// What `Players` holds that no other service does.
	//
	// **A component of its own rather than fields on `ServiceComponent`**, for
	// `LightingServiceComponent`'s reason: thirteen services carry that one and
	// exactly one of them has a respawn delay.
	//
	// @since v0.17
	struct PlayersServiceComponent {
		// What the next player admitted is numbered, when nobody says.
		//
		// **A counter on the world rather than a clock or a random draw**, which
		// is what makes two runs of one recording hand out the same identities.
		// It is not a declared property: a game authors `MaxPlayers`, and the
		// next free number is bookkeeping the engine owns.
		int64_t NextUserId = 1;

		// How many players this world admits.
		//
		// **Enforced by `AddPlayer`, which is the one door a player arrives
		// through.** The transport has its own cap -
		// `replication::ListenerSettings::MaximumClients` - and the two are
		// different questions: that one is how many sockets a process will hold,
		// this one is how many occupants the *game* is for.
		int32_t MaxPlayers = 50;

		// How long after losing a character a player waits for the next one,
		// in seconds. What a new `Player` starts its own `RespawnTime` from.
		float RespawnTime = 5.0f;

		// Whether losing a character earns a new one without a script asking.
		//
		// Roblox's `Players.CharacterAutoLoads`. False is a game that spawns
		// its occupants itself, through `Player:LoadCharacter()`.
		bool CharacterAutoLoads = true;

		// Explicit padding, so the object representation a snapshot writes holds
		// no uninitialised bytes. The reason every other `Reserved` here gives.
		uint8_t Reserved[7] = {};
	};

	// Who one occupant is, held on the `Player`.
	//
	// **Separate from `PlayerCharacter` because the two have different
	// lifetimes.** Who somebody is lasts as long as they are connected; what
	// body they are driving is gone and remade on every respawn, and a component
	// holding both would be written on every spawn for the sake of two fields
	// that never change.
	//
	// @since v0.17
	struct PlayerIdentity {
		// The stable number a game keys saved data by.
		//
		// **Widest first**, so the object representation a snapshot writes holds
		// no padding between this and the two fields below it.
		int64_t UserId = 0;

		// What to show instead of `Name`.
		//
		// **A `core::Name` like `Instance.Name` beside it**, and for that
		// property's reason rather than in spite of `PropertyType::String`: a
		// display name is assigned once per occupant and never computed per
		// frame, so it is exactly the bounded set interning is for. A score or a
		// chat line would be a `String`; this is not one.
		core::Name DisplayName;

		// How long *this* player waits, which is `PlayersServiceComponent`'s
		// value at the moment they joined unless a game has changed it.
		float RespawnTime = 5.0f;
	};

	// When this player may have a body again.
	//
	// **A tick number rather than a countdown**, which is `DebrisQueue`'s rule
	// and is what keeps a respawn on the same beat on a fast machine and a slow
	// one: `ceil(seconds / delta)` is computed once, from the fixed tick delta,
	// and nothing afterwards reads a clock.
	//
	// Present only between losing a character and gaining the next one, so the
	// ordinary state of a living player is not to have this row at all.
	//
	// @since v0.17
	struct PlayerRespawn {
		// The tick `UpdateRespawns` spawns on.
		uint64_t DueTick = 0;
	};

	// Registers `Service` and the thirteen classes under it.
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
	// same eleven roots, and calling it twice does nothing the second time -
	// which is what lets the studio run it after every load without checking
	// which kind of file it got.
	//
	// The arrangement is Roblox's: `StarterPlayerScripts` under `StarterPlayer`
	// rather than beside it.
	//
	// **No camera.** A camera belongs to whoever is looking rather than to the
	// game - see `TransientComponent` - so the viewer makes its own.
	//
	// @param store The world to furnish.
	// @return The `Workspace`, which is the one callers usually want next.
	ecs::Entity InstallServices(ecs::Store &store);

	// The world's service of one class, or a null entity when it has none.
	//
	// **By class and never by name, and that distinction was a live bug.**
	// `WorkspaceOf` was `FindFirstRoot("Workspace")`, so a script renaming the
	// workspace made it answer nothing - and `InstallServices`, which used the
	// same lookup to decide what a world was missing, then minted a *second*
	// `Workspace` beside the one holding the scene. A class is registered once
	// and a rename cannot touch it, so this is the shape every fixture lookup in
	// this module takes.
	//
	// **Roots only**, which is what a service is: `InstallServices` parents
	// eleven of them to nothing, and `StarterPlayerScripts` and
	// `StarterCharacterScripts` to `StarterPlayer`. Those two are found through
	// `ServiceUnder`.
	//
	// @param store The world.
	// @param klass The service's class.
	// @return The instance, or `ecs::NULL_ENTITY`.
	// @since v0.17
	ecs::Entity ServiceOf(const ecs::Store &store, ecs::ClassId klass);

	// The same, for a service that lives under another one.
	//
	// @param store  The world.
	// @param parent The service it sits under.
	// @param klass  The service's class.
	// @return The instance, or `ecs::NULL_ENTITY`.
	// @since v0.17
	ecs::Entity ServiceUnder(const ecs::Store &store, ecs::Entity parent, ecs::ClassId klass);

	// The world's `Workspace`, or a null entity when it has none.
	//
	// @param store The world.
	// @return The workspace instance.
	ecs::Entity WorkspaceOf(const ecs::Store &store);

	// The world's `Players` service, or a null entity when it has none.
	//
	// @param store The world.
	// @return The service instance.
	ecs::Entity PlayersOf(const ecs::Store &store);

	// Whether a client should be sent this instance ahead of the rest of the
	// world.
	//
	// **What `ReplicatedFirst` is for, and the container had no reader.** Roblox
	// replicates that service and runs its scripts before the rest of the tree
	// arrives, which is the whole point of it existing beside
	// `ReplicatedStorage`; here it was an ordinary root with an ordinary
	// priority, so a loading screen in it arrived somewhere in the middle of the
	// world it was meant to cover.
	//
	// **A predicate rather than a filter, for `VisibleToClients`' reason.**
	// `mono.engine/replication` does not depend on `scene` and must not, so the
	// rule lives here beside the service it reads and the plumbing lives in
	// whoever owns a connection - `Authority::SetPriority` is the hook.
	//
	// @param store    The world.
	// @param instance Anything in the tree.
	// @return `true` for anything under `ReplicatedFirst`.
	// @since v0.17
	bool InReplicatedFirst(const ecs::Store &store, ecs::Entity instance);

	// Which player this host is, when it is a client.
	//
	// **A resource rather than a component, because there is one of it.** A
	// world has many `Player` instances and at most one of them is *this*
	// viewer - `ecs/AGENTS.md`'s rule is that one-of-a-kind state is a resource,
	// and a `LocalPlayer` tag on a row would be a second place the answer lived.
	//
	// **Empty on a server, and that is the whole point.** `Players.LocalPlayer`
	// is nil where `IsClient()` is false, so a `Script` that reached for it gets
	// nil rather than somebody else's player - which is the bug this separation
	// exists to make impossible.
	//
	// @since v0.10
	struct LocalPlayer {
		// The player instance this host is looking through, or a null entity.
		ecs::Entity Instance;
	};

	// Admits one occupant: the `Player`, its four containers, and the two
	// `Starter*` copies that are made once and never again.
	//
	// **Not automatic.** Who is in a game is the host's business - a dedicated
	// server admits players as they connect and the studio admits one per client
	// view - so furnishing a world does not invent an occupant.
	//
	// ## What a join does, in order
	//
	//   1. The `Player` row, under `Players`, named `name`.
	//   2. `PlayerGui`, `PlayerScripts`, `Backpack` and `StarterGear` under it.
	//      All four are private to that player on the wire - see `PlayerOwning`.
	//   3. Every child of `StarterPlayer.StarterPlayerScripts` cloned into
	//      `PlayerScripts`. **Once, on join, and never on a respawn**, which is
	//      Roblox's rule and the whole reason that container is not
	//      `StarterCharacterScripts`.
	//   4. Every child of `StarterPack` cloned into `StarterGear`.
	//      `StarterGear` is the *persistent* gear list: what a game adds to it
	//      later survives every respawn, because it is `Backpack` that is
	//      refilled from this rather than the other way round.
	//
	// The interface is deliberately not step five: `StarterGui` is copied into
	// `PlayerGui` on **every** spawn rather than on the join, and it is
	// `gui::ResetPlayerGui` that does it because `scene` may not link `gui`.
	//
	// **Refused past `Players.MaxPlayers`**, which is what gives that property a
	// reader. A host that gets a null back has a world that is full.
	//
	// @param store  The world.
	// @param name   What to call them.
	// @param local  Whether this host is looking through them, which is what
	//        makes `Players.LocalPlayer` answer. At most one player per world
	//        should be marked local; the last one marked wins.
	// @param userId The stable number a game keys saved data by. Zero takes the
	//        next one the world has not handed out, which is what a host with no
	//        account system behind it wants.
	// @return The player instance, or `NULL_ENTITY` when there is no `Players`
	//         or the world is full.
	ecs::Entity AddPlayer(ecs::Store &store, std::string_view name, bool local = false, int64_t userId = 0);

	// Copies every child of one container into another.
	//
	// **The whole of what a `Starter*` service does, in one function, because
	// there are four of them and they differ only in which pair they name.**
	// `StarterPlayerScripts` into `PlayerScripts`, `StarterPack` into
	// `StarterGear`, `StarterCharacterScripts` into the character, `StarterGear`
	// into `Backpack` - four calls rather than four copies of a loop, which is
	// the paste `docs/CODE_QUALITY.md` asks about. The fifth,
	// `StarterGui` into `PlayerGui`, is `gui::ResetPlayerGui`'s because it
	// carries a survival rule the other four do not have.
	//
	// **Cloned rather than moved**, so the template is still there for the next
	// player and the next life.
	//
	// **Collected before anything is cloned.** Each copy is parented into the
	// destination while the source's child list is being walked; a walk that
	// mutates the store the walk is reading is the shape that bites eventually
	// rather than immediately, and `gui::ResetPlayerGui` takes the same
	// precaution one door along.
	//
	// @param store       The world.
	// @param source      What to copy from. A null entity copies nothing, which
	//        is what a world with no such service is.
	// @param destination What to copy into. A null entity copies nothing.
	// @return How many children were cloned.
	// @since v0.17
	size_t CloneChildrenInto(ecs::Store &store, ecs::Entity source, ecs::Entity destination);

	// Destroys every child of a container.
	//
	// **`Backpack`'s half of a respawn, and it is a function because the
	// gathering matters.** `DestroyInstance` walks the instance's own children,
	// so calling it from inside `EachChild` is destroying what the walk is
	// holding - the same collect-then-act `ResetPlayerGui` and
	// `ReclaimOrphanedCharacters` both make.
	//
	// @param store     The world.
	// @param container What to empty. A null entity empties nothing.
	// @return How many children were destroyed.
	// @since v0.17
	size_t ClearChildren(ecs::Store &store, ecs::Entity container);

	// How many `Player` instances the world's `Players` service holds.
	//
	// **Filtered by class**, because `Players` is an ordinary container and a
	// `Folder` somebody parented there is not an occupant.
	//
	// @param store The world.
	// @return The count, and zero for a world with no `Players`.
	// @since v0.17
	size_t PlayerCount(const ecs::Store &store);

	// The player holding one `UserId`, or a null entity.
	//
	// @param store  The world.
	// @param userId The number to look for.
	// @return The `Player`, or `ecs::NULL_ENTITY`.
	// @since v0.17
	ecs::Entity PlayerByUserId(const ecs::Store &store, int64_t userId);

	// What a player's containers are called.
	//
	// **`PlayerGui` is spelled here and again in `gui::PLAYER_GUI`**, because
	// `gui` decides whether a `ScreenGui` draws by walking its ancestors and
	// comparing names, and the two modules may not link each other -
	// `gui/AGENTS.md` refuses the edge, and this is the other side of the same
	// refusal.
	//
	// `examples/tests/Scene.cpp` pins the two against each other from the one
	// place both are linked. A rename on either side without the other would not
	// break a build; it would make every client interface stop drawing.
	//
	// The other three are named here alone. They are looked up by name rather
	// than by class because they are plain `Instance`s: a `Backpack` class would
	// be a row every consumer of the class table then has to describe, plus an
	// `Instance.new("Backpack")` that mints a container belonging to nobody.
	//
	//@{
	// @since v0.8
	inline constexpr std::string_view PLAYER_GUI_NAME = "PlayerGui";

	// @since v0.17
	inline constexpr std::string_view PLAYER_SCRIPTS_NAME = "PlayerScripts";

	// @since v0.17
	inline constexpr std::string_view BACKPACK_NAME = "Backpack";

	// @since v0.17
	inline constexpr std::string_view STARTER_GEAR_NAME = "StarterGear";
	//@}

	// The `Player` class id, registering the service tree on first call.
	//
	// @return The class id.
	ecs::ClassId PlayerClass();

	// --- who may be shown what ---------------------------------------------
	//
	// **`ServiceComponent::Scope` has existed since v0.7 and nothing read it.**
	// It round-trips through a save file and it is in the properties panel, and
	// the wire ignored it - so `ServerScriptService` and `ServerStorage`, the two
	// containers whose entire purpose is that a client must not see them, were
	// replicated to every client along with everything else. A game's server
	// scripts and its unreleased content went down the wire.
	//
	// **These are predicates and not a filter, and that split is the tier.**
	// `mono.engine/replication` does not depend on `scene` and must not: the
	// wire's job is to move components and it has no business knowing what a
	// service is. `replication::Authority::Interest` is the hook that exists for
	// exactly this - a host that knows both sides installs a predicate built from
	// these. So the *rule* lives here, beside the scope it reads, and the
	// *plumbing* lives in whoever owns a connection.

	// Which service an instance lives under.
	//
	// **Walks to the root rather than reading the instance**, because scope is a
	// containment fact: a `Script` in `ServerScriptService` is server-only
	// because of where it is, and moving it to `ReplicatedStorage` must change
	// the answer with nothing else being edited. A flag copied onto every
	// descendant would be a second place the answer lived, and it would be the
	// one that went stale on a reparent.
	//
	// @param store    The world.
	// @param instance Anything in the tree.
	// @return The scope of the service it is under. `Shared` for an instance
	//         under no service at all, which is the safe answer for an orphan a
	//         script has created and not yet parented - it is not yet secret,
	//         and treating it as secret would make a client miss it forever if
	//         the script then parents it into `Workspace`.
	// @since v0.15
	ServiceScope ScopeOfInstance(const ecs::Store &store, ecs::Entity instance);

	// Whether a client may be shown this instance at all.
	//
	// **A whitelist by scope rather than a blacklist by name**, which is the
	// difference between a rule and a list somebody has to remember. A tenth
	// service added tomorrow is covered the moment it declares a scope; a list of
	// forbidden names would not have covered it, and the failure would be silent
	// and in the direction of leaking.
	//
	// @param store    The world.
	// @param instance Anything in the tree.
	// @return `false` for anything under a `Server`-scoped service.
	// @since v0.15
	bool VisibleToClients(const ecs::Store &store, ecs::Entity instance);

	// Which player's own subtree this instance is in, or a null entity.
	//
	// **The other half of the whitelist, and it is per client rather than per
	// world.** A player's `PlayerGui` is that player's - a second client seeing
	// it would see an interface it cannot interact with, and a script writing
	// into one player's would be writing into everybody's. Scope cannot express
	// that, because `Players` is `Shared`: both halves need the *list* of who is
	// connected, and only the owner needs what is under their own row.
	//
	// @param store    The world.
	// @param instance Anything in the tree.
	// @return The `Player` this is under, or `ecs::NULL_ENTITY` for anything
	//         that is not under one - which is almost everything.
	// @since v0.15
	ecs::Entity PlayerOwning(const ecs::Store &store, ecs::Entity instance);
}
