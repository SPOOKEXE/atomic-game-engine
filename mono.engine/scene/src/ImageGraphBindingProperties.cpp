#include "ImageGraphBindingProperties.hpp"

#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ImageGraphBinding.hpp>

#include <bit>
#include <limits>
#include <type_traits>

namespace engine::scene {
	namespace {
		template <auto Member, class Wire, bool SeedBits = false>
		ecs::PropertyDescriptor Field(std::string_view name, ecs::PropertyType type) {
			ecs::PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = type;
			property.Size = sizeof(Wire);
			property.Kind = ecs::PropertyKind::Computed;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<ImageGraphBinding>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity entity, void *out) {
				const auto *existing = store.Get<ImageGraphBinding>(entity);
				const ImageGraphBinding binding = existing ? *existing : ImageGraphBinding{};
				if constexpr (std::is_same_v<Wire, int64_t> && !SeedBits)
					if (binding.*Member > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
						return false;
				// Signed wire carries all 64 seed bits; a negative spelling is not a negative tick.
				if constexpr (SeedBits)
					*static_cast<Wire *>(out) = std::bit_cast<int64_t>(binding.*Member);
				else
					*static_cast<Wire *>(out) = static_cast<Wire>(binding.*Member);
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity entity, const void *input) {
				const Wire value = *static_cast<const Wire *>(input);
				if constexpr (std::is_same_v<Wire, core::Name>) {
					if (value.IsValid() && (value.Text().size() > IMAGE_GRAPH_BINDING_MAXIMUM_NAME_BYTES ||
											value.Text().find('\0') != std::string_view::npos))
						return false;
				} else if (!SeedBits && value < 0)
					return false;
				const auto *existing = store.Get<ImageGraphBinding>(entity);
				ImageGraphBinding binding = existing ? *existing : ImageGraphBinding{};
				binding.*Member = value;
				store.Set(entity, binding);
				return true;
			};
			return property;
		}
		template <bool TickPolicy> ecs::PropertyDescriptor Choice(std::string_view name) {
			ecs::PropertyDescriptor property;
			property.Name = core::Name(name);
			property.Type = ecs::PropertyType::Name;
			property.Size = sizeof(core::Name);
			property.Kind = ecs::PropertyKind::Computed;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<ImageGraphBinding>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity entity, void *out) {
				const auto *binding = store.Get<ImageGraphBinding>(entity);
				if constexpr (TickPolicy)
					*static_cast<core::Name *>(out) = core::Name(
						binding && binding->TickPolicy == ImageGraphTickPolicy::World ? "world" : "fixed"
					);
				else
					*static_cast<core::Name *>(out) = core::Name(
						binding && binding->ColorSpace == ImageGraphColorSpace::Linear ? "linear" : "display"
					);
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity entity, const void *input) {
				const auto text = static_cast<const core::Name *>(input)->Text();
				if constexpr (TickPolicy) {
					if (text != "world" && text != "fixed") return false;
				} else {
					if (text != "linear" && text != "display") return false;
				}
				const auto *existing = store.Get<ImageGraphBinding>(entity);
				ImageGraphBinding binding = existing ? *existing : ImageGraphBinding{};
				if constexpr (TickPolicy)
					binding.TickPolicy =
						text == "world" ? ImageGraphTickPolicy::World : ImageGraphTickPolicy::Fixed;
				else
					binding.ColorSpace =
						text == "linear" ? ImageGraphColorSpace::Linear : ImageGraphColorSpace::Display;
				store.Set(entity, binding);
				return true;
			};
			return property;
		}
	}

	void RegisterImageGraphBindingProperties(ecs::ClassId instance) {
		// Optional row keeps ordinary instances out of graph evaluation. Hosts admit only complete selectors.
		ecs::Classes::Computed(
			instance, Field<&ImageGraphBinding::Graph, core::Name>("ImageGraph", ecs::PropertyType::Name)
		);
		ecs::Classes::Computed(
			instance,
			Field<&ImageGraphBinding::Output, core::Name>("ImageGraphOutput", ecs::PropertyType::Name)
		);
		ecs::Classes::Computed(
			instance,
			Field<&ImageGraphBinding::Texture, core::Name>("ImageGraphTexture", ecs::PropertyType::Name)
		);
		ecs::Classes::Computed(
			instance,
			Field<&ImageGraphBinding::Seed, int64_t, true>("ImageGraphSeed", ecs::PropertyType::Int64)
		);
		ecs::Classes::Computed(
			instance,
			Field<&ImageGraphBinding::FixedTick, int64_t>("ImageGraphFixedTick", ecs::PropertyType::Int64)
		);
		ecs::Classes::Computed(instance, Choice<true>("ImageGraphTickPolicy"));
		ecs::Classes::Computed(instance, Choice<false>("ImageGraphColorSpace"));
	}
}
