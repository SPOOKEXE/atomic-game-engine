#include <engine/graph/EngineGraph.hpp>
#include <engine/graph/PipelineCatalogue.hpp>

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace engine::graph {

	const char *Describe(EngineStage stage) {
		switch (stage) {
		case EngineStage::Input:
			return "Input Graph";
		case EngineStage::Ai:
			return "AI Graph";
		case EngineStage::World:
			return "World Graph";
		case EngineStage::Physics:
			return "Physics Graph";
		case EngineStage::Animation:
			return "Animation Graph";
		case EngineStage::Render:
			return "Render Graph";
		case EngineStage::Count:
			return "?";
		}
		return "?";
	}

	const char *Describe(EngineGraphStatus status) {
		switch (status) {
		case EngineGraphStatus::Ok:
			return "ok";
		case EngineGraphStatus::InvalidName:
			return "a node or resource has no stable name";
		case EngineGraphStatus::UnknownResource:
			return "a node names an undeclared resource";
		case EngineGraphStatus::MissingProducer:
			return "a read has no producer";
		case EngineGraphStatus::StageRegression:
			return "a dependency points back to an earlier engine stage";
		case EngineGraphStatus::Cycle:
			return "the engine dependencies form a cycle";
		}
		return "unknown engine graph status";
	}

	bool EngineGraph::AddResource(EngineResourceDesc resource) {
		if (!resource.Name.IsValid() || FindResource(resource.Name) != nullptr) {
			return false;
		}
		DeclaredResources.push_back(std::move(resource));
		return true;
	}

	EngineNodeId EngineGraph::AddNode(EngineNode node) {
		if (!node.Name.IsValid() || node.Stage == EngineStage::Count) {
			return {};
		}
		const auto duplicate = std::find_if(Nodes.begin(), Nodes.end(), [&](const EngineNode &existing) {
			return existing.Name == node.Name;
		});
		if (duplicate != Nodes.end()) {
			return {};
		}
		Nodes.push_back(std::move(node));
		return EngineNodeId{static_cast<uint32_t>(Nodes.size())};
	}

	const EngineNode *EngineGraph::Find(EngineNodeId node) const {
		return node.IsValid() && node.Value <= Nodes.size() ? &Nodes[node.Value - 1] : nullptr;
	}

	const EngineResourceDesc *EngineGraph::FindResource(core::Name name) const {
		const auto found = std::find_if(
			DeclaredResources.begin(), DeclaredResources.end(), [&](const EngineResourceDesc &resource) {
				return resource.Name == name;
			}
		);
		return found == DeclaredResources.end() ? nullptr : &*found;
	}

	namespace {
		void AddEdge(
			std::vector<std::unordered_set<size_t>> &outgoing,
			std::vector<std::unordered_set<size_t>> &incoming,
			size_t before,
			size_t after
		) {
			if (before == after || !outgoing[before].insert(after).second) {
				return;
			}
			incoming[after].insert(before);
		}
	}

	EngineGraphStatus CompileEngineGraph(
		const EngineGraph &graph, EngineSchedule &out, core::Name &offender, EngineExecutionMode mode
	) {
		out.Clear();
		const std::span<const EngineNode> nodes = graph.AllNodes();
		const size_t count = nodes.size();
		std::vector<std::unordered_set<size_t>> outgoing(count);
		std::vector<std::unordered_set<size_t>> incoming(count);
		std::unordered_map<uint32_t, std::vector<size_t>> writers;

		for (size_t index = 0; index < count; index++) {
			for (const core::Name resource : nodes[index].Reads) {
				if (!resource.IsValid()) {
					offender = nodes[index].Name;
					return EngineGraphStatus::InvalidName;
				}
				if (graph.FindResource(resource) == nullptr) {
					offender = resource;
					return EngineGraphStatus::UnknownResource;
				}
			}
			for (const core::Name resource : nodes[index].Writes) {
				if (!resource.IsValid()) {
					offender = nodes[index].Name;
					return EngineGraphStatus::InvalidName;
				}
				if (graph.FindResource(resource) == nullptr) {
					offender = resource;
					return EngineGraphStatus::UnknownResource;
				}
				writers[resource.Id()].push_back(index);
			}
		}

		for (size_t before = 0; before < count; before++) {
			for (size_t after = 0; after < count; after++) {
				if (nodes[before].Stage < nodes[after].Stage) {
					AddEdge(outgoing, incoming, before, after);
				}
			}
		}

		for (const EngineResourceDesc &resource : graph.Resources()) {
			const auto found = writers.find(resource.Name.Id());
			const std::vector<size_t> empty;
			const std::vector<size_t> &resourceWriters = found == writers.end() ? empty : found->second;
			for (size_t index = 1; index < resourceWriters.size(); index++) {
				AddEdge(outgoing, incoming, resourceWriters[index - 1], resourceWriters[index]);
			}
			for (size_t reader = 0; reader < count; reader++) {
				if (std::find(nodes[reader].Reads.begin(), nodes[reader].Reads.end(), resource.Name) ==
					nodes[reader].Reads.end()) {
					continue;
				}
				size_t producer = count;
				for (const size_t writer : resourceWriters) {
					if (writer < reader) {
						producer = writer;
					}
				}
				if (producer == count && resourceWriters.size() == 1) {
					producer = resourceWriters.front();
				}
				if (producer == count) {
					if (resource.External) {
						continue;
					}
					offender = resource.Name;
					return EngineGraphStatus::MissingProducer;
				}
				if (nodes[producer].Stage > nodes[reader].Stage) {
					offender = nodes[reader].Name;
					return EngineGraphStatus::StageRegression;
				}
				AddEdge(outgoing, incoming, producer, reader);
				const auto next =
					std::find_if(resourceWriters.begin(), resourceWriters.end(), [producer](size_t writer) {
						return writer > producer;
					});
				if (next != resourceWriters.end()) {
					AddEdge(outgoing, incoming, reader, *next);
				}
			}
		}

		std::vector<size_t> remaining(count);
		std::vector<uint32_t> waveOf(count, std::numeric_limits<uint32_t>::max());
		for (size_t index = 0; index < count; index++) {
			remaining[index] = incoming[index].size();
		}
		size_t emitted = 0;
		while (emitted < count) {
			std::vector<size_t> ready;
			for (size_t index = 0; index < count; index++) {
				if (remaining[index] == 0) {
					ready.push_back(index);
				}
			}
			if (ready.empty()) {
				for (size_t index = 0; index < count; index++) {
					if (remaining[index] != std::numeric_limits<size_t>::max()) {
						offender = nodes[index].Name;
						break;
					}
				}
				out.Clear();
				return EngineGraphStatus::Cycle;
			}
			if (mode == EngineExecutionMode::Deterministic) {
				ready.resize(1);
			}
			for (const size_t index : ready) {
				remaining[index] = std::numeric_limits<size_t>::max();
			}

			EngineWave wave;
			std::unordered_set<uint8_t> queues;
			const uint32_t waveIndex = static_cast<uint32_t>(out.Waves.size());
			for (const size_t index : ready) {
				wave.Nodes.push_back({
					.Node = EngineNodeId{static_cast<uint32_t>(index + 1)},
					.Queue = nodes[index].Queue,
					.AsyncEligible = nodes[index].AsyncEligible,
					.Profile = nodes[index].Profile,
				});
				queues.insert(static_cast<uint8_t>(nodes[index].Queue));
				waveOf[index] = waveIndex;
			}
			wave.Concurrent =
				mode == EngineExecutionMode::Concurrent && ready.size() > 1 &&
				(queues.size() == 1 || std::any_of(ready.begin(), ready.end(), [&](size_t index) {
					 return nodes[index].AsyncEligible;
				 }));
			out.Waves.push_back(std::move(wave));
			emitted += ready.size();
			for (const size_t index : ready) {
				for (const size_t dependent : outgoing[index]) {
					remaining[dependent]--;
				}
			}
		}

		for (size_t producer = 0; producer < count; producer++) {
			for (const size_t consumer : outgoing[producer]) {
				if (nodes[producer].Queue == nodes[consumer].Queue) {
					continue;
				}
				for (const core::Name resource : nodes[producer].Writes) {
					if (std::find(nodes[consumer].Reads.begin(), nodes[consumer].Reads.end(), resource) ==
						nodes[consumer].Reads.end()) {
						continue;
					}
					out.Barriers.push_back({
						.Resource = resource,
						.From = nodes[producer].Queue,
						.To = nodes[consumer].Queue,
						.ProducerWave = waveOf[producer],
						.ConsumerWave = waveOf[consumer],
					});
				}
			}
		}

		for (const EngineResourceDesc &resource : graph.Resources()) {
			uint32_t first = std::numeric_limits<uint32_t>::max();
			uint32_t last = 0;
			for (size_t index = 0; index < count; index++) {
				const bool touches =
					std::find(nodes[index].Reads.begin(), nodes[index].Reads.end(), resource.Name) !=
						nodes[index].Reads.end() ||
					std::find(nodes[index].Writes.begin(), nodes[index].Writes.end(), resource.Name) !=
						nodes[index].Writes.end();
				if (touches) {
					first = std::min(first, waveOf[index]);
					last = std::max(last, waveOf[index]);
				}
			}
			if (first != std::numeric_limits<uint32_t>::max()) {
				out.Resources.push_back({resource.Name, resource.Lifetime, first, last});
			}
		}
		return EngineGraphStatus::Ok;
	}

	EngineGraphStatus AppendRenderStage(
		const RenderGraph &renderGraph,
		EngineGraph &engineGraph,
		core::Name &offender,
		std::string_view prefix
	) {
		ExecutionSchedule renderSchedule;
		if (CompileSchedule(renderGraph, renderSchedule, offender) != ScheduleStatus::Ok) {
			return EngineGraphStatus::Cycle;
		}
		std::unordered_map<uint32_t, ScheduledNode> scheduled;
		for (const ExecutionWave &wave : renderSchedule.Waves) {
			for (const ScheduledNode &node : wave.Nodes) {
				scheduled[node.Node.Value] = node;
			}
		}
		const auto qualified = [prefix](core::Name name) {
			return core::Name(std::string(prefix) + std::string(name.Text()));
		};
		for (uint32_t value = 1; value <= renderGraph.ResourceCount(); value++) {
			const ResourceDesc *resource = renderGraph.FindResource(ResourceId{value});
			if (resource == nullptr) {
				continue;
			}
			const core::Name name = qualified(resource->Name);
			if (engineGraph.FindResource(name) == nullptr &&
				!engineGraph.AddResource({
					.Name = name,
					.Lifetime = resource->External ? EngineResourceLifetime::Persistent
												   : EngineResourceLifetime::Tick,
					.External = resource->External,
				})) {
				offender = name;
				return EngineGraphStatus::InvalidName;
			}
		}
		for (uint32_t value = 1; value <= renderGraph.Count(); value++) {
			const Node *node = renderGraph.Find(NodeId{value});
			if (node == nullptr || !node->Enabled) {
				continue;
			}
			EngineNode imported;
			imported.Name = qualified(node->Name);
			imported.Stage = EngineStage::Render;
			imported.Queue = scheduled[value].Queue;
			imported.AsyncEligible = scheduled[value].AsyncEligible;
			imported.Profile = node->Parameter(core::Name("profile")) == nullptr ||
							   *node->Parameter(core::Name("profile")) != "false";
			for (const ResourceId resource : node->Reads) {
				const ResourceDesc *desc = renderGraph.FindResource(resource);
				if (desc != nullptr) {
					imported.Reads.push_back(qualified(desc->Name));
				}
			}
			for (const ResourceId resource : node->Writes) {
				const ResourceDesc *desc = renderGraph.FindResource(resource);
				if (desc != nullptr) {
					imported.Writes.push_back(qualified(desc->Name));
				}
			}
			if (!engineGraph.AddNode(std::move(imported)).IsValid()) {
				offender = qualified(node->Name);
				return EngineGraphStatus::InvalidName;
			}
		}
		return EngineGraphStatus::Ok;
	}

	bool
	ExecuteEngineGraph(const EngineGraph &graph, const EngineSchedule &schedule, EngineNodeRunner &runner) {
		for (uint32_t wave = 0; wave < schedule.Waves.size(); wave++) {
			std::vector<EngineBarrier> barriers;
			for (const EngineBarrier &barrier : schedule.Barriers) {
				if (barrier.ConsumerWave == wave) {
					barriers.push_back(barrier);
				}
			}
			const EngineWave &scheduled = schedule.Waves[wave];
			if (!runner.BeginWave(wave, scheduled.Concurrent, barriers)) {
				return false;
			}
			for (const EngineScheduledNode &entry : scheduled.Nodes) {
				const EngineNode *node = graph.Find(entry.Node);
				if (node == nullptr || !runner.Run({*node, wave, barriers})) {
					return false;
				}
			}
			if (!runner.EndWave(wave)) {
				return false;
			}
		}
		return true;
	}
}
