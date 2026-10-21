#include <engine/graph/RenderStage.hpp>

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace engine::graph {
	namespace {
		constexpr uint8_t UNASSIGNED = 255;
		using NodeSlots = std::array<uint8_t, RenderGraph::MAXIMUM_NODES + 1>;

		EngineGraphStatus Project(
			const RenderGraph &graph,
			const CompiledGraph &compiled,
			const ExecutionSchedule *schedule,
			RenderStageProjection &out,
			core::Name &offender,
			std::string_view prefix
		) {
			offender = {};
			if (graph.Count() > EngineGraph::MAXIMUM_NODES ||
				graph.ResourceCount() > EngineGraph::MAXIMUM_RESOURCES ||
				(schedule != nullptr && schedule->Waves.size() > EngineGraph::MAXIMUM_NODES)) {
				offender = {};
				return EngineGraphStatus::LimitExceeded;
			}
			NodeSlots blockOf;
			blockOf.fill(UNASSIGNED);
			const std::array<const std::vector<NodeId> *, 3> lists{
				&compiled.Shared, &compiled.PerView, &compiled.Final
			};
			for (uint8_t block = 0; block < lists.size(); block++) {
				if (lists[block]->size() > EngineGraph::MAXIMUM_NODES)
					return EngineGraphStatus::LimitExceeded;
				for (const auto id : *lists[block]) {
					const Node *node = graph.Find(id);
					if (node == nullptr || !node->Enabled || blockOf[id.Value] != UNASSIGNED ||
						(node->Scope != NodeScope::World && node->Scope != NodeScope::Frame &&
						 node->Scope != NodeScope::View) ||
						(node->Scope == NodeScope::View) != (block == 1) ||
						(schedule != nullptr && node->Scope == NodeScope::World && block != 0)) {
						offender = node ? node->Name : core::Name{};
						return EngineGraphStatus::InvalidDependency;
					}
					blockOf[id.Value] = block;
				}
			}
			size_t identityBytes = 0;
			const auto admitIdentity = [&](core::Name name) {
				const size_t remaining = EngineGraph::MAXIMUM_IMPORT_IDENTITY_BYTES - identityBytes;
				if (prefix.size() > remaining || name.Text().size() > remaining - prefix.size()) return false;
				identityBytes += prefix.size() + name.Text().size();
				return true;
			};
			for (uint32_t value = 1; value <= graph.ResourceCount(); value++) {
				const auto *resource = graph.FindResourceDesc(ResourceId{value});
				if (resource == nullptr || !resource->Name.IsValid()) return EngineGraphStatus::InvalidName;
				if (!admitIdentity(resource->Name)) return EngineGraphStatus::LimitExceeded;
			}
			std::array<const ScheduledNode *, RenderGraph::MAXIMUM_NODES + 1> scheduled{};
			if (schedule != nullptr) {
				for (const auto &wave : schedule->Waves) {
					if (wave.Nodes.size() > EngineGraph::MAXIMUM_NODES)
						return EngineGraphStatus::LimitExceeded;
					for (const auto &entry : wave.Nodes) {
						const Node *node = graph.Find(entry.Node);
						if (node == nullptr || !node->Enabled || scheduled[entry.Node.Value] != nullptr ||
							(entry.Queue != ExecutionQueue::Cpu && entry.Queue != ExecutionQueue::Graphics &&
							 entry.Queue != ExecutionQueue::Compute &&
							 entry.Queue != ExecutionQueue::Transfer)) {
							offender = node ? node->Name : core::Name{};
							return EngineGraphStatus::InvalidDependency;
						}
						scheduled[entry.Node.Value] = &entry;
					}
				}
			}
			bool seenView = false;
			for (uint32_t value = 1; value <= graph.Count(); value++) {
				const Node *node = graph.Find(NodeId{value});
				if (node == nullptr || !node->Enabled) continue;
				if (!node->Name.IsValid()) {
					offender = node->Name;
					return EngineGraphStatus::InvalidName;
				}
				for (uint32_t prior = 1; prior < value; prior++) {
					const auto *existing = graph.Find(NodeId{prior});
					if (existing->Enabled && existing->Name == node->Name) {
						offender = node->Name;
						return EngineGraphStatus::InvalidName;
					}
				}
				const uint8_t expectedBlock =
					node->Scope == NodeScope::View
						? 1
						: (schedule != nullptr && node->Scope == NodeScope::World ? 0 : (seenView ? 2 : 0));
				seenView = seenView || node->Scope == NodeScope::View;
				if (blockOf[value] != expectedBlock || (schedule != nullptr && scheduled[value] == nullptr)) {
					offender = node->Name;
					return EngineGraphStatus::InvalidDependency;
				}
				if (node->Reads.size() > EngineGraph::MAXIMUM_RESOURCE_USES ||
					node->Writes.size() > EngineGraph::MAXIMUM_RESOURCE_USES ||
					node->ReadPorts.size() > EngineGraph::MAXIMUM_RESOURCE_USES ||
					node->WritePorts.size() > EngineGraph::MAXIMUM_RESOURCE_USES ||
					!admitIdentity(node->Name)) {
					offender = node->Name;
					return EngineGraphStatus::LimitExceeded;
				}
			}

			RenderStageProjection candidate;
			const auto qualified = [prefix](core::Name name) {
				return core::Name(std::string(prefix) + std::string(name.Text()));
			};
			std::array<core::Name, EngineGraph::MAXIMUM_RESOURCES + 1> resourceNames{};
			for (uint32_t value = 1; value <= graph.ResourceCount(); value++) {
				const auto &resource = *graph.FindResourceDesc(ResourceId{value});
				resourceNames[value] = qualified(resource.Name);
				if (!candidate.Graph.AddResource(
						{resourceNames[value],
						 resource.External || resource.Lifetime != ResourceLifetime::Transient
							 ? EngineResourceLifetime::Persistent
							 : EngineResourceLifetime::Tick,
						 resource.External}
					))
					return EngineGraphStatus::AllocationFailed;
			}
			std::array<EngineNodeId, RenderGraph::MAXIMUM_NODES + 1> dense{};
			for (uint32_t value = 1; value <= graph.Count(); value++) {
				const Node *node = graph.Find(NodeId{value});
				if (node == nullptr || !node->Enabled) continue;
				EngineNode imported;
				imported.Name = qualified(node->Name);
				imported.Stage = EngineStage::Render;
				if (schedule != nullptr) {
					imported.Queue = scheduled[value]->Queue;
					imported.AsyncEligible = scheduled[value]->AsyncEligible;
				}
				const auto *profile = node->Parameter(core::Name("profile"));
				imported.Profile = profile == nullptr || *profile != "false";
				for (const auto *side : {&node->Reads, &node->Writes}) {
					auto &names = side == &node->Reads ? imported.Reads : imported.Writes;
					for (const auto resource : *side) {
						const auto *description = graph.FindResourceDesc(resource);
						if (description == nullptr) {
							offender = node->Name;
							return EngineGraphStatus::UnknownResource;
						}
						names.push_back(resourceNames[resource.Value]);
					}
				}
				dense[value] = candidate.Graph.AddNode(std::move(imported));
				if (!dense[value].IsValid()) {
					offender = node->Name;
					return EngineGraphStatus::AllocationFailed;
				}
				candidate.RenderNodes.push_back(NodeId{value});
			}
			const auto entry = [&](NodeId id) {
				const auto &node = *candidate.Graph.Find(dense[id.Value]);
				return EngineScheduledNode{dense[id.Value], node.Queue, node.AsyncEligible, node.Profile};
			};
			if (schedule == nullptr) {
				for (size_t block = 0; block < lists.size(); block++) {
					for (const auto id : *lists[block])
						candidate.Blocks[block].Waves.push_back({{entry(id)}, false});
				}
			} else {
				for (const auto &wave : schedule->Waves) {
					std::array<EngineWave, 3> filtered;
					for (const auto &node : wave.Nodes)
						filtered[blockOf[node.Node.Value]].Nodes.push_back(entry(node.Node));
					for (size_t block = 0; block < filtered.size(); block++) {
						if (filtered[block].Nodes.empty()) continue;
						filtered[block].Concurrent = wave.Concurrent;
						candidate.Blocks[block].Waves.push_back(std::move(filtered[block]));
					}
				}
			}
			out = std::move(candidate);
			offender = {};
			return EngineGraphStatus::Ok;
		}

		class ScopeRunner final : public EngineNodeRunner {
		  public:
			ScopeRunner(
				const RenderGraph &graph,
				const RenderStageProjection &projection,
				NodeRunner &runner,
				size_t view,
				size_t world,
				bool frame
			)
				: Graph(graph), Projection(projection), Runner(runner), View(view), World(world),
				  Frame(frame) {}
			bool BeginWave(uint32_t, bool, std::span<const EngineBarrier>) override {
				return true;
			}
			bool EndWave(uint32_t) override {
				return true;
			}
			bool Run(const EngineRunContext &engine) override {
				if (!engine.Node.IsValid() || engine.Node.Value > Projection.RenderNodes.size()) return false;
				const auto id = Projection.RenderNodes[engine.Node.Value - 1];
				const Node *node = Graph.Find(id);
				if (node == nullptr) return false;
				if (node->Scope == NodeScope::Frame && !Frame) return true;
				return Runner.Run(
					{.Node = id,
					 .Name = node->Name,
					 .Kind = node->Kind,
					 .View = View,
					 .World = node->Scope == NodeScope::Frame ? RunContext::WHOLE_FRAME : World,
					 .Reads = node->Reads,
					 .Writes = node->Writes}
				);
			}

		  private:
			const RenderGraph &Graph;
			const RenderStageProjection &Projection;
			NodeRunner &Runner;
			size_t View;
			size_t World;
			bool Frame;
		};
	}

	EngineGraphStatus ProjectRenderStage(
		const RenderGraph &graph,
		const CompiledGraph &compiled,
		RenderStageProjection &out,
		core::Name &offender,
		std::string_view prefix
	) try {
		return Project(graph, compiled, nullptr, out, offender, prefix);
	} catch (const std::bad_alloc &) {
		offender = {};
		return EngineGraphStatus::AllocationFailed;
	} catch (const std::length_error &) {
		offender = {};
		return EngineGraphStatus::AllocationFailed;
	}

	EngineGraphStatus ProjectRenderStage(
		const RenderGraph &graph,
		const CompiledGraph &compiled,
		const ExecutionSchedule &schedule,
		RenderStageProjection &out,
		core::Name &offender,
		std::string_view prefix
	) try {
		return Project(graph, compiled, &schedule, out, offender, prefix);
	} catch (const std::bad_alloc &) {
		offender = {};
		return EngineGraphStatus::AllocationFailed;
	} catch (const std::length_error &) {
		offender = {};
		return EngineGraphStatus::AllocationFailed;
	}

	bool ExecuteRenderView(
		const RenderGraph &graph,
		const RenderStageProjection &projection,
		NodeRunner &runner,
		size_t view,
		size_t world,
		bool shared,
		bool frame
	) {
		if (frame && !shared) return false;
		ScopeRunner setup(graph, projection, runner, RunContext::WHOLE_FRAME, world, frame);
		if (shared && !ExecuteEngineGraph(projection.Graph, projection.Blocks[0], setup)) return false;
		if (view == RunContext::WHOLE_FRAME) return true;
		ScopeRunner perView(graph, projection, runner, view, world, true);
		return ExecuteEngineGraph(projection.Graph, projection.Blocks[1], perView);
	}

	bool ExecuteRenderFinal(
		const RenderGraph &graph, const RenderStageProjection &projection, NodeRunner &runner
	) {
		ScopeRunner final(graph, projection, runner, RunContext::WHOLE_FRAME, RunContext::WHOLE_FRAME, true);
		return ExecuteEngineGraph(projection.Graph, projection.Blocks[2], final);
	}

	bool ExecuteRenderStage(
		const RenderGraph &graph,
		const RenderStageProjection &projection,
		NodeRunner &runner,
		std::span<const uint64_t> worlds
	) {
		std::vector<uint64_t> distinct;
		for (const auto world : worlds)
			if (std::ranges::find(distinct, world) == distinct.end()) distinct.push_back(world);
		if (distinct.empty()) distinct.push_back(0);
		for (size_t world = 0; world < distinct.size(); world++) {
			bool shared = true;
			for (size_t view = 0; view < worlds.size(); view++) {
				if (worlds[view] != distinct[world]) continue;
				if (!ExecuteRenderView(graph, projection, runner, view, world, shared, shared && world == 0))
					return false;
				shared = false;
			}
			if (shared && !ExecuteRenderView(
							  graph, projection, runner, RunContext::WHOLE_FRAME, world, true, world == 0
						  ))
				return false;
		}
		return ExecuteRenderFinal(graph, projection, runner);
	}

	bool ExecuteRenderStage(
		const RenderGraph &graph, const RenderStageProjection &projection, NodeRunner &runner, size_t views
	) {
		const std::vector<uint64_t> worlds(views, 0);
		return ExecuteRenderStage(graph, projection, runner, worlds);
	}
}
