#include <engine/core/Log.hpp>
#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/Schedule.hpp>
#include <engine/graphcore/Compile.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace engine::graph {

	const char *Describe(ExecutionQueue queue) {
		switch (queue) {
		case ExecutionQueue::Cpu:
			return "cpu";
		case ExecutionQueue::Graphics:
			return "graphics";
		case ExecutionQueue::Compute:
			return "compute";
		case ExecutionQueue::Transfer:
			return "transfer";
		}
		return "unknown";
	}

	const char *Describe(AsyncPolicy policy) {
		switch (policy) {
		case AsyncPolicy::Automatic:
			return "auto";
		case AsyncPolicy::Allow:
			return "allow";
		case AsyncPolicy::Serial:
			return "serial";
		}
		return "unknown";
	}

	const char *Describe(CullingMode mode) {
		switch (mode) {
		case CullingMode::Inherit:
			return "inherit";
		case CullingMode::None:
			return "none";
		case CullingMode::Frustum:
			return "frustum";
		case CullingMode::Occlusion:
			return "occlusion";
		}
		return "unknown";
	}

	const char *Describe(ScheduleStatus status) {
		switch (status) {
		case ScheduleStatus::Ok:
			return "ok";
		case ScheduleStatus::InvalidGraph:
			return "the graph is structurally invalid";
		case ScheduleStatus::MissingProducer:
			return "a resource has no producer";
		case ScheduleStatus::ScopeDependency:
			return "a node depends on work from a later execution scope";
		case ScheduleStatus::Cycle:
			return "the resource dependencies form a cycle";
		case ScheduleStatus::InvalidHint:
			return "a scheduling hint is not valid";
		case ScheduleStatus::LimitExceeded:
			return "the render graph exceeds dependency compilation limits";
		case ScheduleStatus::AllocationFailed:
			return "render dependency compilation could not allocate storage";
		}
		return "unknown";
	}

	namespace {
		constexpr size_t DEPENDENCY_WORD_BITS = 64;

		uint8_t RankOfScope(NodeScope scope, bool setup) {
			switch (scope) {
			case NodeScope::World:
				return 0;
			case NodeScope::View:
				return 1;
			case NodeScope::Frame:
				return setup ? 0 : 2;
			}
			return 2;
		}

		ExecutionQueue QueueOf(const RenderGraph &graph, const Node &node) {
			if (const std::string *hint = node.Parameter(core::Name("queue"));
				hint != nullptr && *hint != "auto") {
				for (const ExecutionQueue candidate :
					 {ExecutionQueue::Cpu,
					  ExecutionQueue::Graphics,
					  ExecutionQueue::Compute,
					  ExecutionQueue::Transfer}) {
					if (*hint == Describe(candidate)) {
						return candidate;
					}
				}
			}

			if (const NodeKindSpec *spec = NodeCatalogue::Find(node.Kind); spec != nullptr) {
				return spec->Queue;
			}

			const auto inspect = [&](std::span<const ResourceId> resources) {
				ExecutionQueue queue = ExecutionQueue::Graphics;
				for (const ResourceId id : resources) {
					const ResourceDesc *resource = graph.FindResourceDesc(id);
					if (resource == nullptr) {
						continue;
					}
					if (resource->Kind == ResourceKind::Camera || resource->Kind == ResourceKind::Entities) {
						return ExecutionQueue::Cpu;
					}
					if (resource->Kind == ResourceKind::Storage || resource->Kind == ResourceKind::Buffer) {
						queue = ExecutionQueue::Compute;
					}
				}
				return queue;
			};

			const ExecutionQueue reads = inspect(node.Reads);
			const ExecutionQueue writes = inspect(node.Writes);
			if (reads == ExecutionQueue::Cpu || writes == ExecutionQueue::Cpu) {
				return ExecutionQueue::Cpu;
			}
			if (reads == ExecutionQueue::Compute || writes == ExecutionQueue::Compute) {
				return ExecutionQueue::Compute;
			}
			return ExecutionQueue::Graphics;
		}

		bool ReadHints(const RenderGraph &graph, const Node &node, NodeId id, ScheduledNode &scheduled) {
			scheduled.Node = id;
			scheduled.Queue = QueueOf(graph, node);

			if (const std::string *queue = node.Parameter(core::Name("queue")); queue != nullptr) {
				bool known = *queue == "auto";
				for (const ExecutionQueue candidate :
					 {ExecutionQueue::Cpu,
					  ExecutionQueue::Graphics,
					  ExecutionQueue::Compute,
					  ExecutionQueue::Transfer}) {
					known = known || *queue == Describe(candidate);
				}
				if (!known) {
					return false;
				}
			}

			if (const std::string *async = node.Parameter(core::Name("async")); async != nullptr) {
				if (*async == "auto") {
					scheduled.Async = AsyncPolicy::Automatic;
				} else if (*async == "allow") {
					scheduled.Async = AsyncPolicy::Allow;
				} else if (*async == "serial") {
					scheduled.Async = AsyncPolicy::Serial;
				} else {
					return false;
				}
			}

			if (const std::string *culling = node.Parameter(core::Name("culling")); culling != nullptr) {
				if (*culling == "inherit") {
					scheduled.Culling = CullingMode::Inherit;
				} else if (*culling == "none") {
					scheduled.Culling = CullingMode::None;
				} else if (*culling == "frustum") {
					scheduled.Culling = CullingMode::Frustum;
				} else if (*culling == "occlusion") {
					scheduled.Culling = CullingMode::Occlusion;
				} else {
					return false;
				}
			}

			const auto group = [&node](const char *key, uint32_t &value) {
				const std::string *text = node.Parameter(core::Name(key));
				if (text == nullptr) {
					return true;
				}
				const char *first = text->data();
				const char *last = first + text->size();
				const auto parsed = std::from_chars(first, last, value);
				return parsed.ec == std::errc{} && parsed.ptr == last && value > 0;
			};
			if (!group("dispatch.x", scheduled.GroupsX) || !group("dispatch.y", scheduled.GroupsY) ||
				!group("dispatch.z", scheduled.GroupsZ)) {
				return false;
			}

			scheduled.AsyncEligible =
				scheduled.Async == AsyncPolicy::Allow ||
				(scheduled.Async == AsyncPolicy::Automatic &&
				 (scheduled.Queue == ExecutionQueue::Compute || scheduled.Queue == ExecutionQueue::Transfer));
			return true;
		}

		void AddDependency(
			std::vector<std::vector<size_t>> &outgoing,
			std::vector<uint64_t> &incoming,
			size_t wordsPerRow,
			size_t before,
			size_t after
		) {
			if (before == after) return;
			auto &word = incoming[after * wordsPerRow + before / DEPENDENCY_WORD_BITS];
			const uint64_t bit = uint64_t{1} << (before % DEPENDENCY_WORD_BITS);
			if ((word & bit) != 0) return;
			word |= bit;
			outgoing[before].push_back(after);
		}
	}

	ScheduleStatus
	CompileSchedule(const RenderGraph &graph, ExecutionSchedule &out, core::Name &offender) try {
		ExecutionSchedule candidate;

		core::Name invalid;
		if (graph.Validate(invalid) != GraphStatus::Ok) {
			offender = invalid;
			return ScheduleStatus::InvalidGraph;
		}

		std::vector<NodeId> ids;
		std::vector<const Node *> nodes;
		for (uint32_t value = 1; value <= graph.Count(); value++) {
			const NodeId id{value};
			const Node *node = graph.Find(id);
			if (node != nullptr && node->Enabled) {
				ids.push_back(id);
				nodes.push_back(node);
			}
		}

		const size_t count = nodes.size();
		const auto firstView = std::find_if(nodes.begin(), nodes.end(), [](const Node *node) {
			return node->Scope == NodeScope::View;
		});
		const size_t setupCount = static_cast<size_t>(firstView - nodes.begin());
		const auto rank = [&](size_t index) { return RankOfScope(nodes[index]->Scope, index < setupCount); };
		std::vector<std::vector<size_t>> outgoing(count);
		const size_t wordsPerRow = (count + DEPENDENCY_WORD_BITS - 1) / DEPENDENCY_WORD_BITS;
		// RenderGraph caps the enabled set at 256; membership never orders outgoing edges.
		std::vector<uint64_t> incoming(count * wordsPerRow);

		{
			std::vector<size_t> writers;
			for (uint32_t resourceValue = 1; resourceValue <= graph.ResourceCount(); resourceValue++) {
				const ResourceId resource{resourceValue};
				const ResourceDesc *desc = graph.FindResourceDesc(resource);
				writers.clear();
				for (size_t index = 0; index < count; index++) {
					if (std::find(nodes[index]->Writes.begin(), nodes[index]->Writes.end(), resource) !=
						nodes[index]->Writes.end()) {
						writers.push_back(index);
					}
				}

				for (size_t index = 1; index < writers.size(); index++) {
					AddDependency(outgoing, incoming, wordsPerRow, writers[index - 1], writers[index]);
				}

				for (size_t reader = 0; reader < count; reader++) {
					if (std::find(nodes[reader]->Reads.begin(), nodes[reader]->Reads.end(), resource) ==
						nodes[reader]->Reads.end()) {
						continue;
					}

					size_t producer = count;
					for (const size_t writer : writers) {
						if (writer < reader) {
							producer = writer;
						}
					}

					if (producer == count && writers.size() == 1 && writers[0] != reader &&
						(desc == nullptr || desc->Lifetime != ResourceLifetime::History)) {
						producer = writers[0];
					}

					if (producer == count) {
						if (desc == nullptr || !desc->External) {
							offender = desc != nullptr ? desc->Name : nodes[reader]->Name;
							ENGINE_WARN(
								"'{}' reads '{}' and nothing in the graph writes it",
								nodes[reader]->Name.Text(),
								offender.Text()
							);
							return ScheduleStatus::MissingProducer;
						}
						// A history read observes the completed previous generation. Its later
						// writer updates the next generation and must not form a same-frame cycle.
						if (desc->Lifetime == ResourceLifetime::History) {
							continue;
						}
						const auto later =
							std::find_if(writers.begin(), writers.end(), [reader](size_t writer) {
								return writer > reader;
							});
						if (later != writers.end()) {
							AddDependency(outgoing, incoming, wordsPerRow, reader, *later);
						}
						continue;
					}

					if (rank(producer) > rank(reader)) {
						offender = nodes[reader]->Name;
						ENGINE_WARN(
							"'{}' reads something '{}' writes at a narrower scope",
							nodes[reader]->Name.Text(),
							nodes[producer]->Name.Text()
						);
						return ScheduleStatus::ScopeDependency;
					}
					AddDependency(outgoing, incoming, wordsPerRow, producer, reader);

					// The reader consumes this version before the next writer replaces
					// it. A producer edge alone lets that overwrite enter the reader's
					// wave, which is a read-write race on a GPU even when the graph is
					// acyclic. This is the anti-dependency that keeps resource versions
					// alive through their last consumer.
					const auto nextWriter =
						std::find_if(writers.begin(), writers.end(), [producer](size_t writer) {
							return writer > producer;
						});
					if (nextWriter != writers.end()) {
						AddDependency(outgoing, incoming, wordsPerRow, reader, *nextWriter);
					}
				}
			}
		}

		// Scope is an execution boundary even when two nodes share no resource.
		// A final frame overlay cannot start beside a world shadow just because
		// their targets differ: it belongs after every view. Frame setup belongs
		// before the views and retains its place among world nodes. Work that
		// produces one world's shared state completes before any view of that
		// world begins. These edges preserve concurrency inside a scope while
		// making `World`, `View`, and `Frame` mean more than labels in the editor.
		for (size_t before = 0; before < count; before++) {
			for (size_t after = 0; after < count; after++) {
				if (rank(before) < rank(after)) {
					AddDependency(outgoing, incoming, wordsPerRow, before, after);
				}
				// Frame setup and world setup share an ordered prefix. Keep their
				// authored boundary while allowing independent world work to overlap.
				if (before < after && after < setupCount && nodes[before]->Scope != nodes[after]->Scope) {
					AddDependency(outgoing, incoming, wordsPerRow, before, after);
				}
			}
		}

		std::vector<graphcore::Dependency> dependencies;
		for (size_t before = 0; before < count; ++before)
			for (const size_t after : outgoing[before])
				dependencies.push_back(
					{static_cast<graphcore::NodeIndex>(before), static_cast<graphcore::NodeIndex>(after)}
				);
		const graphcore::Limits limits{
			.MaximumNodes = RenderGraph::MAXIMUM_NODES,
			.MaximumConnections = size_t{RenderGraph::MAXIMUM_NODES} * (RenderGraph::MAXIMUM_NODES - 1),
		};
		graphcore::Topology topology;
		graphcore::Diagnostic diagnostic;
		const auto status = graphcore::CompileDependencies(count, dependencies, limits, topology, diagnostic);
		if (status != graphcore::CompileStatus::Ok && status != graphcore::CompileStatus::Cycle) {
			offender = {};
			if (status == graphcore::CompileStatus::LimitExceeded) return ScheduleStatus::LimitExceeded;
			if (status == graphcore::CompileStatus::AllocationFailed) return ScheduleStatus::AllocationFailed;
			return ScheduleStatus::InvalidGraph;
		}
		// A reachable bad hint precedes a later cycle in the established refusal order.
		// Prefix waves are diagnostics, never a partially installed schedule.
		const auto &waves =
			status == graphcore::CompileStatus::Cycle ? diagnostic.AcyclicPrefixWaves : topology.Waves;
		std::vector<uint8_t> completed(count);
		size_t emitted = 0;
		for (const auto &ready : waves) {
			ExecutionWave wave;
			std::unordered_set<uint8_t> queues;
			bool permitsOverlap = false;
			bool forcesSerial = false;
			for (const auto index : ready) {
				ScheduledNode scheduled;
				if (!ReadHints(graph, *nodes[index], ids[index], scheduled)) {
					offender = nodes[index]->Name;
					ENGINE_WARN("'{}' carries a hint this build cannot read", offender.Text());
					return ScheduleStatus::InvalidHint;
				}
				queues.insert(static_cast<uint8_t>(scheduled.Queue));
				permitsOverlap = permitsOverlap || scheduled.AsyncEligible;
				forcesSerial = forcesSerial || scheduled.Async == AsyncPolicy::Serial;
				wave.Nodes.push_back(scheduled);
				completed[index] = 1;
			}
			wave.Concurrent = queues.size() > 1 && permitsOverlap && !forcesSerial;
			emitted += ready.size();
			candidate.Waves.push_back(std::move(wave));
		}
		if (status == graphcore::CompileStatus::Cycle) {
			offender = diagnostic.Blocked < count ? nodes[diagnostic.Blocked]->Name : core::Name{};
			for (size_t index = 0; index < count; ++index)
				if (!completed[index])
					ENGINE_WARN("'{}' is in the unschedulable cycle", nodes[index]->Name.Text());
			return ScheduleStatus::Cycle;
		}

		// The wave shape is what decides how much of a frame can overlap, and it
		// is computed once at compile and never reported. `--log graph=debug`
		// answers "why is nothing running on the compute queue".
		ENGINE_DEBUG("scheduled {} node(s) into {} wave(s)", emitted, candidate.Waves.size());
		for (size_t index = 0; index < candidate.Waves.size(); index++) {
			ENGINE_TRACE(
				"wave {}: {} node(s), {}",
				index,
				candidate.Waves[index].Nodes.size(),
				candidate.Waves[index].Concurrent ? "concurrent" : "serial"
			);
		}
		out = std::move(candidate);
		offender = {};
		return ScheduleStatus::Ok;
	} catch (const std::bad_alloc &) {
		offender = {};
		return ScheduleStatus::AllocationFailed;
	} catch (const std::length_error &) {
		offender = {};
		return ScheduleStatus::AllocationFailed;
	}
}
