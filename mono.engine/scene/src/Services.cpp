#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/Teams.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::scene {

	namespace {
		using ecs::Classes;
		using ecs::ClassId;
		using ecs::Entity;
		using ecs::NULL_ENTITY;
		using ecs::PropertyDescriptor;
		using ecs::PropertyKind;
		using ecs::PropertyType;
		using ecs::Store;

		// The names `ServiceScope` reads and writes as.
		//
		// Ordered to match the enum, because the conversion below is an index
		// either way and a table that drifted from the enum would rename a
		// service's audience silently.
		constexpr std::array<std::string_view, 3> SCOPES{"Shared", "Server", "Client"};

		core::Name ScopeName(ServiceScope scope) {
			const auto index = static_cast<size_t>(scope);
			return core::Name(SCOPES[index < SCOPES.size() ? index : 0]);
		}

		ServiceScope ScopeOf(core::Name name) {
			for (size_t index = 0; index < SCOPES.size(); index++) {
				if (name == core::Name(SCOPES[index])) {
					return static_cast<ServiceScope>(index);
				}
			}
			return ServiceScope::Shared;
		}

		// Scope, as a name rather than as a byte.
		//
		// Computed rather than a plain field, because the stored form is a
		// `uint8_t` and what a script, a properties panel and a save file all
		// want is the word. The same trade `Material` makes one file over: the
		// component holds what is cheap to iterate and the property holds what
		// is legible.
		// `Players.LocalPlayer`.
		//
		// **A property, so no binding ever learns the name.** `LuauInstances.cpp`
		// switches on `PropertyType` and nothing else - that is what makes a
		// property declared here readable from Luau, from JavaScript and in the
		// properties panel with none of them changing. A `if (name ==
		// "LocalPlayer")` in the script layer would be the second source of
		// truth the whole conversion design exists to prevent.
		//
		// **Read-only, because who you are is not yours to assign.** A script
		// setting `LocalPlayer` would be a client claiming to be somebody else,
		// which is the one thing a client must not be able to say.
		PropertyDescriptor LocalPlayerProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("LocalPlayer");
			property.Type = PropertyType::Reference;
			property.Size = sizeof(Entity);
			property.Kind = PropertyKind::Computed;
			property.Writable = false;

			// Reads nothing on the row: the answer is a resource, because there
			// is one of it per world. An empty set rather than null, so every
			// consumer that walks `Reads` finds a set to walk.
			property.Reads = &ecs::ComponentSet::Intern({});
			property.Writes = property.Reads;

			property.Get = [](const Store &store, Entity, void *out) -> bool {
				const LocalPlayer *local = store.Resource<LocalPlayer>();

				// **Nil on a server, and that is the feature.** A `Script` that
				// reaches for it gets nothing rather than somebody else's
				// player - the mistake this separation exists to make
				// impossible.
				*static_cast<Entity *>(out) = local != nullptr ? local->Instance : NULL_ENTITY;
				return true;
			};
			return property;
		}

		// One of a player's four containers, as a reference a script can name.
		//
		// **The containers existed and nothing could name them.** `AddPlayer`
		// creates all four beside every player - `gui::Layout` draws a
		// `ScreenGui` only from `StarterGui` or from a player's `PlayerGui`, and
		// the spawn pipeline refills `Backpack` from `StarterGear` - so a script
		// wanting to reach one had to find the child by string and hope the
		// spelling matched the one the engine uses. The name constants are that
		// spelling, and these are them handed over rather than retyped.
		//
		// **A lookup rather than a stored handle**, and that is the same call
		// `Character` makes one property down: a stored entity is a second place
		// the answer lives, and it goes stale the moment somebody reparents or
		// destroys the container. Finding the child by name is one source of
		// truth and costs a walk of a player's few children.
		//
		// **Read-only, all four.** Roblox does not let a script assign any of
		// them either, and here the reason is sharper: every consumer finds the
		// container by walking the tree, so a script pointing the property
		// somewhere else would move nothing - the interface would still draw
		// from the real container and the property would describe a place it is
		// not.
		//
		// **A template over the name rather than four near-identical
		// functions.** `PropertyDescriptor::Get` is a raw function pointer, so
		// the conversion has to be captureless - which makes the container's
		// name a template argument and not a parameter. That is the same
		// constraint `Classes::ClampedProperty` is built around.
		//
		// @since v0.15
		template <const std::string_view &Container> PropertyDescriptor ContainerProperty() {
			PropertyDescriptor property;
			property.Name = core::Name(Container);
			property.Type = PropertyType::Reference;
			property.Size = sizeof(Entity);
			property.Kind = PropertyKind::Computed;
			property.Writable = false;

			// Reads nothing on the row: the answer is a child, and children are
			// the tree rather than a component. An empty set rather than null,
			// so every consumer that walks `Reads` finds a set to walk.
			property.Reads = &ecs::ComponentSet::Intern({});
			property.Writes = property.Reads;

			property.Get = [](const Store &store, Entity instance, void *out) -> bool {
				// **Nil rather than a failure for a player who has none**, which
				// is a player some other host built without going through
				// `AddPlayer`. `if player.PlayerGui then` is how a script asks,
				// and it is the same shape `Character` answers with.
				*static_cast<Entity *>(out) = store.FindFirstChild(instance, Container);
				return true;
			};
			return property;
		}

		// `Player.UserId`, the number a game keys saved data by.
		//
		// **Read-only, for `Players.LocalPlayer`'s reason one property up: who
		// you are is not yours to assign.** A script that could write this could
		// read another player's data store entry by claiming their number, which
		// is the one thing a shared world must not let a game script do by
		// accident. The host assigns it at `AddPlayer` and nothing else may.
		//
		// @since v0.17
		PropertyDescriptor UserIdProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("UserId");
			property.Type = PropertyType::Int64;
			property.Size = sizeof(int64_t);
			property.Kind = PropertyKind::Computed;
			property.Writable = false;
			// **Reads the row and writes nothing, and the two sets differ for
			// once.** A `.Changed` listener should hear a `UserId` that was
			// assigned, so `Reads` names the component it comes off; nothing may
			// assign it, so `Writes` is empty - which is the contradiction
			// `mono.tools/bindings` refuses when a read-only property claims a
			// write set, and it refused this one first.
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<PlayerIdentity>()});
			property.Writes = &ecs::ComponentSet::Intern({});

			property.Get = [](const Store &store, Entity instance, void *out) -> bool {
				const PlayerIdentity *identity = store.Get<PlayerIdentity>(instance);
				if (identity == nullptr) {
					return false;
				}
				*static_cast<int64_t *>(out) = identity->UserId;
				return true;
			};
			return property;
		}

		// `Players.NumPlayers`, which is `#Players:GetPlayers()` without the
		// table.
		//
		// **Counted rather than kept.** A field incremented on arrival and
		// decremented on departure is a second copy of a fact the tree already
		// holds, and it is the copy that goes wrong the first time a script
		// calls `player:Destroy()` - rule 2, with a scoreboard attached. A
		// player's few siblings are a short walk.
		//
		// @since v0.17
		PropertyDescriptor NumPlayersProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("NumPlayers");
			property.Type = PropertyType::Int32;
			property.Size = sizeof(int32_t);
			property.Kind = PropertyKind::Computed;
			property.Writable = false;

			// Reads nothing on the row: the answer is the tree.
			property.Reads = &ecs::ComponentSet::Intern({});
			property.Writes = property.Reads;

			property.Get = [](const Store &store, Entity, void *out) -> bool {
				*static_cast<int32_t *>(out) = static_cast<int32_t>(PlayerCount(store));
				return true;
			};
			return property;
		}

		// `Player.Character`, and the whole of what a game has to say to make a
		// body walk.
		//
		// **Writable, unlike `Players.LocalPlayer` beside it.** Roblox assigns
		// this from `Player:LoadCharacter()` and lets a game assign it directly;
		// so does this. `SetPlayerCharacter` carries the argument for what the
		// assignment does - it is the setter, not a field write, because
		// binding a model to a player is three rows and an ownership grant
		// rather than one entity handle.
		//
		// **Computed rather than a field**, because a `Player` is registered
		// with an empty component set: `PlayerCharacter` is added to the row
		// when the first character is assigned and is absent before that, which
		// a generated field property has no way to answer nil for.
		PropertyDescriptor CharacterProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Character");
			property.Type = PropertyType::Reference;
			property.Size = sizeof(Entity);
			property.Kind = PropertyKind::Computed;

			// Names the row the answer comes from, so a `.Changed` listener
			// fires when a player is given or loses a body. `CurrentCamera` says
			// why naming the nearest component beats naming nothing.
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<PlayerCharacter>()});
			property.Writes = property.Reads;

			property.Get = [](const Store &store, Entity instance, void *out) -> bool {
				// **Nil rather than a failure for a player who has none.** A
				// player between a death and a respawn is an ordinary state, and
				// `if player.Character then` is how a script asks about it.
				*static_cast<Entity *>(out) = CharacterOf(store, instance);
				return true;
			};

			property.Set = [](Store &store, Entity instance, const void *value) -> bool {
				return SetPlayerCharacter(store, instance, *static_cast<const Entity *>(value));
			};

			return property;
		}

		// `Player.Team`, and the only reason this class exists.
		//
		// **Writable, unlike every other reference on a `Player` but
		// `Character`.** Which side somebody is on is a decision a game script
		// makes and remakes - a round ends, an autobalance runs - where
		// `UserId`, `PlayerGui` and the rest are facts about the connection.
		// `SetPlayerTeam` is the setter rather than a field write because it
		// refuses anything that is not a live `Team`, which is where that
		// mistake is cheapest to catch.
		//
		// **Computed rather than a field**, for `Character`'s reason one
		// property up: a `Player` is registered with `PlayerIdentity` alone, so
		// `PlayerTeam` is added to the row on the first assignment and is absent
		// before it - which a generated field property has no way to answer nil
		// for.
		//
		// @since v0.15
		PropertyDescriptor TeamProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Team");
			property.Type = PropertyType::Reference;
			property.Size = sizeof(Entity);
			property.Kind = PropertyKind::Computed;

			// Names the row the answer comes from, so a `.Changed` listener
			// fires when somebody swaps sides.
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<PlayerTeam>()});
			property.Writes = property.Reads;

			property.Get = [](const Store &store, Entity instance, void *out) -> bool {
				// **Nil rather than a failure for a player on no side**, which
				// is the state a world starts in and stays in until a game says
				// otherwise. `if player.Team then` is how a script asks.
				*static_cast<Entity *>(out) = TeamOf(store, instance);
				return true;
			};

			property.Set = [](Store &store, Entity instance, const void *value) -> bool {
				return SetPlayerTeam(store, instance, *static_cast<const Entity *>(value));
			};

			return property;
		}

		PropertyDescriptor ScopeProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("Scope");
			property.Type = PropertyType::Enum;
			property.EnumName = core::Name("ServiceScope");
			property.Size = sizeof(core::Name);
			property.Kind = PropertyKind::Computed;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<ServiceComponent>()});
			property.Writes = property.Reads;

			property.Get = [](const Store &store, Entity instance, void *out) -> bool {
				const ServiceComponent *service = store.Get<ServiceComponent>(instance);
				if (service == nullptr) {
					return false;
				}
				*static_cast<core::Name *>(out) = ScopeName(service->Scope);
				return true;
			};

			property.Set = [](Store &store, Entity instance, const void *value) -> bool {
				ServiceComponent *service = store.GetMutable<ServiceComponent>(instance);
				if (service == nullptr) {
					return false;
				}
				service->Scope = ScopeOf(*static_cast<const core::Name *>(value));
				return true;
			};
			return property;
		}

		// One service: what to call it, who sees it, and where it sits.
		//
		// **A table rather than thirteen registration calls**, for `input`'s
		// `BINDINGS` reason: the list of services is the thing being described,
		// and a tenth service should be a line here rather than an edit in
		// three functions. `InstallServices` walks the same table it was
		// registered from, so a class that exists and is never created is not a
		// state this can be in.
		struct ServiceDesc {
			std::string_view Name;
			ServiceScope Scope;

			// The service this one lives under, or empty for a root.
			//
			// `StarterPlayerScripts` is a child of `StarterPlayer` in Roblox
			// and it is one here. It is listed as a service rather than as an
			// ordinary folder because that is what it is - a client asks for it
			// by name.
			std::string_view Parent;
		};

		constexpr std::array<ServiceDesc, 13> SERVICES{{
			// **Workspace first, and the order matters.** These are created in
			// this order and the explorer draws roots in creation order, so
			// this is the order an author sees - Workspace at the top, exactly
			// as Studio has it.
			{"Workspace", ServiceScope::Shared, {}},
			{"Lighting", ServiceScope::Shared, {}},
			{"ReplicatedFirst", ServiceScope::Shared, {}},
			{"ReplicatedStorage", ServiceScope::Shared, {}},
			{"ServerScriptService", ServiceScope::Server, {}},
			{"ServerStorage", ServiceScope::Server, {}},
			{"StarterGui", ServiceScope::Client, {}},

			// **The gear template, and it is copied on the join rather than on
			// the spawn.** Roblox clones this into `Player.StarterGear` once,
			// and refills `Backpack` from *that* on every respawn - so a tool a
			// game grants at run time is added to `StarterGear` and survives
			// dying, where one added to `Backpack` does not. Two containers,
			// two lifetimes; collapsing them is how "my tool vanished when I
			// died" happens.
			{"StarterPack", ServiceScope::Client, {}},

			{"StarterPlayer", ServiceScope::Client, {}},
			{"StarterPlayerScripts", ServiceScope::Client, "StarterPlayer"},

			// **The other half of the pair, and the difference between them is
			// *when*.** `StarterPlayerScripts` is copied once, on the join;
			// this is copied into the character model on every spawn, because
			// what it holds is a script about a body and the body is new each
			// time. A world with one container instead of two cannot express a
			// script that should survive a death.
			{"StarterCharacterScripts", ServiceScope::Client, "StarterPlayer"},

			// **`Shared`, because both halves need it and for different
			// things.** A server enumerates who is connected; a client asks
			// which of them is itself. A `Client` scope would hide the list
			// from the authority that maintains it.
			{"Players", ServiceScope::Shared, {}},

			// **`Shared`, and for exactly `Players`' reason.** A server decides
			// who is on which side and puts them there; a client asks which
			// side it is on so it can colour a name and read a scoreboard. A
			// `Client` scope would hide the list from the authority that
			// maintains it, and a `Server` one would leave a client unable to
			// answer `LocalPlayer.Team`.
			//
			// **Last, so it is last in the explorer.** These are created in the
			// order this table lists and the explorer draws roots in creation
			// order, so appending here is the same rule
			// `RegisterSceneComponents` keeps one file over.
			{"Teams", ServiceScope::Shared, {}},
		}};

		// `workspace.CurrentCamera`: a reference over the `ActiveCamera` resource.
		//
		// **The resource is the storage and this is the only way to reach it from
		// a script**, which is the whole design. `ActiveCamera` has named the live
		// camera since v0.4 and there was no property projecting it, so a script
		// could create a `Camera` and had no way to say "look through this one" -
		// it had to be handed one by whatever built the world.
		//
		// **A read resolves the resource and a write moves it.** Not a component
		// on `Workspace`: that would be a second place the live camera is
		// recorded, and the two would disagree the first time anything set the
		// resource directly - which `client::InstallDefaultCamera` does.
		//
		// **`PropertyKind::Resource` and not `Structural`**: structural means the
		// write moves the row to another archetype, and this moves nothing. What
		// it touches is outside the entity entirely. That was recorded here in
		// prose while there was one such property; there are two now, and
		// `ecs::AuditProperties` cannot tell a setter that writes a resource from
		// a setter that forgot to mark a component without being told which this
		// is.
		PropertyDescriptor CurrentCameraProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("CurrentCamera");
			property.Type = PropertyType::Reference;
			property.Size = sizeof(ecs::Entity);
			property.Kind = PropertyKind::Resource;

			// **Names the `Camera` component rather than nothing**, which is the
			// nearest honest answer: what this projects is a resource, and
			// `Reads`/`Writes` can only name components. A `.Changed` listener on
			// this property therefore fires when a camera row is written rather
			// than when the live one changes - over-reporting, which is the
			// direction `ecs::ChangeChannel` says to err in.
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Camera>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity, void *out) -> bool {
				const ActiveCamera *active = store.Resource<ActiveCamera>();

				// A world with no live camera answers nil rather than failing.
				// `Instance.new("Camera")` before anything has been made live is
				// an ordinary state, and a getter that returned false would make
				// `workspace.CurrentCamera` an error rather than a nil.
				*static_cast<ecs::Entity *>(out) = active == nullptr ? ecs::NULL_ENTITY : active->Entity;
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity, const void *value) -> bool {
				const auto camera = *static_cast<const ecs::Entity *>(value);

				// **Refused for anything that is not a camera.** Assigning a part
				// here would leave `ResolveActiveCamera` looking for a `Camera`
				// component it will not find, so the matrices would silently stay
				// as they were - a frame drawn from where the camera used to be,
				// which reads as a renderer fault. `ActiveCamera.hpp` describes
				// that exact failure and this is what keeps a script from causing
				// it.
				//
				// Clearing is allowed: `workspace.CurrentCamera = nil` is how a
				// script says "nothing is looking", and the resource keeps its
				// last matrices for the reason `ResolveActiveCamera` gives.
				if (camera != ecs::NULL_ENTITY && store.Get<Camera>(camera) == nullptr) {
					return false;
				}

				ActiveCamera live;
				if (const ActiveCamera *existing = store.Resource<ActiveCamera>()) {
					// **Read-modify-write, so the aspect ratio and the matrices
					// survive.** A fresh `ActiveCamera` would reset `AspectRatio`
					// to 1, and the consumer that owns that number - a window, a
					// mirror's texture - writes it once rather than every frame.
					// A script changing camera would have squashed the view until
					// the next resize.
					live = *existing;
				}
				live.Entity = camera;
				store.SetResource(live);
				return true;
			};

			return property;
		}

		// `workspace.SurfaceBounces`: how deep a chain of mirrors this world
		// resolves, over the `SurfaceBounces` resource.
		//
		// **On `Workspace` because it is a statement about the scene**, and the
		// scene is what is in there. The alternative reading - that it is a
		// quality setting and belongs beside `Lighting`'s - is the one the
		// command line already had and is exactly what this replaces: a corridor
		// of facing panes needs a different number from a room with one mirror
		// in it, and which of those a world *is* is not something a session
		// picks.
		//
		// **The resource is the storage and this is the only way in**, which is
		// `CurrentCameraProperty`'s design and the reason it takes the same
		// shape: a component on `Workspace` would be a second place the number
		// lived, and the two would disagree the first time a host set the
		// resource directly.
		//
		// Zero is `AUTOMATIC_SURFACE_BOUNCES` and is the default, so a world
		// that never says anything writes nothing into its own file.
		PropertyDescriptor SurfaceBouncesProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("SurfaceBounces");
			property.Type = PropertyType::Int32;
			property.Size = sizeof(int32_t);
			property.Kind = PropertyKind::Resource;

			// **The resource's own component id, which `CurrentCamera` could
			// not name.** A resource is keyed by a component id like anything
			// else, so a `.Changed` listener on this fires when this number is
			// written and not when something adjacent is.
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<SurfaceBounces>()});
			property.Writes = property.Reads;

			property.Get = [](const ecs::Store &store, ecs::Entity, void *out) -> bool {
				*static_cast<int32_t *>(out) = SurfaceBouncesOf(store);
				return true;
			};

			property.Set = [](ecs::Store &store, ecs::Entity, const void *value) -> bool {
				// **A negative is refused rather than clamped**, because it is
				// the one value that cannot be a mistake about the ceiling: too
				// large is a world asking for more than the device will
				// allocate, which is drawn at what it can, and below zero is a
				// world asking for something the word does not mean.
				const auto levels = *static_cast<const int32_t *>(value);
				if (levels < 0) {
					return false;
				}

				store.SetResource(SurfaceBounces{levels});
				return true;
			};

			return property;
		}

		ClassId RegisterServiceTree() {
			// The root of everything, and the components these classes are sets
			// of, both through `PartClass`. A service derives from `Instance`,
			// so registering that again here would be a second root - and
			// `Classes::Register` handing back the existing id for a repeated
			// name would have hidden it rather than refused it.
			EnsureClassTree();

			ecs::EnumTable::Register("ServiceScope", SCOPES);

			const ClassId instance = Classes::Find(core::Name("Instance"));

			// **Abstract, and the class picker knows it by this rather than by
			// a list of names.** `Explorer.cpp` filters the palette to
			// descendants of `Instance` and then writes out the handful of
			// bases nobody can instantiate. Services are excluded as a
			// *category*, so a tenth service never touches that function - a
			// world has exactly one of each and `InstallServices` is what puts
			// it there.
			const std::array serviceSet{ecs::Components::Of<ServiceComponent>()};
			const ClassId service = Classes::Register("Service", instance, serviceSet);

			const std::array lightingSet{
				ecs::Components::Of<ServiceComponent>(),
				ecs::Components::Of<LightingServiceComponent>(),
			};

			const std::array playersSet{
				ecs::Components::Of<ServiceComponent>(),
				ecs::Components::Of<PlayersServiceComponent>(),
			};

			for (const ServiceDesc &desc : SERVICES) {
				if (desc.Name == "Lighting") {
					Classes::Register(desc.Name, service, lightingSet);
					continue;
				}
				if (desc.Name == "Players") {
					Classes::Register(desc.Name, service, playersSet);
					continue;
				}
				Classes::Register(desc.Name, service, {});
			}

			// **A `Player` is an instance, not a service.** There are many of
			// them, they come and go, and a script reaches one through
			// `Players` - which is exactly the shape the instance tree already
			// has. Derived from `Instance` rather than from `Service` so the
			// class picker's service exclusion does not hide it, and so a world
			// can hold as many as it has occupants.
			const std::array playerSet{ecs::Components::Of<PlayerIdentity>()};
			const ClassId player = Classes::Register("Player", instance, playerSet);

			// **A `Team` is an instance for `Player`'s reason**, one line up: a
			// game makes as many as it has sides, destroys them when a round
			// ends, and reaches them through `Teams`. Derived from `Instance` so
			// the class picker's service exclusion does not hide it.
			const std::array teamSet{ecs::Components::Of<Team>()};
			const ClassId team = Classes::Register("Team", instance, teamSet);

			// **The one hook a game needs to make a character walk.** Setting it
			// is what links a model to a player, and every consumer downstream -
			// the client's move submission, the host's move application, the
			// camera - reads that link rather than being told separately.
			Classes::Computed(player, CharacterProperty());

			// **The four containers a join makes**, none of which a script could
			// name before. See `ContainerProperty`.
			Classes::Computed(player, ContainerProperty<PLAYER_GUI_NAME>());
			Classes::Computed(player, ContainerProperty<PLAYER_SCRIPTS_NAME>());
			Classes::Computed(player, ContainerProperty<BACKPACK_NAME>());
			Classes::Computed(player, ContainerProperty<STARTER_GEAR_NAME>());

			Classes::Computed(player, UserIdProperty());
			Classes::Property<&PlayerIdentity::DisplayName>(player, "DisplayName");

			// **Per player as well as per world, which is Roblox's
			// arrangement.** `Players.RespawnTime` is what a new occupant starts
			// from and `Player.RespawnTime` is what that occupant actually
			// waits, so a game can hold one person out of a round without
			// changing the rule for everybody.
			Classes::Property<&PlayerIdentity::RespawnTime>(player, "RespawnTime");

			// **The side, and it is what decides where a respawn puts them.**
			// `FindSpawn` reads this against every `SpawnLocation` in the
			// workspace - see `Teams.hpp` - which is what keeps a team from
			// being a coloured label.
			Classes::Computed(player, TeamProperty());

			// **`TeamColor` and not `Colour`**, which is the spelling rule this
			// codebase keeps: a script written against Roblox says
			// `team.TeamColor`, and a second word for one thing is the debt
			// `AGENTS.md` calls the most expensive kind. The *field* is British
			// because it is local code; the *property* is the one that crosses.
			Classes::Property<&Team::Colour>(team, "TeamColor");

			Classes::Computed(service, ScopeProperty());

			const ClassId workspace = Classes::Find(core::Name("Workspace"));
			Classes::Computed(workspace, CurrentCameraProperty());
			Classes::Computed(workspace, SurfaceBouncesProperty());

			const ClassId players = Classes::Find(core::Name("Players"));
			Classes::Computed(players, LocalPlayerProperty());
			Classes::Computed(players, NumPlayersProperty());
			Classes::Property<&PlayersServiceComponent::MaxPlayers>(players, "MaxPlayers");
			Classes::Property<&PlayersServiceComponent::RespawnTime>(players, "RespawnTime");
			Classes::Property<&PlayersServiceComponent::CharacterAutoLoads>(players, "CharacterAutoLoads");
			Classes::Property<&ServiceComponent::Fixture>(service, "Fixture");

			const ClassId lighting = Classes::Find(core::Name("Lighting"));
			Classes::Property<&LightingServiceComponent::Ambient>(lighting, "Ambient");
			Classes::Property<&LightingServiceComponent::OutdoorAmbient>(lighting, "OutdoorAmbient");
			Classes::Property<&LightingServiceComponent::FogColor>(lighting, "FogColor");
			Classes::Property<&LightingServiceComponent::Brightness>(lighting, "Brightness");
			Classes::Property<&LightingServiceComponent::ClockTime>(lighting, "ClockTime");
			Classes::Property<&LightingServiceComponent::FogStart>(lighting, "FogStart");
			Classes::Property<&LightingServiceComponent::FogEnd>(lighting, "FogEnd");
			Classes::Property<&LightingServiceComponent::GeographicLatitude>(lighting, "GeographicLatitude");

			return service;
		}
	}

	ecs::ClassId ServiceClass() {
		static const ClassId service = RegisterServiceTree();
		return service;
	}

	Entity ServiceOf(const Store &store, ecs::ClassId klass) {
		if (!klass.IsValid()) {
			return NULL_ENTITY;
		}

		Entity found = NULL_ENTITY;
		store.EachRoot([&](Entity root) {
			// **The first in `EachRoot`'s order**, which is creation order - the
			// same tie-break `FindFirstRoot` made, kept so a world that somehow
			// holds two of one service resolves to the same one it always did.
			if (found == NULL_ENTITY && store.IsA(root, klass)) {
				found = root;
			}
		});
		return found;
	}

	Entity ServiceUnder(const Store &store, Entity parent, ecs::ClassId klass) {
		if (parent == NULL_ENTITY || !klass.IsValid()) {
			return NULL_ENTITY;
		}
		return store.FindFirstChildWhichIsA(parent, klass);
	}

	Entity WorkspaceOf(const Store &store) {
		return ServiceOf(store, Classes::Find(core::Name("Workspace")));
	}

	Entity PlayersOf(const Store &store) {
		return ServiceOf(store, Classes::Find(core::Name("Players")));
	}

	bool InReplicatedFirst(const Store &store, Entity instance) {
		const ecs::ClassId klass = Classes::Find(core::Name("ReplicatedFirst"));
		if (!klass.IsValid()) {
			return false;
		}

		// **Up the chain rather than down from the service**, which is
		// `ScopeOfInstance`'s walk and its argument: containment is the fact,
		// and a flag copied onto every descendant would be the one that went
		// stale on a reparent. A world's tree is shallow, and this is asked once
		// per entity per publish beside a walk that already happens.
		//
		// Bounded by the walk itself: `SetParent` refuses a cycle, so every
		// chain ends.
		for (Entity at = instance; at != NULL_ENTITY; at = store.ParentOf(at)) {
			if (store.IsA(at, klass)) {
				return true;
			}
		}
		return false;
	}

	ServiceScope ScopeOfInstance(const Store &store, Entity instance) {
		// **Up to the root, and the *root* is what carries the scope.** A
		// service is always a root - `InstallServices` parents eleven of them to
		// nothing and `StarterPlayerScripts` to `StarterPlayer` - so walking to
		// the top and reading there is the same answer as reading the nearest
		// service on the way, with one lookup instead of one per level.
		//
		// **Bounded by the walk itself.** A cycle in the tree is not
		// representable: `SetParent` refuses one, so every chain ends.
		Entity at = instance;
		while (at != NULL_ENTITY) {
			const Entity above = store.ParentOf(at);
			if (above == NULL_ENTITY) {
				break;
			}
			at = above;
		}

		if (at == NULL_ENTITY) {
			return ServiceScope::Shared;
		}

		const ServiceComponent *service = store.Get<ServiceComponent>(at);

		// **Shared for a root that is not a service**, which is an orphan a
		// script made and has not parented yet. See the declaration for why
		// that is the safe answer rather than the cautious one.
		return service != nullptr ? service->Scope : ServiceScope::Shared;
	}

	bool VisibleToClients(const Store &store, Entity instance) {
		return ScopeOfInstance(store, instance) != ServiceScope::Server;
	}

	Entity PlayerOwning(const Store &store, Entity instance) {
		// **Stops at the first `Player` on the way up**, rather than walking to
		// the root and coming back down. A player's subtree is shallow - a
		// `PlayerGui` and what a script puts in it - and the answer for
		// everything else is found by reaching a root that is not a player,
		// which is the ordinary case and costs the same walk either way.
		const ClassId player = Classes::Find(core::Name("Player"));
		if (!player.IsValid()) {
			return NULL_ENTITY;
		}

		Entity at = instance;
		while (at != NULL_ENTITY) {
			if (store.IsA(at, player)) {
				return at;
			}
			at = store.ParentOf(at);
		}
		return NULL_ENTITY;
	}

	ecs::ClassId PlayerClass() {
		ServiceClass();
		return Classes::Find(core::Name("Player"));
	}

	size_t CloneChildrenInto(Store &store, Entity source, Entity destination) {
		if (source == NULL_ENTITY || destination == NULL_ENTITY) {
			return 0;
		}

		std::vector<Entity> sources;
		store.EachChild(source, [&](Entity child) { sources.push_back(child); });

		size_t cloned = 0;
		for (const Entity child : sources) {
			const Entity copy = store.CloneInstance(child);
			if (copy == NULL_ENTITY) {
				continue;
			}
			store.SetParent(copy, destination);
			cloned++;
		}
		return cloned;
	}

	size_t ClearChildren(Store &store, Entity container) {
		if (container == NULL_ENTITY) {
			return 0;
		}

		std::vector<Entity> going;
		store.EachChild(container, [&](Entity child) { going.push_back(child); });

		for (const Entity child : going) {
			store.DestroyInstance(child);
		}
		return going.size();
	}

	size_t PlayerCount(const Store &store) {
		const Entity players = PlayersOf(store);
		if (players == NULL_ENTITY) {
			return 0;
		}

		const ecs::ClassId player = Classes::Find(core::Name("Player"));
		size_t counted = 0;
		store.EachChild(players, [&](Entity child) { counted += store.IsA(child, player) ? 1u : 0u; });
		return counted;
	}

	Entity PlayerByUserId(const Store &store, int64_t userId) {
		const Entity players = PlayersOf(store);
		if (players == NULL_ENTITY) {
			return NULL_ENTITY;
		}

		Entity found = NULL_ENTITY;
		store.EachChild(players, [&](Entity child) {
			if (found != NULL_ENTITY) {
				return;
			}
			const PlayerIdentity *identity = store.Get<PlayerIdentity>(child);
			if (identity != nullptr && identity->UserId == userId) {
				found = child;
			}
		});
		return found;
	}

	Entity AddPlayer(Store &store, std::string_view name, bool local, int64_t userId) {
		const Entity players = PlayersOf(store);
		if (players == NULL_ENTITY) {
			// **Refused rather than furnished on the way past.** A caller adding
			// a player to a world with no `Players` has skipped
			// `InstallServices`, and quietly creating the service here would
			// make that omission invisible until something else asked for it.
			return NULL_ENTITY;
		}

		auto *settings = store.GetMutable<PlayersServiceComponent>(players);

		// **Refused when the world is full, which is what `MaxPlayers` means.**
		// A world from before that property existed has the component with its
		// default; a `Players` with none at all is one somebody built by hand,
		// and the honest answer for it is to admit rather than to invent a cap.
		if (settings != nullptr &&
			PlayerCount(store) >= static_cast<size_t>(std::max(0, settings->MaxPlayers))) {
			return NULL_ENTITY;
		}

		const Entity player = store.CreateInstance(PlayerClass(), name);
		if (player == NULL_ENTITY) {
			return NULL_ENTITY;
		}

		store.SetParent(player, players);

		PlayerIdentity identity;
		identity.DisplayName = core::Name(name);
		identity.RespawnTime = settings != nullptr ? settings->RespawnTime : 5.0f;

		// **Zero means "take the next one", and the counter is on the world.**
		// A host with an account system behind it passes the real number; one
		// without gets identities that are stable across two runs of the same
		// recording, which a clock or a random draw would not be.
		if (userId != 0) {
			identity.UserId = userId;
		} else if (settings != nullptr) {
			identity.UserId = settings->NextUserId++;
		}
		store.Set(player, identity);

		// **The four containers, because everything under a player has to be
		// somewhere and every consumer finds them by these names.**
		// `gui::Layout` draws a `ScreenGui` only from `StarterGui` or from a
		// player's `PlayerGui` - Roblox's containment rule - so a player without
		// one is a viewer that can never be shown an interface, and the symptom
		// is a black overlay rather than an error. The other three are the gear
		// pipeline and the client's own scripts.
		//
		// Made here rather than by whoever adds the player, for
		// `InstallServices`' reason: a container every consumer has to remember
		// to create is one somebody will not, and the failure is silent.
		//
		// **Plain `Instance`s rather than classes of their own.** `scene` may
		// not link `gui` - the refusal runs both ways, and `gui/AGENTS.md`
		// states this side of it - and what every containment test reads is the
		// *name*. So the spelling is what matters, and `examples/tests/Scene.cpp`
		// pins `PlayerGui` against `gui::PLAYER_GUI` from the one place both are
		// linked.
		const ecs::ClassId container = Classes::Find(core::Name("Instance"));
		for (const std::string_view label :
			 {PLAYER_GUI_NAME, PLAYER_SCRIPTS_NAME, BACKPACK_NAME, STARTER_GEAR_NAME}) {
			const Entity child = store.CreateInstance(container, std::string(label));
			if (child != NULL_ENTITY) {
				store.SetParent(child, player);
			}
		}

		// **The two copies a join makes, and neither is made again.**
		// `StarterPlayerScripts` is the client's own code and `StarterPack` is
		// the gear a game starts somebody with; a respawn refills `Backpack`
		// from what this put in `StarterGear`, which is why a tool granted at
		// run time survives a death and one dropped straight into `Backpack`
		// does not. `Services.hpp` carries the whole sequence.
		const Entity starterPlayer = ServiceOf(store, Classes::Find(core::Name("StarterPlayer")));
		CloneChildrenInto(
			store,
			ServiceUnder(store, starterPlayer, Classes::Find(core::Name("StarterPlayerScripts"))),
			store.FindFirstChild(player, PLAYER_SCRIPTS_NAME)
		);
		CloneChildrenInto(
			store,
			ServiceOf(store, Classes::Find(core::Name("StarterPack"))),
			store.FindFirstChild(player, STARTER_GEAR_NAME)
		);

		if (local) {
			// One per world, and the last marked wins. A resource rather than a
			// tag because there is one of it - `ecs/AGENTS.md`'s rule - and
			// because "who am I" is a question with one answer.
			store.SetResource(LocalPlayer{player});
		}
		return player;
	}

	Entity InstallServices(Store &store) {
		ServiceClass();

		Entity workspace = NULL_ENTITY;

		for (const ServiceDesc &desc : SERVICES) {
			// **Found before created, which is the whole of the idempotence.**
			// A world read out of a game file already has these as ordinary
			// instances - they were saved like everything else - so creating
			// them unconditionally would give an author two Workspaces, one of
			// which holds their scene and one of which is empty.
			// **Found by class, never by name, and that was a real bug.** These
			// lookups were `FindFirstRoot(desc.Name)`, so a script that renamed
			// the `Workspace` made this find nothing and mint a *second* one
			// beside the scene - see `ServiceOf`. A class survives a rename by
			// construction.
			const ClassId klass = Classes::Find(core::Name(desc.Name));

			Entity parent = NULL_ENTITY;
			if (!desc.Parent.empty()) {
				parent = ServiceOf(store, Classes::Find(core::Name(desc.Parent)));
				if (parent == NULL_ENTITY) {
					continue;
				}
			}

			Entity existing =
				parent == NULL_ENTITY ? ServiceOf(store, klass) : ServiceUnder(store, parent, klass);

			if (existing == NULL_ENTITY) {
				existing = store.CreateInstance(klass, desc.Name);
				if (existing == NULL_ENTITY) {
					continue;
				}

				if (parent != NULL_ENTITY) {
					store.SetParent(existing, parent);
				}
			}

			// Set every time rather than only on creation. A world from an old
			// file has the class but not the scope, and a service whose
			// audience defaulted to `Shared` is a `ServerStorage` that is not
			// one.
			if (ServiceComponent *component = store.GetMutable<ServiceComponent>(existing);
				component != nullptr) {
				component->Scope = desc.Scope;

				// **`Fixture` finally refuses something.** The field has said
				// since v0.7 that an author may not delete or reparent a
				// service, and nothing read it - a script could `Destroy()`
				// `Lighting` and the editor could delete it with the Delete key,
				// which is rule 6 in its plainest form. `Store::Protect` is the
				// seam and this is its only filler.
				//
				// **Here rather than at creation**, so a world read out of a
				// game file is protected too: the loop above finds those rather
				// than making them, and protection that only applied to freshly
				// minted services would hold in a new game and not in a saved
				// one - which is the worst half to be missing.
				if (component->Fixture) {
					store.Protect(existing);
				}
			}

			if (desc.Name == "Workspace") {
				workspace = existing;
			}
		}

		// **No camera here, and that is the correction.** A camera belongs to
		// whoever is looking, not to the world: the editor makes one to show its
		// viewport, a client makes one for its player, and several people editing
		// one game make one each. `InstallServices` furnishes a world with what
		// the *game* has, and a viewpoint is not that - putting one here wrote
		// somebody's camera into every game file, which is what
		// `TransientComponent` now exists to prevent.
		//
		// The viewer creates its own and marks it transient. See
		// `studio::Editor::EnsureViewerCamera`.

		return workspace;
	}
}
