#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>

#include <array>
#include <string_view>

namespace engine::scene {

	namespace {
		using ecs::ClassId;
		using ecs::Classes;
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

		constexpr std::array<ServiceDesc, 9> SERVICES{{
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
		}};

		ClassId RegisterServiceTree() {
			// The root of everything, and the components these classes are sets
			// of, both through `PartClass`. A service derives from `Instance`,
			// so registering that again here would be a second root — and
			// `Classes::Register` handing back the existing id for a repeated
			// name would have hidden it rather than refused it.
			PartClass();

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

			Classes::Computed(service, ScopeProperty());
			Classes::Property<&ServiceComponent::Fixture>(service, "Fixture");

			const ClassId lighting = Classes::Find(core::Name("Lighting"));
			Classes::Property<&LightingServiceComponent::Ambient>(lighting, "Ambient");
			Classes::Property<&LightingServiceComponent::OutdoorAmbient>(lighting, "OutdoorAmbient");
			Classes::Property<&LightingServiceComponent::FogColor>(lighting, "FogColor");
			Classes::Property<&LightingServiceComponent::Brightness>(lighting, "Brightness");
			Classes::Property<&LightingServiceComponent::ClockTime>(lighting, "ClockTime");
			Classes::Property<&LightingServiceComponent::FogStart>(lighting, "FogStart");
			Classes::Property<&LightingServiceComponent::FogEnd>(lighting, "FogEnd");
			Classes::Property<&LightingServiceComponent::GeographicLatitude>(
				lighting, "GeographicLatitude"
			);

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
