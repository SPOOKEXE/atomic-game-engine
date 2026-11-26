#include <engine/ecs/Classes.hpp>
#include <engine/ecs/EnumTable.hpp>
#include <engine/ecs/Property.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/content/MaterialLayout.hpp>
#include <engine/scene/content/RenderPasses.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace engine::scene {
	namespace {
		// `RenderPass.Parameters`, encoded as `MeshPart.MaterialParameters` is.
		ecs::PropertyDescriptor PassParametersProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("Parameters");
			property.Type = ecs::PropertyType::String;
			property.Size = sizeof(std::string);
			property.Kind = ecs::PropertyKind::Computed;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<RenderPass>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const auto *pass = store.Get<RenderPass>(instance);
				if (pass == nullptr) return false;
				SurfaceAppearance values;
				std::copy(
					std::begin(pass->Parameters),
					std::end(pass->Parameters),
					std::begin(values.MaterialParameters)
				);
				values.MaterialParameterMask = pass->ParameterMask;
				*static_cast<std::string *>(out) = FormatMaterialParameters(values);
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				auto *pass = store.GetMutable<RenderPass>(instance);
				SurfaceAppearance values;
				if (pass == nullptr ||
					!ParseMaterialParameters(*static_cast<const std::string *>(value), values))
					return false;
				std::copy(
					std::begin(values.MaterialParameters),
					std::end(values.MaterialParameters),
					std::begin(pass->Parameters)
				);
				pass->ParameterMask = values.MaterialParameterMask;
				return true;
			};
			return property;
		}

		constexpr std::array<std::string_view, 2> STAGES{"Lighting", "Transparent"};

		// `RenderPass.Stage`, the `PassStage` enum by name.
		ecs::PropertyDescriptor PassStageProperty() {
			ecs::PropertyDescriptor property;
			property.Name = core::Name("Stage");
			property.Type = ecs::PropertyType::Enum;
			property.EnumName = core::Name("PassStage");
			property.Size = sizeof(core::Name);
			property.Kind = ecs::PropertyKind::Computed;
			property.Reads = &ecs::ComponentSet::Intern({ecs::Components::Of<RenderPass>()});
			property.Writes = property.Reads;
			property.Get = [](const ecs::Store &store, ecs::Entity instance, void *out) -> bool {
				const auto *pass = store.Get<RenderPass>(instance);
				if (pass == nullptr) return false;
				*static_cast<core::Name *>(out) = core::Name(STAGES[static_cast<size_t>(pass->Stage)]);
				return true;
			};
			property.Set = [](ecs::Store &store, ecs::Entity instance, const void *value) -> bool {
				auto *pass = store.GetMutable<RenderPass>(instance);
				const core::Name named = *static_cast<const core::Name *>(value);
				for (size_t index = 0; pass != nullptr && index < STAGES.size(); ++index) {
					if (named != core::Name(STAGES[index])) continue;
					pass->Stage = static_cast<PassStage>(index);
					return true;
				}
				return false;
			};
			return property;
		}

		ecs::ClassId RegisterRenderPassClass() {
			EnsureClassTree();
			const ecs::ClassId instance = ecs::Classes::Find(core::Name("Instance"));
			const std::array held{ecs::Components::Of<RenderPass>()};
			const ecs::ClassId pass = ecs::Classes::Register("RenderPass", instance, held);
			ecs::Classes::Property<&RenderPass::Shader>(pass, "Shader");
			ecs::Classes::Property<&RenderPass::Demand>(pass, "Demand");
			ecs::Classes::Property<&RenderPass::Output>(pass, "Output");
			ecs::EnumTable::Register("PassStage", STAGES);
			ecs::Classes::Computed(pass, PassStageProperty());
			ecs::Classes::ClampedProperty<&RenderPass::Scale, 0.125f, 1.0f>(pass, "Scale");
			ecs::Classes::Property<&RenderPass::Order>(pass, "Order");
			ecs::Classes::Property<&RenderPass::Composite>(pass, "Composite");
			ecs::Classes::Property<&RenderPass::Enabled>(pass, "Enabled");
			ecs::Classes::Computed(pass, PassParametersProperty());
			for (const std::string_view property :
				 {"Shader", "Demand", "Output", "Stage", "Scale", "Order", "Composite", "Enabled", "Parameters"})
				(void)ecs::Classes::SetPropertiesTag(pass, property, core::Name("Rendering"));
			return pass;
		}
	}

	ecs::ClassId RenderPassClass() {
		static const ecs::ClassId pass = RegisterRenderPassClass();
		return pass;
	}

	size_t ResolveRenderPasses(
		const ecs::Store &store, ecs::Entity parent, std::array<ResolvedPass, MAX_RENDER_PASSES> &out
	) {
		struct Row {
			uint64_t Entity;
			RenderPass Pass;
		};
		std::vector<Row> rows;
		store.EachChild(parent, [&](ecs::Entity entity) {
			const auto *pass = store.Get<RenderPass>(entity);
			if (pass != nullptr && pass->Enabled && pass->Shader.IsValid())
				rows.push_back({entity.Id, *pass});
		});
		std::sort(rows.begin(), rows.end(), [](const Row &left, const Row &right) {
			return left.Pass.Order != right.Pass.Order ? left.Pass.Order < right.Pass.Order
													   : left.Entity < right.Entity;
		});
		const size_t count = std::min(rows.size(), out.size());
		for (size_t index = 0; index < count; ++index) {
			const RenderPass &pass = rows[index].Pass;
			out[index] = {
				.Shader = pass.Shader,
				.Demand = pass.Demand,
				.Output = pass.Output,
				.Stage = pass.Stage,
				.Scale = pass.Scale, .Composite = pass.Composite
			};
			std::copy(std::begin(pass.Parameters), std::end(pass.Parameters), out[index].Parameters.begin());
			out[index].ParameterMask = pass.ParameterMask;
		}
		return count;
	}

	size_t DemandedPassShaders(ecs::Store &store, std::vector<core::Name> &out) {
		out.clear();
		store.Each<const RenderPass>([&](ecs::Entity, const RenderPass &pass) {
			if (pass.Enabled && pass.Shader.IsValid()) out.push_back(pass.Shader);
		});
		std::sort(out.begin(), out.end(), [](const core::Name &left, const core::Name &right) {
			return left.Id() < right.Id();
		});
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out.size();
	}
}
