#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>

#include <array>
#include <string_view>

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
		// **A property, so no binding ever learns the name.** `Instances.cpp`
		// switches on `PropertyType` and nothing else — that is what makes a
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
				// player — the mistake this separation exists to make
				// impossible.
				*static_cast<Entity *>(out) = local != nullptr ? local->Instance : NULL_ENTITY;
				return true;
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
		// **A table rather than nine registration calls**, for `input`'s
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
			// ordinary folder because that is what it is — a client asks for it
			// by name.
			std::string_view Parent;
		};

		constexpr std::array<ServiceDesc, 10> SERVICES{{
			// **Workspace first, and the order matters.** These are created in
			// this order and the explorer draws roots in creation order, so
			// this is the order an author sees — Workspace at the top, exactly
			// as Studio has it.
			{"Workspace", ServiceScope::Shared, {}},
			{"Lighting", ServiceScope::Shared, {}},
			{"ReplicatedFirst", ServiceScope::Shared, {}},
			{"ReplicatedStorage", ServiceScope::Shared, {}},
			{"ServerScriptService", ServiceScope::Server, {}},
			{"ServerStorage", ServiceScope::Server, {}},
			{"StarterGui", ServiceScope::Client, {}},
			{"StarterPlayer", ServiceScope::Client, {}},
			{"StarterPlayerScripts", ServiceScope::Client, "StarterPlayer"},

			// **`Shared`, because both halves need it and for different
			// things.** A server enumerates who is connected; a client asks
			// which of them is itself. A `Client` scope would hide the list
			// from the authority that maintains it.
			{"Players", ServiceScope::Shared, {}},
		}};

		// `workspace.CurrentCamera`: a reference over the `ActiveCamera` resource.
		//
		// **The resource is the storage and this is the only way to reach it from
		// a script**, which is the whole design. `ActiveCamera` has named the live
		// camera since v0.4 and there was no property projecting it, so a script
		// could create a `Camera` and had no way to say "look through this one" —
		// it had to be handed one by whatever built the world.
		//
		// **A read resolves the resource and a write moves it.** Not a component
		// on `Workspace`: that would be a second place the live camera is
		// recorded, and the two would disagree the first time anything set the
		// resource directly — which `client::InstallDefaultCamera` does.
		//
		// **`PropertyKind::Computed` and not `Structural`**, even though it writes
		// a resource rather than a component: structural means the write moves the
		// row to another archetype, and this moves nothing. What it does touch is
		// outside the entity entirely, which is a case `PropertyKind` has no
		// member for — recorded here rather than by inventing one for a single
		// property.
		PropertyDescriptor CurrentCameraProperty() {
			PropertyDescriptor property;
			property.Name = core::Name("CurrentCamera");
			property.Type = PropertyType::Reference;
			property.Size = sizeof(ecs::Entity);
			property.Kind = PropertyKind::Computed;

			// **Names the `Camera` component rather than nothing**, which is the
			// nearest honest answer: what this projects is a resource, and
			// `Reads`/`Writes` can only name components. A `.Changed` listener on
			// this property therefore fires when a camera row is written rather
			// than when the live one changes — over-reporting, which is the
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
				// as they were — a frame drawn from where the camera used to be,
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
					// to 1, and the consumer that owns that number — a window, a
					// mirror's texture — writes it once rather than every frame.
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

		ClassId RegisterServiceTree() {
			// The root of everything, and the components these classes are sets
			// of, both through `PartClass`. A service derives from `Instance`,
			// so registering that again here would be a second root — and
			// `Classes::Register` handing back the existing id for a repeated
			// name would have hidden it rather than refused it.
			EnsureClassTree();

			ecs::EnumTable::Register("ServiceScope", SCOPES);

			const ClassId instance = Classes::Find(core::Name("Instance"));

			// **Abstract, and the class picker knows it by this rather than by
			// a list of names.** `Explorer.cpp` filters the palette to
			// descendants of `Instance` and then writes out the handful of
			// bases nobody can instantiate. Services are excluded as a
			// *category*, so a tenth service never touches that function — a
			// world has exactly one of each and `InstallServices` is what puts
			// it there.
			const std::array serviceSet{ecs::Components::Of<ServiceComponent>()};
			const ClassId service = Classes::Register("Service", instance, serviceSet);

			const std::array lightingSet{
				ecs::Components::Of<ServiceComponent>(),
				ecs::Components::Of<LightingServiceComponent>(),
			};

			for (const ServiceDesc &desc : SERVICES) {
				if (desc.Name == "Lighting") {
					Classes::Register(desc.Name, service, lightingSet);
					continue;
				}
				Classes::Register(desc.Name, service, {});
			}

			// **A `Player` is an instance, not a service.** There are many of
			// them, they come and go, and a script reaches one through
			// `Players` — which is exactly the shape the instance tree already
			// has. Derived from `Instance` rather than from `Service` so the
			// class picker's service exclusion does not hide it, and so a world
			// can hold as many as it has occupants.
			Classes::Register("Player", instance, {});

			Classes::Computed(service, ScopeProperty());

			const ClassId workspace = Classes::Find(core::Name("Workspace"));
			Classes::Computed(workspace, CurrentCameraProperty());

			const ClassId players = Classes::Find(core::Name("Players"));
			Classes::Computed(players, LocalPlayerProperty());
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

	Entity WorkspaceOf(const Store &store) {
		return store.FindFirstRoot("Workspace");
	}

	Entity PlayersOf(const Store &store) {
		return store.FindFirstRoot("Players");
	}

	ecs::ClassId PlayerClass() {
		ServiceClass();
		return Classes::Find(core::Name("Player"));
	}

	Entity AddPlayer(Store &store, std::string_view name, bool local) {
		const Entity players = PlayersOf(store);
		if (players == NULL_ENTITY) {
			// **Refused rather than furnished on the way past.** A caller adding
			// a player to a world with no `Players` has skipped
			// `InstallServices`, and quietly creating the service here would
			// make that omission invisible until something else asked for it.
			return NULL_ENTITY;
		}

		const Entity player = store.CreateInstance(PlayerClass(), name);
		if (player == NULL_ENTITY) {
			return NULL_ENTITY;
		}

		store.SetParent(player, players);

		// **A `PlayerGui`, because a client's interface has nowhere else to
		// live.** `gui::Layout` draws a `ScreenGui` only from `StarterGui` or
		// from a player's `PlayerGui` — Roblox's containment rule — so a player
		// without one is a viewer that can never be shown an interface, and the
		// symptom is a black overlay rather than an error.
		//
		// Made here rather than by whoever adds the player, for
		// `InstallServices`' reason: a container every consumer has to remember
		// to create is one somebody will not, and the failure is silent.
		//
		// **A plain `Instance` rather than a `gui` class.** `scene` may not link
		// `gui` — the refusal runs both ways, and `gui/AGENTS.md` states this
		// side of it — and what the containment test reads is the *name*. So the
		// spelling is what matters, and `examples/tests/Scene.cpp` pins it
		// against `gui::PLAYER_GUI` from the one place both are linked.
		const Entity playerGui =
			store.CreateInstance(Classes::Find(core::Name("Instance")), std::string(PLAYER_GUI_NAME));
		if (playerGui != NULL_ENTITY) {
			store.SetParent(playerGui, player);
		}

		if (local) {
			// One per world, and the last marked wins. A resource rather than a
			// tag because there is one of it — `ecs/AGENTS.md`'s rule — and
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
			// instances — they were saved like everything else — so creating
			// them unconditionally would give an author two Workspaces, one of
			// which holds their scene and one of which is empty.
			Entity parent = NULL_ENTITY;
			if (!desc.Parent.empty()) {
				parent = store.FindFirstRoot(desc.Parent);
				if (parent == NULL_ENTITY) {
					continue;
				}
			}

			Entity existing = parent == NULL_ENTITY ? store.FindFirstRoot(desc.Name)
													: store.FindFirstChild(parent, desc.Name);

			if (existing == NULL_ENTITY) {
				const ClassId klass = Classes::Find(core::Name(desc.Name));
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
			}

			if (desc.Name == "Workspace") {
				workspace = existing;
			}
		}

		// **No camera here, and that is the correction.** A camera belongs to
		// whoever is looking, not to the world: the editor makes one to show its
		// viewport, a client makes one for its player, and several people editing
		// one game make one each. `InstallServices` furnishes a world with what
		// the *game* has, and a viewpoint is not that — putting one here wrote
		// somebody's camera into every game file, which is what
		// `TransientComponent` now exists to prevent.
		//
		// The viewer creates its own and marks it transient. See
		// `studio::Editor::EnsureViewerCamera`.

		return workspace;
	}
}
