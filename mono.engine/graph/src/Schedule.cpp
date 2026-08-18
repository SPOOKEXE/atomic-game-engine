#include <engine/graph/Schedule.hpp>

#include <algorithm>
#include <charconv>
#include <unordered_set>

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
		}
		return "unknown";
	}

	namespace {
		uint8_t ScopeRank(NodeScope scope) {
			switch (scope) {
			case NodeScope::World:
				return 0;
			case NodeScope::View:
				return 1;
			case NodeScope::Frame:
				return 2;
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

			if (node.Kind == core::Name("viewer") || node.Kind == core::Name("capture") ||
				node.Kind == core::Name("upload-instances") || node.Kind == core::Name("output")) {
				return ExecutionQueue::Transfer;
			}

			const auto inspect = [&](std::span<const ResourceId> resources) {
				ExecutionQueue queue = ExecutionQueue::Graphics;
				for (const ResourceId id : resources) {
					const ResourceDesc *resource = graph.FindResource(id);
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
			if (reads == ExecutionQueue::Compute || writes == ExecutionQueue::Compute ||
				node.Kind == core::Name("dispatch")) {
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
			std::vector<std::unordered_set<size_t>> &incoming,
			size_t before,
			size_t after
		) {
			if (before == after || !incoming[after].insert(before).second) {
				return;
			}
			outgoing[before].push_back(after);
		}
	}

	ScheduleStatus CompileSchedule(const RenderGraph &graph, ExecutionSchedule &out, core::Name &offender) {
		out.Clear();

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
		std::vector<std::vector<size_t>> outgoing(count);
		std::vector<std::unordered_set<size_t>> incoming(count);

		for (uint32_t resourceValue = 1; resourceValue <= graph.ResourceCount(); resourceValue++) {
			const ResourceId resource{resourceValue};
			const ResourceDesc *desc = graph.FindResource(resource);
			std::vector<size_t> writers;
			for (size_t index = 0; index < count; index++) {
				if (std::find(nodes[index]->Writes.begin(), nodes[index]->Writes.end(), resource) !=
					nodes[index]->Writes.end()) {
					writers.push_back(index);
				}
			}

			for (size_t index = 1; index < writers.size(); index++) {
				AddDependency(outgoing, incoming, writers[index - 1], writers[index]);
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

				if (producer == count && writers.size() == 1 && writers[0] != reader) {
					producer = writers[0];
				}

				if (producer == count) {
					if (desc == nullptr || !desc->External) {
						offender = desc != nullptr ? desc->Name : nodes[reader]->Name;
						return ScheduleStatus::MissingProducer;
					}
					const auto later = std::find_if(writers.begin(), writers.end(), [reader](size_t writer) {
						return writer > reader;
					});
					if (later != writers.end()) {
						AddDependency(outgoing, incoming, reader, *later);
					}
					continue;
				}

				if (ScopeRank(nodes[producer]->Scope) > ScopeRank(nodes[reader]->Scope)) {
					offender = nodes[reader]->Name;
					return ScheduleStatus::ScopeDependency;
				}
				AddDependency(outgoing, incoming, producer, reader);

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
					AddDependency(outgoing, incoming, reader, *nextWriter);
				}
			}
		}

		// Scope is an execution boundary even when two nodes share no resource.
		// A frame-scoped overlay cannot start beside a world shadow just because
		// their targets differ: it belongs after every view. Likewise, work that
		// produces one world's shared state completes before any view of that
		// world begins. These edges preserve concurrency inside a scope while
		// making `World`, `View`, and `Frame` mean more than labels in the editor.
		for (size_t before = 0; before < count; before++) {
			for (size_t after = 0; after < count; after++) {
				if (ScopeRank(nodes[before]->Scope) < ScopeRank(nodes[after]->Scope)) {
					AddDependency(outgoing, incoming, before, after);
				}
			}
		}

		std::vector<size_t> remaining(count);
		for (size_t index = 0; index < count; index++) {
			remaining[index] = incoming[index].size();
		}

		size_t emitted = 0;
		while (emitted < count) {
			ExecutionWave wave;
			std::vector<size_t> ready;
			for (size_t index = 0; index < count; index++) {
				if (remaining[index] == 0) {
					ready.push_back(index);
					remaining[index] = static_cast<size_t>(-1);
				}
			}

			if (ready.empty()) {
				for (size_t index = 0; index < count; index++) {
					if (remaining[index] != static_cast<size_t>(-1)) {
						offender = nodes[index]->Name;
						break;
					}
				}
				out.Clear();
				return ScheduleStatus::Cycle;
			}

			std::unordered_set<uint8_t> queues;
			bool permitsOverlap = false;
			bool forcesSerial = false;
			for (const size_t index : ready) {
				ScheduledNode scheduled;
				if (!ReadHints(graph, *nodes[index], ids[index], scheduled)) {
					offender = nodes[index]->Name;
					out.Clear();
					return ScheduleStatus::InvalidHint;
				}
				queues.insert(static_cast<uint8_t>(scheduled.Queue));
				permitsOverlap = permitsOverlap || scheduled.AsyncEligible;
				forcesSerial = forcesSerial || scheduled.Async == AsyncPolicy::Serial;
				wave.Nodes.push_back(scheduled);
				for (const size_t next : outgoing[index]) {
					remaining[next]--;
				}
			}
			wave.Concurrent = queues.size() > 1 && permitsOverlap && !forcesSerial;
			emitted += ready.size();
			out.Waves.push_back(std::move(wave));
		}

		offender = core::Name{};
		return ScheduleStatus::Ok;
	}
}
