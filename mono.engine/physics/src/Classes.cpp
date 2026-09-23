// Storm component authoring properties on ordinary scene instances.

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Storm.hpp>
#include <engine/scene/Part.hpp>

#include <string_view>
#include <type_traits>
#include <utility>

namespace engine::physics {

	namespace {
		using ecs::PropertyDescriptor;
		using ecs::PropertyKind;
		using ecs::PropertyType;

		template <class Component, auto Member> PropertyDescriptor StormProperty(std::string_view name) {
			using Value = std::remove_cvref_t<decltype(std::declval<Component>().*Member)>;

			PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Size = sizeof(Value);
			property.Kind = PropertyKind::Structural;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<Component>()});
			property.Writes = property.Reads;

			if constexpr (std::is_same_v<Value, bool>) {
				property.Type = PropertyType::Bool;
			} else if constexpr (std::is_same_v<Value, float>) {
				property.Type = PropertyType::Float;
			} else {
				static_assert(std::is_same_v<Value, core::CFrame>);
				property.Type = PropertyType::CFrame;
			}

			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const Component *component = store.Get<Component>(instance);
				*static_cast<Value *>(out) = component == nullptr ? Component{}.*Member : component->*Member;
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				Component component;
				if (const Component *existing = store.Get<Component>(instance)) component = *existing;
				component.*Member = *static_cast<const Value *>(value);
				store.Set(instance, component);
				return true;
			};
			return property;
		}

		void RegisterResponseProperties(ecs::ClassId part) {
			ecs::Classes::Computed(part, StormProperty<StormResponse, &StormResponse::ExposedArea>("StormExposedArea"));
			ecs::Classes::Computed(
				part, StormProperty<StormResponse, &StormResponse::DragCoefficient>("StormDragCoefficient")
			);
			ecs::Classes::Computed(part, StormProperty<StormResponse, &StormResponse::ForceScale>("StormForceScale"));
			ecs::Classes::Computed(part, StormProperty<StormResponse, &StormResponse::Enabled>("StormEnabled"));
		}

		void RegisterVegetationProperties(ecs::ClassId part) {
			ecs::Classes::Computed(part, StormProperty<StormVegetation, &StormVegetation::RestFrame>("StormRestFrame"));
			ecs::Classes::Computed(
				part, StormProperty<StormVegetation, &StormVegetation::MaximumBendRadians>("StormMaximumBendRadians")
			);
			ecs::Classes::Computed(
				part, StormProperty<StormVegetation, &StormVegetation::ResponsePerSecond>("StormResponsePerSecond")
			);
			ecs::Classes::Computed(
				part,
				StormProperty<StormVegetation, &StormVegetation::WindSpeedForMaximumBend>("StormWindSpeedForMaximumBend")
			);
			ecs::Classes::Computed(part, StormProperty<StormVegetation, &StormVegetation::Enabled>("StormVegetationEnabled"));
		}

		void RegisterLinkProperties(ecs::ClassId link) {
			ecs::Classes::Computed(link, StormProperty<StormLink, &StormLink::BreakForce>("StormBreakForce"));
			ecs::Classes::Computed(link, StormProperty<StormLink, &StormLink::MaterialStrength>("StormMaterialStrength"));
			ecs::Classes::Computed(link, StormProperty<StormLink, &StormLink::Enabled>("StormLinkEnabled"));
		}
	}

	void RegisterPhysicsClasses() {
		static const bool registered = [] {
			RegisterPhysicsComponents();
			scene::EnsureClassTree();

			const ecs::ClassId part = scene::PartClass();
			RegisterResponseProperties(part);
			RegisterVegetationProperties(part);
			RegisterLinkProperties(ecs::Classes::Find(core::Name("Weld")));
			RegisterLinkProperties(ecs::Classes::Find(core::Name("WeldConstraint")));
			return true;
		}();
		(void)registered;
	}
}
