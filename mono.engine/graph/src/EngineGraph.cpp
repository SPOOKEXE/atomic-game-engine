#include <engine/ang/Compile.hpp>
#include <engine/ang/Runner.hpp>
#include <engine/graph/EngineGraph.hpp>
#include <engine/graphcore/Compile.hpp>

#include <algorithm>
#include <charconv>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
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

	// One schedule's waves as an ang Tick plan, with the adapter of the call
	// running it. Boundary and stage nodes read the call state through `Self`.
	struct EngineProgram {
		// Wave sizes then node handles, to notice a schedule edited after lowering.
		std::vector<uint32_t> Signature;
		// Handovers each wave opens with.
		std::vector<std::vector<EngineBarrier>> Barriers;
		std::vector<bool> Concurrent;
		// Scheduled handle of each stage node, by its ang node index.
		std::vector<EngineNodeId> Work;
		std::vector<uint32_t> WaveOf;
		std::unique_ptr<ang::Runner> Runner;

		// Set for the duration of one ExecuteEngineGraph.
		const EngineGraph *Graph = nullptr;
		EngineNodeRunner *Adapter = nullptr;
		// What an adapter threw, rethrown to the caller after the run stops.
		std::exception_ptr Thrown;
	};

	namespace {
		std::vector<uint32_t> SignatureOf(const EngineSchedule &schedule) {
			std::vector<uint32_t> signature;
			signature.push_back(static_cast<uint32_t>(schedule.Waves.size()));
			for (const EngineWave &wave : schedule.Waves) {
				signature.push_back(
					static_cast<uint32_t>(wave.Nodes.size()) | (wave.Concurrent ? 1u << 31 : 0u)
				);
			}
			for (const EngineWave &wave : schedule.Waves) {
				for (const EngineScheduledNode &entry : wave.Nodes) {
					signature.push_back(entry.Node.Value);
				}
			}
			signature.push_back(static_cast<uint32_t>(schedule.Barriers.size()));
			for (const EngineBarrier &barrier : schedule.Barriers) {
				signature.push_back(barrier.Resource.Id());
				signature.push_back(
					static_cast<uint32_t>(barrier.From) << 8 | static_cast<uint32_t>(barrier.To)
				);
				signature.push_back(barrier.ProducerWave);
				signature.push_back(barrier.ConsumerWave);
			}
			return signature;
		}

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

		// Lowers `schedule` into `program`. Node order is J0, wave 0, J1, wave 1,
		// ..., Jn, chained by order links, so ang's waves are exactly the
		// schedule's waves with a boundary node between each pair.
		bool Lower(const EngineGraph &graph, const EngineSchedule &schedule, EngineProgram &program) {
			ang::Registry registry;
			registry.DefineType({.Id = std::string(ORDER_TYPE), .Label = "Order"});
			const ang::PortDef after{.Name = "after", .Type = std::string(ORDER_TYPE), .MaximumLinks = 0};
			const ang::PortDef done{
				.Name = "done", .Type = std::string(ORDER_TYPE), .Dir = ang::Direction::Output
			};
			EngineProgram *self = &program;
			const auto mark = [](ang::EvalContext &context) {
				context.SetOutput("done", ang::Value(true).WithType(std::string(ORDER_TYPE)));
				return ang::NodeResult::Success();
			};
			// ang turns a throw into a failed node; keep it so the caller still sees it.
			const auto guarded = [self](auto call) {
				return [self, call](ang::EvalContext &context) {
					try {
						return call(context);
					} catch (...) {
						self->Thrown = std::current_exception();
						return ang::NodeResult::Failure("adapter threw");
					}
				};
			};
			// Closes the wave before it and opens its own; the last one only closes.
			const auto boundary = guarded([self, mark](ang::EvalContext &context) {
				const auto wave = static_cast<uint32_t>(context.Param("wave").AsNumber());
				if (wave > 0 && !self->Adapter->EndWave(wave - 1)) {
					return ang::NodeResult::Failure("EndWave refused");
				}
				if (wave < self->Barriers.size() &&
					!self->Adapter->BeginWave(wave, self->Concurrent[wave], self->Barriers[wave])) {
					return ang::NodeResult::Failure("BeginWave refused");
				}
				return mark(context);
			});
			const auto stage = guarded([self, mark](ang::EvalContext &context) {
				const auto slot = static_cast<size_t>(context.Param("slot").AsNumber());
				const EngineNodeId id = self->Work[slot];
				const EngineNode *node = self->Graph->Find(id);
				const uint32_t wave = self->WaveOf[slot];
				if (node == nullptr || !self->Adapter->Run({*node, wave, self->Barriers[wave], id})) {
					return ang::NodeResult::Failure("Run refused");
				}
				return mark(context);
			});
			if (!registry
					 .DefineNode(
						 {.Id = "engine.begin",
						  .Outputs = {done},
						  .Pure = ang::Purity::Impure,
						  .Evaluate = boundary}
					 )
					 .Ok ||
				!registry
					 .DefineNode(
						 {.Id = "engine.wave",
						  .Inputs = {after},
						  .Outputs = {done},
						  .Pure = ang::Purity::Impure,
						  .Evaluate = boundary}
					 )
					 .Ok) {
				return false;
			}
			for (uint8_t index = 0; index < static_cast<uint8_t>(EngineStage::Count); ++index) {
				if (!registry
						 .DefineNode(
							 {.Id = StageDef(static_cast<EngineStage>(index)),
							  .Inputs = {after},
							  .Outputs = {done},
							  .Pure = ang::Purity::Impure,
							  .Evaluate = stage}
						 )
						 .Ok) {
					return false;
				}
			}
			if (!registry
					 .DefineNode(
						 {.Id = StageDef(EngineStage::Count),
						  .Inputs = {after},
						  .Outputs = {done},
						  .Pure = ang::Purity::Impure,
						  .Evaluate = stage}
					 )
					 .Ok) {
				return false;
			}

			program.Barriers.assign(schedule.Waves.size(), {});
			for (const EngineBarrier &barrier : schedule.Barriers) {
				if (barrier.ConsumerWave < program.Barriers.size()) {
					program.Barriers[barrier.ConsumerWave].push_back(barrier);
				}
			}
			program.Concurrent.clear();
			program.Work.clear();
			program.WaveOf.clear();
			std::vector<ang::Edit> edits;
			std::vector<std::string> previous;
			const auto boundaryNode = [&](uint32_t wave) {
				const std::string id = "wave." + std::to_string(wave);
				edits.push_back(
					ang::edit::AddNode{
						{.Id = id,
						 .Def = wave == 0 ? "engine.begin" : "engine.wave",
						 .Params = {{"wave", ang::Value(int64_t{wave})}}}
					}
				);
				for (const std::string &from : previous) {
					edits.push_back(ang::edit::AddLink{{from, "done", id, "after"}});
				}
				previous.assign(1, id);
			};
			for (uint32_t wave = 0; wave < schedule.Waves.size(); ++wave) {
				boundaryNode(wave);
				program.Concurrent.push_back(schedule.Waves[wave].Concurrent);
				std::vector<std::string> members;
				for (const EngineScheduledNode &entry : schedule.Waves[wave].Nodes) {
					const size_t slot = program.Work.size();
					const EngineNode *node = graph.Find(entry.Node);
					const std::string id = "node." + std::to_string(slot);
					edits.push_back(
						ang::edit::AddNode{
							{.Id = id,
							 .Def = StageDef(node == nullptr ? EngineStage::Count : node->Stage),
							 .Params = {{"slot", ang::Value(static_cast<int64_t>(slot))}}}
						}
					);
					edits.push_back(ang::edit::AddLink{{previous.front(), "done", id, "after"}});
					program.Work.push_back(entry.Node);
					program.WaveOf.push_back(wave);
					members.push_back(id);
				}
				if (!members.empty()) {
					previous = std::move(members);
				}
			}
			boundaryNode(static_cast<uint32_t>(schedule.Waves.size()));

			ang::Graph lowered;
			if (!lowered.Apply(edits).Ok) {
				return false;
			}
			ang::Plan plan;
			const size_t nodes = program.Work.size() + schedule.Waves.size() + 1;
			const ang::CompileOptions options{
				.Runs = ang::Domain::Tick,
				.MaximumNodes = nodes,
				.MaximumLinks = edits.size(),
				.MaximumPorts = nodes * 2,
				.MaximumIdentityBytes = 64u * 1024u * 1024u,
				.Order = graphcore::OrderPolicy::LowestReady,
			};
			if (!ang::Compile(lowered, registry, options, plan).Ok) {
				return false;
			}
			program.Runner = std::make_unique<ang::Runner>(
				std::move(plan), ang::RunnerOptions{.TimelineRuns = 1, .StopOnFailure = true}
			);
			program.Signature = SignatureOf(schedule);
			return true;
		}
	}

	// No hooks are set on this path. `just enginegraph-bench` (bench preset,
	// unity off, 2026-11-25), prepared 64-node schedules: 9.6 us independent and
	// 18.3 us chained through ang, 0.16 us and 1.3 us for a plain wave loop. The
	// cost is per executed node; lowering happens once per compiled schedule.
	bool
	ExecuteEngineGraph(const EngineGraph &graph, const EngineSchedule &schedule, EngineNodeRunner &runner) {
		if (schedule.Program.Held == nullptr) {
			schedule.Program.Held = std::make_shared<EngineProgram>();
		}
		// Held here so an adapter that clears the schedule cannot free a live run.
		const std::shared_ptr<EngineProgram> held = schedule.Program.Held;
		EngineProgram &program = *held;
		if (program.Runner == nullptr || program.Signature != SignatureOf(schedule)) {
			program.Runner.reset();
			if (!Lower(graph, schedule, program)) {
				return false;
			}
		}
		program.Graph = &graph;
		program.Adapter = &runner;
		const ang::RunReport report = program.Runner->Run();
		program.Graph = nullptr;
		program.Adapter = nullptr;
		if (program.Thrown != nullptr) {
			std::rethrow_exception(std::exchange(program.Thrown, nullptr));
		}
		return report.Failed == 0 && report.Skipped == 0;
	}
}
