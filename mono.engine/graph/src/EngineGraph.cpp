#include <engine/ang/Compile.hpp>
#include <engine/ang/Runner.hpp>
#include <engine/graph/EngineGraph.hpp>
#include <engine/graphcore/Compile.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
		case EngineGraphStatus::LimitExceeded:
			return "the engine graph exceeds dependency compilation limits";
		case EngineGraphStatus::InvalidDependency:
			return "the engine graph has an invalid dependency";
		case EngineGraphStatus::AllocationFailed:
			return "engine dependency compilation could not allocate storage";
		}
		return "unknown engine graph status";
	}

	bool EngineGraph::AddResource(EngineResourceDesc resource) try {
		if (!resource.Name.IsValid() || DeclaredResources.size() >= MAXIMUM_RESOURCES ||
			FindResourceDesc(resource.Name) != nullptr) {
			return false;
		}
		DeclaredResources.push_back(std::move(resource));
		return true;
	} catch (const std::bad_alloc &) {
		return false;
	} catch (const std::length_error &) {
		return false;
	}

	EngineNodeId EngineGraph::AddNode(EngineNode node) try {
		if (!node.Name.IsValid() || node.Stage == EngineStage::Count || Nodes.size() >= MAXIMUM_NODES ||
			node.Reads.size() > MAXIMUM_RESOURCE_USES || node.Writes.size() > MAXIMUM_RESOURCE_USES) {
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
	} catch (const std::bad_alloc &) {
		return {};
	} catch (const std::length_error &) {
		return {};
	}

	const EngineNode *EngineGraph::Find(EngineNodeId node) const {
		return node.IsValid() && node.Value <= Nodes.size() ? &Nodes[node.Value - 1] : nullptr;
	}

	const EngineResourceDesc *EngineGraph::FindResourceDesc(core::Name name) const {
		const auto found = std::find_if(
			DeclaredResources.begin(), DeclaredResources.end(), [&](const EngineResourceDesc &resource) {
				return resource.Name == name;
			}
		);
		return found == DeclaredResources.end() ? nullptr : &*found;
	}

	namespace {
		const char *LifetimeType(EngineResourceLifetime lifetime) {
			switch (lifetime) {
			case EngineResourceLifetime::Wave:
				return "engine.resource.wave";
			case EngineResourceLifetime::Tick:
				return "engine.resource.tick";
			case EngineResourceLifetime::World:
				return "engine.resource.world";
			case EngineResourceLifetime::Persistent:
				return "engine.resource.persistent";
			}
			return "engine.resource.tick";
		}

		constexpr std::string_view ORDER_TYPE = "engine.order";

		// Resource ports carry a prefix so no resource name can collide with `after` or `done`.
		std::string ResourcePort(core::Name resource) {
			return "resource:" + std::string(resource.Text());
		}

		// Lowers the derived dependencies to an ang Tick graph and lets ang (and
		// graphcore under it) compute waves and the LowestReady serial order.
		// Each node gets its own definition: an input per read resource, an
		// output per written resource, typed by the resource lifetime, plus
		// `after`/`done` order ports for edges that carry no resource.
		EngineGraphStatus OrderThroughAng(
			const EngineGraph &graph,
			const std::vector<std::unordered_set<size_t>> &outgoing,
			const std::vector<std::vector<std::pair<core::Name, size_t>>> &producers,
			std::vector<graphcore::NodeIndex> &order,
			std::vector<std::vector<graphcore::NodeIndex>> &waves,
			core::Name &offender
		) {
			const std::span<const EngineNode> nodes = graph.AllNodes();
			ang::Registry registry;
			registry.DefineType({.Id = std::string(ORDER_TYPE), .Label = "Order"});
			for (const EngineResourceLifetime lifetime :
				 {EngineResourceLifetime::Wave,
				  EngineResourceLifetime::Tick,
				  EngineResourceLifetime::World,
				  EngineResourceLifetime::Persistent}) {
				registry.DefineType({.Id = LifetimeType(lifetime), .Label = LifetimeType(lifetime)});
			}
			const auto typeOf = [&](core::Name resource) {
				return std::string(LifetimeType(graph.FindResourceDesc(resource)->Lifetime));
			};

			std::vector<ang::Edit> edits;
			edits.reserve(nodes.size() * 2);
			size_t links = 0;
			for (size_t index = 0; index < nodes.size(); ++index) {
				const EngineNode &node = nodes[index];
				ang::NodeDef def{.Id = "engine.node." + std::to_string(index)};
				def.Inputs.push_back(
					{.Name = "after", .Type = std::string(ORDER_TYPE), .Optional = true, .MaximumLinks = 0}
				);
				def.Outputs.push_back(
					{.Name = "done", .Type = std::string(ORDER_TYPE), .Dir = ang::Direction::Output}
				);
				std::unordered_set<uint32_t> seen;
				for (const core::Name resource : node.Reads) {
					if (seen.insert(resource.Id()).second) {
						def.Inputs.push_back(
							{.Name = ResourcePort(resource), .Type = typeOf(resource), .Optional = true}
						);
					}
				}
				seen.clear();
				for (const core::Name resource : node.Writes) {
					if (seen.insert(resource.Id()).second) {
						def.Outputs.push_back(
							{.Name = ResourcePort(resource),
							 .Type = typeOf(resource),
							 .Dir = ang::Direction::Output}
						);
					}
				}
				if (!registry.DefineNode(std::move(def)).Ok) {
					offender = node.Name;
					return EngineGraphStatus::InvalidDependency;
				}
				edits.push_back(
					ang::edit::AddNode{
						{.Id = std::to_string(index), .Def = "engine.node." + std::to_string(index)}
					}
				);
			}
			for (size_t before = 0; before < nodes.size(); ++before) {
				// Sorted so link order, and with it graphcore's tie breaks, never
				// depends on hash set iteration.
				std::vector<size_t> after(outgoing[before].begin(), outgoing[before].end());
				std::sort(after.begin(), after.end());
				for (const size_t consumer : after) {
					bool carried = false;
					// A reader takes one producer per resource, the one the
					// dependency walk chose; any other edge is order only.
					for (const auto &[resource, producer] : producers[consumer]) {
						if (producer != before) {
							continue;
						}
						edits.push_back(
							ang::edit::AddLink{
								{std::to_string(before),
								 ResourcePort(resource),
								 std::to_string(consumer),
								 ResourcePort(resource)}
							}
						);
						carried = true;
						++links;
					}
					if (!carried) {
						edits.push_back(
							ang::edit::AddLink{
								{std::to_string(before), "done", std::to_string(consumer), "after"}
							}
						);
						++links;
					}
				}
			}
			ang::Graph lowered;
			if (!lowered.Apply(edits).Ok) {
				offender = {};
				return EngineGraphStatus::InvalidDependency;
			}
			ang::Plan plan;
			const ang::CompileOptions options{
				.Runs = ang::Domain::Tick,
				.MaximumNodes = EngineGraph::MAXIMUM_NODES,
				.MaximumLinks = links + 1,
				.MaximumPorts =
					size_t{EngineGraph::MAXIMUM_NODES} * (2 * EngineGraph::MAXIMUM_RESOURCE_USES + 2),
				.MaximumIdentityBytes = 64u * 1024u * 1024u,
				.Order = graphcore::OrderPolicy::LowestReady,
			};
			const ang::CompileResult compiled = ang::Compile(lowered, registry, options, plan);
			if (!compiled.Ok) {
				const ang::Diagnostic &first = compiled.Diagnostics.front();
				uint32_t blocked = 0;
				const bool named =
					!first.Node.empty() &&
					std::from_chars(first.Node.data(), first.Node.data() + first.Node.size(), blocked).ec ==
						std::errc{} &&
					blocked < nodes.size();
				offender = named ? nodes[blocked].Name : core::Name{};
				switch (first.GraphStatus) {
				case graphcore::CompileStatus::Cycle:
					return EngineGraphStatus::Cycle;
				case graphcore::CompileStatus::LimitExceeded:
					return EngineGraphStatus::LimitExceeded;
				case graphcore::CompileStatus::AllocationFailed:
					return EngineGraphStatus::AllocationFailed;
				default:
					return EngineGraphStatus::InvalidDependency;
				}
			}
			// Plan indices follow declaration order; map through ids regardless.
			std::vector<graphcore::NodeIndex> nodeOf(plan.Nodes.size());
			for (size_t index = 0; index < plan.Nodes.size(); ++index) {
				nodeOf[index] = static_cast<graphcore::NodeIndex>(std::stoul(plan.Nodes[index].Id));
			}
			order.clear();
			for (const uint32_t index : plan.Order) {
				order.push_back(nodeOf[index]);
			}
			waves.clear();
			for (const std::vector<uint32_t> &wave : plan.Waves) {
				std::vector<graphcore::NodeIndex> ready;
				for (const uint32_t index : wave) {
					ready.push_back(nodeOf[index]);
				}
				waves.push_back(std::move(ready));
			}
			return EngineGraphStatus::Ok;
		}

		void AddEdge(std::vector<std::unordered_set<size_t>> &outgoing, size_t before, size_t after) {
			if (before != after) {
				outgoing[before].insert(after);
			}
		}
	}

	EngineGraphStatus CompileEngineGraph(
		const EngineGraph &graph, EngineSchedule &out, core::Name &offender, EngineExecutionMode mode
	) try {
		EngineSchedule candidate;
		const std::span<const EngineNode> nodes = graph.AllNodes();
		const size_t count = nodes.size();
		// Admission enforces these bounds; check again before derived work so
		// this compiler never relies on a caller constructing a smaller graph.
		if (count > EngineGraph::MAXIMUM_NODES || graph.Resources().size() > EngineGraph::MAXIMUM_RESOURCES) {
			offender = {};
			return EngineGraphStatus::LimitExceeded;
		}
		std::vector<std::unordered_set<size_t>> outgoing(count);
		std::unordered_map<uint32_t, std::vector<size_t>> writers;
		// Per reader, the producer chosen for each resource it reads.
		std::vector<std::vector<std::pair<core::Name, size_t>>> producers(count);

		for (size_t index = 0; index < count; index++) {
			for (const core::Name resource : nodes[index].Reads) {
				if (!resource.IsValid()) {
					offender = nodes[index].Name;
					return EngineGraphStatus::InvalidName;
				}
				if (graph.FindResourceDesc(resource) == nullptr) {
					offender = resource;
					return EngineGraphStatus::UnknownResource;
				}
			}
			for (const core::Name resource : nodes[index].Writes) {
				if (!resource.IsValid()) {
					offender = nodes[index].Name;
					return EngineGraphStatus::InvalidName;
				}
				if (graph.FindResourceDesc(resource) == nullptr) {
					offender = resource;
					return EngineGraphStatus::UnknownResource;
				}
				writers[resource.Id()].push_back(index);
			}
		}

		for (size_t before = 0; before < count; before++) {
			for (size_t after = 0; after < count; after++) {
				if (nodes[before].Stage < nodes[after].Stage) {
					AddEdge(outgoing, before, after);
				}
			}
		}

		for (const EngineResourceDesc &resource : graph.Resources()) {
			const auto found = writers.find(resource.Name.Id());
			const std::vector<size_t> empty;
			const std::vector<size_t> &resourceWriters = found == writers.end() ? empty : found->second;
			for (size_t index = 1; index < resourceWriters.size(); index++) {
				AddEdge(outgoing, resourceWriters[index - 1], resourceWriters[index]);
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
				AddEdge(outgoing, producer, reader);
				if (producer != reader) {
					producers[reader].push_back({resource.Name, producer});
				}
				const auto next =
					std::find_if(resourceWriters.begin(), resourceWriters.end(), [producer](size_t writer) {
						return writer > producer;
					});
				if (next != resourceWriters.end()) {
					AddEdge(outgoing, reader, *next);
				}
			}
		}

		std::vector<graphcore::NodeIndex> serialOrder;
		std::vector<std::vector<graphcore::NodeIndex>> waves;
		const EngineGraphStatus lowered =
			OrderThroughAng(graph, outgoing, producers, serialOrder, waves, offender);
		if (lowered != EngineGraphStatus::Ok) {
			return lowered;
		}

		std::vector<uint32_t> waveOf(count, std::numeric_limits<uint32_t>::max());
		const auto appendWave = [&](std::span<const graphcore::NodeIndex> ready) {
			EngineWave wave;
			std::unordered_set<uint8_t> queues;
			const uint32_t waveIndex = static_cast<uint32_t>(candidate.Waves.size());
			for (const graphcore::NodeIndex index : ready) {
				wave.Nodes.push_back({
					.Node = EngineNodeId{static_cast<uint32_t>(index + 1)},
					.Queue = nodes[index].Queue,
					.AsyncEligible = nodes[index].AsyncEligible,
					.Profile = nodes[index].Profile,
				});
				queues.insert(static_cast<uint8_t>(nodes[index].Queue));
				waveOf[index] = waveIndex;
			}
			wave.Concurrent = mode == EngineExecutionMode::Concurrent && ready.size() > 1 &&
							  (queues.size() == 1 ||
							   std::any_of(ready.begin(), ready.end(), [&](graphcore::NodeIndex index) {
								   return nodes[index].AsyncEligible;
							   }));
			candidate.Waves.push_back(std::move(wave));
		};
		if (mode == EngineExecutionMode::Deterministic) {
			for (const graphcore::NodeIndex index : serialOrder) {
				appendWave(std::span<const graphcore::NodeIndex>(&index, 1));
			}
		} else {
			for (const std::vector<graphcore::NodeIndex> &ready : waves) {
				appendWave(ready);
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
					if (candidate.Barriers.size() >= EngineGraph::MAXIMUM_BARRIERS) {
						offender = resource;
						return EngineGraphStatus::LimitExceeded;
					}
					candidate.Barriers.push_back({
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
				candidate.Resources.push_back({resource.Name, resource.Lifetime, first, last});
			}
		}
		out = std::move(candidate);
		offender = {};
		return EngineGraphStatus::Ok;
	} catch (const std::bad_alloc &) {
		offender = {};
		return EngineGraphStatus::AllocationFailed;
	} catch (const std::length_error &) {
		offender = {};
		return EngineGraphStatus::AllocationFailed;
	}

	namespace {
		const char *StageDef(EngineStage stage) {
			switch (stage) {
			case EngineStage::Input:
				return "engine.stage.input";
			case EngineStage::Ai:
				return "engine.stage.ai";
			case EngineStage::World:
				return "engine.stage.world";
			case EngineStage::Physics:
				return "engine.stage.physics";
			case EngineStage::Animation:
				return "engine.stage.animation";
			case EngineStage::Render:
				return "engine.stage.render";
			case EngineStage::Count:
				break;
			}
			return "engine.stage.unknown";
		}

		int64_t NowNanoseconds() {
			return std::chrono::duration_cast<std::chrono::nanoseconds>(
					   std::chrono::steady_clock::now().time_since_epoch()
			)
				.count();
		}

		// Records one observed run. Built only when a caller passed an observer.
		class Observation {
		  public:
			Observation(EngineObserver &observer, const EngineSchedule &schedule)
				: Observer(observer), Start(NowNanoseconds()) {
				size_t nodes = 0;
				for (const EngineWave &wave : schedule.Waves) {
					nodes += wave.Nodes.size();
				}
				Report.RunNumber = observer.Report.RunNumber + 1;
				Report.Nodes.resize(nodes);
				Run.RunNumber = Report.RunNumber;
				Run.Samples.reserve(nodes);
				if (Observer.Hooks.PreRun) {
					Observer.Hooks.PreRun(Info());
				}
			}

			bool Node(
				const EngineNode &node,
				uint32_t wave,
				EngineNodeRunner &runner,
				const EngineRunContext &context
			) {
				const ang::NodeInfo info{
					.Id = node.Name.Text(),
					.Def = StageDef(node.Stage),
					.Index = Next,
					.Wave = wave,
					.Mode = ang::ExecMode::Sync,
					.RunNumber = Report.RunNumber,
				};
				const int64_t begin = NowNanoseconds();
				if (Observer.Hooks.PreNode) {
					Observer.Hooks.PreNode(info);
				}
				if (Observer.Hooks.PreCompute) {
					Observer.Hooks.PreCompute(info);
				}
				ang::ComputeResult result;
				if (!runner.Run(context)) {
					result.Status = ang::NodeStatus::Failed;
					result.Note = "the adapter refused the node";
				}
				if (Observer.Hooks.PostCompute) {
					Observer.Hooks.PostCompute(info, result);
				}
				const int64_t end = NowNanoseconds();
				ang::NodeReport &entry = Report.Nodes[Next++];
				entry.Status = result.Status;
				entry.Note = std::move(result.Note);
				entry.Milliseconds = static_cast<double>(end - begin) / 1.0e6;
				const bool accepted = entry.Status == ang::NodeStatus::Ran;
				++(accepted ? Report.Ran : Report.Failed);
				Run.Samples.push_back(
					{.Node = info.Index,
					 .BeginNanoseconds = begin - Start,
					 .EndNanoseconds = end - Start,
					 .Thread = std::hash<std::thread::id>{}(std::this_thread::get_id()),
					 .Outcome = entry.Status}
				);
				if (Observer.Hooks.PostNode) {
					Observer.Hooks.PostNode(info, entry.Status);
				}
				return accepted;
			}

			// Publishes the run, marking what a refusal left unvisited.
			void Finish() {
				for (size_t index = Next; index < Report.Nodes.size(); ++index) {
					Report.Nodes[index].Status = ang::NodeStatus::Skipped;
					++Report.Skipped;
				}
				Report.Milliseconds = static_cast<double>(NowNanoseconds() - Start) / 1.0e6;
				Run.Milliseconds = Report.Milliseconds;
				Observer.Timeline.Push(std::move(Run));
				Observer.Report = std::move(Report);
				if (Observer.Hooks.PostRun) {
					ang::RunInfo info{
						.RunNumber = Observer.Report.RunNumber,
						.Runs = ang::Domain::Tick,
						.NodeCount = Observer.Report.Nodes.size(),
						.Report = &Observer.Report,
					};
					Observer.Hooks.PostRun(info);
				}
			}

		  private:
			ang::RunInfo Info() const {
				return {
					.RunNumber = Report.RunNumber, .Runs = ang::Domain::Tick, .NodeCount = Report.Nodes.size()
				};
			}

			EngineObserver &Observer;
			int64_t Start;
			uint32_t Next = 0;
			ang::RunReport Report;
			ang::TimelineRun Run;
		};

		bool ExecuteWaves(
			const EngineGraph &graph,
			const EngineSchedule &schedule,
			EngineNodeRunner &runner,
			Observation *observation
		) {
			// Keep capacity until this call returns to avoid rebuilding wave storage.
			std::vector<EngineBarrier> barriers;
			for (uint32_t wave = 0; wave < schedule.Waves.size(); wave++) {
				barriers.clear();
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
					if (node == nullptr) {
						return false;
					}
					const EngineRunContext context{*node, wave, barriers, entry.Node};
					const bool accepted = observation == nullptr
											  ? runner.Run(context)
											  : observation->Node(*node, wave, runner, context);
					if (!accepted) {
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

	// A plain loop, not an ang Runner: this runs per view per frame and needs none
	// of the runner's caching. `just enginegraph-bench` (bench preset, unity off,
	// 2026-11-25), prepared 64-node schedules with no observer: 0.19 us independent
	// and 1.4 us chained, against 9.6 us and 18.3 us through a Runner.
	bool ExecuteEngineGraph(
		const EngineGraph &graph,
		const EngineSchedule &schedule,
		EngineNodeRunner &runner,
		EngineObserver *observer
	) {
		if (observer == nullptr) {
			return ExecuteWaves(graph, schedule, runner, nullptr);
		}
		Observation observation(*observer, schedule);
		const bool completed = ExecuteWaves(graph, schedule, runner, &observation);
		observation.Finish();
		return completed;
	}
}
