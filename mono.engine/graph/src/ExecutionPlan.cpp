#include <engine/graph/ExecutionPlan.hpp>

#include <algorithm>
#include <unordered_map>

namespace engine::graph {

	const char *Describe(ExecutionPlanStatus status) {
		switch (status) {
		case ExecutionPlanStatus::Ok:
			return "ok";
		case ExecutionPlanStatus::InvalidDimensions:
			return "the view dimensions are zero";
		case ExecutionPlanStatus::MissingNode:
			return "the schedule names a missing node";
		case ExecutionPlanStatus::MissingResource:
			return "a node names a missing resource";
		case ExecutionPlanStatus::InvalidScopeAccess:
			return "a node cannot access this resource scope";
		}
		return "unknown";
	}

	namespace {
		enum class ResourceDomain : uint8_t {
			World,
			View,
			Frame,
		};

		struct ResourceInstance {
			ResourceId Resource;
			ResourceDomain Domain = ResourceDomain::Frame;
			size_t Ordinal = 0;

			bool operator==(const ResourceInstance &other) const = default;
		};

		struct ResourceInstanceHash {
			size_t operator()(const ResourceInstance &instance) const {
				return (static_cast<size_t>(instance.Resource.Value) << 2) ^
					   (static_cast<size_t>(instance.Domain) << 1) ^ (instance.Ordinal * 0x9e3779b9u);
			}
		};

		struct Owner {
			NodeId Node;
			ExecutionQueue Queue = ExecutionQueue::Graphics;
		};

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

		ResourceDomain DomainOf(NodeScope scope) {
			switch (scope) {
			case NodeScope::World:
				return ResourceDomain::World;
			case NodeScope::View:
				return ResourceDomain::View;
			case NodeScope::Frame:
				return ResourceDomain::Frame;
			}
			return ResourceDomain::Frame;
		}

		bool Has(std::span<const ResourceId> resources, ResourceId resource) {
			return std::find(resources.begin(), resources.end(), resource) != resources.end();
		}

		uint64_t BytesOf(const ResourceDesc &resource, uint32_t width, uint32_t height) {
			if (resource.Kind == ResourceKind::Camera || resource.Kind == ResourceKind::Entities) {
				return 0;
			}
			uint32_t resolvedWidth = 0;
			uint32_t resolvedHeight = 0;
			resource.Resolve(width, height, resolvedWidth, resolvedHeight);
			return (static_cast<uint64_t>(resolvedWidth) * resolvedHeight * BitsPerPixel(resource.Format) +
					7) /
				   8;
		}

		std::vector<size_t> InstancesFor(
			ResourceDomain domain, const PlannedInvocation &invocation, size_t worldCount, size_t viewCount
		) {
			switch (domain) {
			case ResourceDomain::World:
				if (invocation.Scope == NodeScope::Frame) {
					std::vector<size_t> instances(worldCount);
					for (size_t index = 0; index < worldCount; index++) {
						instances[index] = index;
					}
					return instances;
				}
				return {invocation.World};
			case ResourceDomain::View:
				if (invocation.Scope == NodeScope::Frame) {
					std::vector<size_t> instances(viewCount);
					for (size_t index = 0; index < viewCount; index++) {
						instances[index] = index;
					}
					return instances;
				}
				return {invocation.View};
			case ResourceDomain::Frame:
				return {0};
			}
			return {};
		}
	}

	ExecutionPlanStatus PlanFrame(
		const RenderGraph &graph,
		const ExecutionSchedule &schedule,
		std::span<const uint64_t> worlds,
		uint32_t width,
		uint32_t height,
		FrameExecutionPlan &out,
		core::Name &offender
	) {
		out.Clear();
		if (width == 0 || height == 0) {
			offender = core::Name{};
			return ExecutionPlanStatus::InvalidDimensions;
		}

		std::vector<uint64_t> distinctWorlds;
		std::vector<size_t> viewWorlds;
		for (const uint64_t key : worlds) {
			auto found = std::find(distinctWorlds.begin(), distinctWorlds.end(), key);
			if (found == distinctWorlds.end()) {
				distinctWorlds.push_back(key);
				found = distinctWorlds.end() - 1;
			}
			viewWorlds.push_back(static_cast<size_t>(found - distinctWorlds.begin()));
		}
		if (distinctWorlds.empty()) {
			distinctWorlds.push_back(0);
		}

		std::vector<ResourceDomain> domains(graph.ResourceCount(), ResourceDomain::Frame);
		for (uint32_t value = 1; value <= graph.ResourceCount(); value++) {
			const ResourceId resource{value};
			bool found = false;
			NodeScope narrowest = NodeScope::Frame;
			for (uint32_t nodeValue = 1; nodeValue <= graph.Count(); nodeValue++) {
				const Node *node = graph.Find(NodeId{nodeValue});
				if (node == nullptr || !node->Enabled || !Has(node->Writes, resource)) {
					continue;
				}
				if (!found || ScopeRank(node->Scope) < ScopeRank(narrowest)) {
					narrowest = node->Scope;
					found = true;
				}
			}
			if (!found) {
				for (uint32_t nodeValue = 1; nodeValue <= graph.Count(); nodeValue++) {
					const Node *node = graph.Find(NodeId{nodeValue});
					if (node != nullptr && node->Enabled && Has(node->Reads, resource) &&
						(!found || ScopeRank(node->Scope) < ScopeRank(narrowest))) {
						narrowest = node->Scope;
						found = true;
					}
				}
			}
			domains[value - 1] = DomainOf(narrowest);
		}

		for (const ExecutionWave &wave : schedule.Waves) {
			PlannedWave planned;
			planned.ConcurrentQueues = wave.Concurrent;
			for (const ScheduledNode &scheduled : wave.Nodes) {
				const Node *node = graph.Find(scheduled.Node);
				if (node == nullptr || !node->Enabled) {
					offender = node == nullptr ? core::Name{} : node->Name;
					out.Clear();
					return ExecutionPlanStatus::MissingNode;
				}

				const auto append = [&](size_t view, size_t world, uint64_t worldKey) {
					PlannedInvocation invocation;
					invocation.Scheduled = scheduled;
					invocation.Scope = node->Scope;
					invocation.View = view;
					invocation.World = world;
					invocation.WorldKey = worldKey;
					planned.Invocations.push_back(invocation);
				};

				switch (node->Scope) {
				case NodeScope::World:
					for (size_t world = 0; world < distinctWorlds.size(); world++) {
						append(RunContext::WHOLE_FRAME, world, distinctWorlds[world]);
					}
					planned.IndependentWorlds = distinctWorlds.size() > 1;
					break;
				case NodeScope::View:
					for (size_t view = 0; view < worlds.size(); view++) {
						append(view, viewWorlds[view], worlds[view]);
					}
					planned.IndependentViews = worlds.size() > 1;
					planned.IndependentWorlds = distinctWorlds.size() > 1;
					break;
				case NodeScope::Frame:
					append(RunContext::WHOLE_FRAME, RunContext::WHOLE_FRAME, 0);
					break;
				}
			}
			out.Waves.push_back(std::move(planned));
		}

		std::unordered_map<ResourceInstance, Owner, ResourceInstanceHash> owners;
		for (PlannedWave &wave : out.Waves) {
			for (PlannedInvocation &invocation : wave.Invocations) {
				const Node *node = graph.Find(invocation.Scheduled.Node);
				const auto access = [&](std::span<const ResourceId> resources, bool write) {
					for (const ResourceId id : resources) {
						const ResourceDesc *resource = graph.FindResource(id);
						if (resource == nullptr) {
							offender = node->Name;
							return ExecutionPlanStatus::MissingResource;
						}
						const ResourceDomain domain = domains[id.Value - 1];
						if ((domain == ResourceDomain::View && invocation.Scope == NodeScope::World) ||
							(domain == ResourceDomain::Frame && invocation.Scope != NodeScope::Frame)) {
							offender = resource->Name;
							return ExecutionPlanStatus::InvalidScopeAccess;
						}

						const uint64_t bytes = BytesOf(*resource, width, height);
						const std::vector<size_t> instances =
							InstancesFor(domain, invocation, distinctWorlds.size(), worlds.size());
						if (write) {
							invocation.WriteBytes += bytes * instances.size();
						} else {
							invocation.ReadBytes += bytes * instances.size();
						}

						for (const size_t ordinal : instances) {
							const ResourceInstance instance{id, domain, ordinal};
							const auto owner = owners.find(instance);
							if (owner != owners.end() && owner->second.Queue != invocation.Scheduled.Queue) {
								out.Transfers.push_back({
									.Resource = id,
									.Producer = owner->second.Node,
									.Consumer = invocation.Scheduled.Node,
									.From = owner->second.Queue,
									.To = invocation.Scheduled.Queue,
									.View =
										domain == ResourceDomain::View ? ordinal : RunContext::WHOLE_FRAME,
									.World = domain == ResourceDomain::World ? ordinal : invocation.World,
									.Bytes = bytes,
								});
								out.QueueTransferBytes += bytes;
							}
							owners[instance] = {invocation.Scheduled.Node, invocation.Scheduled.Queue};
						}
					}
					return ExecutionPlanStatus::Ok;
				};

				if (const auto status = access(node->Reads, false); status != ExecutionPlanStatus::Ok) {
					out.Clear();
					return status;
				}
				if (const auto status = access(node->Writes, true); status != ExecutionPlanStatus::Ok) {
					out.Clear();
					return status;
				}
				out.ReadBytes += invocation.ReadBytes;
				out.WriteBytes += invocation.WriteBytes;
			}
		}

		offender = core::Name{};
		return ExecutionPlanStatus::Ok;
	}
}
