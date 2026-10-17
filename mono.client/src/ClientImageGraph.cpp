#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Reference.hpp>
#include <engine/scene/ImageGraph.hpp>

#include <algorithm>
#include <client/Client.hpp>

namespace client {
	void Client::RefreshImageGraphs(ContentSession &content, std::span<const engine::world::WorldId> worlds) {
		ENGINE_PROFILE_CAT("content.imagegraphs", engine::core::ProfileCategory::Assets);
		using namespace engine;
		struct Demand {
			core::Name Owner;
			core::Name Name;
			core::Name Asset;
			std::vector<core::Name> Outputs;
			std::vector<imagegraph::InputOverride> Inputs;
			uint64_t Revision = 0;
		};
		std::vector<Demand> demands;
		std::erase_if(content.GraphReferences, [&](const auto &entry) {
			return std::none_of(worlds.begin(), worlds.end(), [&](const auto world) {
				return Universe_->NameOf(world).Id() == entry.first;
			});
		});
		for (auto &[key, applied] : content.AppliedGraphs)
			applied.Seen = false;
		for (const auto world : worlds) {
			const auto owner = Universe_->NameOf(world);
			const auto references = content.GraphReferences.find(owner.Id());
			if (references != content.GraphReferences.end()) {
				for (const auto uri : references->second) {
					imagegraph::Reference reference;
					if (!imagegraph::ParseReference(uri.Text(), reference) ||
						reference.Kind != imagegraph::ReferenceKind::Asset)
						continue;
					const core::Name graphName("imagegraph://" + reference.Name);
					auto found = std::find_if(demands.begin(), demands.end(), [&](const Demand &demand) {
						return demand.Owner == owner && demand.Name == graphName;
					});
					if (found == demands.end()) {
						demands.push_back({owner, graphName, core::Name(reference.Name), {uri}, {}, 0});
					} else
						found->Outputs.push_back(uri);
				}
			}
			// Copy controls while entering the world. GPU work runs after leaving it.
			Universe_->Enter(world, [&](ecs::Store &store) {
				store.Observe<scene::ImageGraph>();
				const uint64_t inputVersion = store.ComponentChangeVersion<scene::ImageGraph>();
				store.Each<const scene::ImageGraph>([&](ecs::Entity, const scene::ImageGraph &graph) {
					if (!imagegraph::IsReferenceToken(graph.InstanceKey.Text()) ||
						!imagegraph::IsRuntimeAsset(graph.Graph.Text()))
						return;
					Demand demand{
						owner,
						core::Name("imagegraph-instance://" + std::string(graph.InstanceKey.Text())),
						graph.Graph,
						{},
						{},
						inputVersion
					};
					if (references != content.GraphReferences.end()) {
						for (const auto uri : references->second) {
							imagegraph::Reference reference;
							if (imagegraph::ParseReference(uri.Text(), reference) &&
								reference.Kind == imagegraph::ReferenceKind::Instance &&
								reference.Name == graph.InstanceKey.Text())
								demand.Outputs.push_back(uri);
						}
					}
					if (demand.Outputs.empty()) return;
					for (const auto &input : graph.Inputs) {
						imagegraph::InputValue value;
						switch (input.Kind) {
						case scene::ImageGraphInputKind::Number:
							value = input.Number;
							break;
						case scene::ImageGraphInputKind::Boolean:
							value = input.Boolean;
							break;
						case scene::ImageGraphInputKind::Colour:
							value = input.Colour;
							break;
						case scene::ImageGraphInputKind::String:
							value = input.String;
							break;
						}
						demand.Inputs.push_back({std::string(input.Name.Text()), std::move(value)});
					}
					demands.push_back(std::move(demand));
				});
			});
		}
		for (Demand &demand : demands) {
			std::sort(demand.Outputs.begin(), demand.Outputs.end(), [](auto a, auto b) {
				return a.Id() < b.Id();
			});
			demand.Outputs.erase(
				std::unique(demand.Outputs.begin(), demand.Outputs.end()), demand.Outputs.end()
			);
			RequestAsset(content, demand.Asset, demand.Owner);
			const uint64_t key = (uint64_t(demand.Owner.Id()) << 32) | demand.Name.Id();
			auto &applied = content.AppliedGraphs[key];
			applied.Seen = true;
			applied.Owner = demand.Owner;
			applied.Name = demand.Name;
			const auto *record = content.GraphContent.Find(demand.Asset.Text());
			if (!record || std::count_if(demands.begin(), demands.end(), [&](const Demand &other) {
							   return other.Owner == demand.Owner && other.Name == demand.Name;
						   }) != 1)
				continue;

			imagegraph::Diagnostic diagnostic;
			if (applied.Asset != demand.Asset || applied.ContentRevision != record->Revision ||
				applied.InputRevision != demand.Revision || applied.Outputs != demand.Outputs) {
				imagegraph::Document resolved;
				std::vector<std::string> sources;
				std::vector<std::string_view> selectedOutputs;
				for (const auto uri : demand.Outputs) {
					const auto text = uri.Text();
					selectedOutputs.push_back(text.substr(text.find('#') + 1));
				}
				if (!imagegraph::ResolveInputs(record->Authored, demand.Inputs, resolved, diagnostic) ||
					!imagegraph::RuntimeSources(resolved, sources, diagnostic, selectedOutputs))
					continue;
				for (const auto &source : sources) {
					const core::Name texture(source);
					RequestAsset(content, texture, demand.Owner);
				}
				// Each output admits its own sources; an unavailable cone must not stall another.
				std::vector<render::ImageGraphSourceBinding> bindings;
				for (const auto &node : resolved.Nodes) {
					const auto *source = std::get_if<imagegraph::Source>(&node.Value);
					if (!source || std::any_of(bindings.begin(), bindings.end(), [&](const auto &binding) {
							return binding.Path == source->Path &&
								   binding.Interpretation == source->Interpretation;
						}))
						continue;
					bindings.push_back(
						{source->Path, core::Name(source->Path), demand.Owner, source->Interpretation}
					);
				}
				if (!Renderer.SetImageGraph(demand.Owner, demand.Name, resolved, bindings, diagnostic))
					continue;
				applied.Asset = demand.Asset;
				applied.ContentRevision = record->Revision;
				applied.InputRevision = demand.Revision;
				applied.Outputs = demand.Outputs;
			}
			for (const auto output : demand.Outputs) {
				imagegraph::Reference reference;
				if (!imagegraph::ParseReference(output.Text(), reference)) continue;
				if (Renderer.EvaluateImageGraph(
						demand.Owner, demand.Name, reference.Output, output, diagnostic
					) == render::ImageGraphEvaluation::Updated)
					VisualResourcesChanged = true;
			}
		}
		std::erase_if(content.AppliedGraphs, [&](const auto &entry) {
			const auto &applied = entry.second;
			if (applied.Seen) return false;
			VisualResourcesChanged =
				Renderer.DropImageGraph(applied.Owner, applied.Name) || VisualResourcesChanged;
			return true;
		});
	}
}
