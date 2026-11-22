#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/imagegraph/Content.hpp>
#include <engine/imagegraph/Reference.hpp>
#include <engine/render/compute/ImageGraphMeshes.hpp>
#include <engine/scene/editable/ImageGraph.hpp>

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
			// World that owns the instance, for mesh attribute lowering.
			world::WorldId World{};
			// Script-authored graph JSON that replaces Asset when present.
			std::string Document{};
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
						demands.push_back(
							{owner, graphName, core::Name(reference.Name), {uri}, {}, 0, world}
						);
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
						(!imagegraph::IsRuntimeAsset(graph.Graph.Text()) && graph.Document.empty()))
						return;
					Demand demand{
						owner,
						core::Name("imagegraph-instance://" + std::string(graph.InstanceKey.Text())),
						graph.Graph,
						{},
						{},
						inputVersion,
						world,
						graph.Document
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
			const uint64_t key = (uint64_t(demand.Owner.Id()) << 32) | demand.Name.Id();
			auto &applied = content.AppliedGraphs[key];
			applied.Seen = true;
			applied.Owner = demand.Owner;
			applied.Name = demand.Name;
			if (std::count_if(demands.begin(), demands.end(), [&](const Demand &other) {
					return other.Owner == demand.Owner && other.Name == demand.Name;
				}) != 1)
				continue;
			imagegraph::Diagnostic diagnostic;
			const imagegraph::Document *authored = nullptr;
			uint64_t contentRevision = 0;
			if (!demand.Document.empty()) {
				// A script-authored document is admitted like signed content and parsed
				// once per text; it needs no asset request.
				const uint64_t hash = std::hash<std::string>{}(demand.Document) | 1;
				if (applied.DocumentHash != hash) {
					applied.DocumentHash = hash;
					applied.Runtime.reset();
					imagegraph::Document parsed;
					if (imagegraph::AdmitRuntimeDocument(demand.Document, parsed, diagnostic))
						applied.Runtime = std::move(parsed);
				}
				if (!applied.Runtime) continue;
				authored = &*applied.Runtime;
				contentRevision = hash;
			} else {
				RequestAsset(content, demand.Asset, demand.Owner);
				const auto *record = content.GraphContent.Find(demand.Asset.Text());
				if (!record) continue;
				applied.DocumentHash = 0;
				applied.Runtime.reset();
				authored = &record->Authored;
				contentRevision = record->Revision;
			}
			const bool edited = applied.Asset != demand.Asset || applied.ContentRevision != contentRevision ||
								applied.InputRevision != demand.Revision || applied.Outputs != demand.Outputs;
			// Mesh revisions are not graph revisions, so mesh graphs re-lower every scan.
			const bool lowers = render::HasMeshAttributes(*authored);
			if (edited || lowers) {
				imagegraph::Document resolved;
				std::vector<std::string> sources;
				std::vector<std::string_view> selectedOutputs;
				for (const auto uri : demand.Outputs) {
					const auto text = uri.Text();
					selectedOutputs.push_back(text.substr(text.find('#') + 1));
				}
				if (!imagegraph::ResolveInputs(*authored, demand.Inputs, resolved, diagnostic) ||
					!imagegraph::RuntimeSources(resolved, sources, diagnostic, selectedOutputs))
					continue;
				for (const auto &source : sources) {
					const core::Name texture(source);
					RequestAsset(content, texture, demand.Owner);
				}
				uint64_t lowered = 0;
				if (lowers) {
					bool accepted = false;
					Universe_->Enter(demand.World, [&](ecs::Store &store) {
						accepted =
							ImageGraphMeshes.Lower(store, Renderer, demand.Owner, key, resolved, diagnostic);
					});
					if (!accepted) continue;
					for (const auto &node : resolved.Nodes)
						if (const auto *source = std::get_if<imagegraph::Source>(&node.Value))
							lowered = (lowered ^ std::hash<std::string>{}(source->Path)) * 1099511628211ull;
				}
				if (edited || lowered != applied.Lowered) {
					// Each output admits its own sources; an unavailable cone must not stall another.
					std::vector<render::ImageGraphSourceBinding> bindings;
					for (const auto &node : resolved.Nodes) {
						const auto *source = std::get_if<imagegraph::Source>(&node.Value);
						if (!source ||
							std::any_of(bindings.begin(), bindings.end(), [&](const auto &binding) {
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
					applied.ContentRevision = contentRevision;
					applied.InputRevision = demand.Revision;
					applied.Outputs = demand.Outputs;
					applied.Lowered = lowered;
				}
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
			ImageGraphMeshes.Forget(Renderer, entry.first);
			VisualResourcesChanged =
				Renderer.DropImageGraph(applied.Owner, applied.Name) || VisualResourcesChanged;
			return true;
		});
	}
}
