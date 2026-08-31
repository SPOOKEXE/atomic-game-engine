#include <engine/graph/PipelineProfile.hpp>

#include <algorithm>
#include <unordered_map>

namespace engine::graph {

	namespace {
		bool TouchesResource(const std::vector<ResourceId> &list, ResourceId resource) {
			return std::find(list.begin(), list.end(), resource) != list.end();
		}

		// Appends one block's nodes as columns.
		void Columns(
			const RenderGraph &graph,
			const std::vector<NodeId> &block,
			Band where,
			std::vector<ProfilePass> &into
		) {
			for (const NodeId id : block) {
				const Node *node = graph.Find(id);
				if (node == nullptr) {
					continue;
				}
				into.push_back(ProfilePass{id, node->Name, node->Kind, where, 0.0, 0.0});
			}
		}

		bool AllocatedTarget(const ResourceDesc &desc) {
			return !desc.External && (desc.Kind == ResourceKind::Colour || desc.Kind == ResourceKind::Depth ||
									  desc.Kind == ResourceKind::Storage);
		}

		bool Compatible(const ResourceDesc &left, const ResourceDesc &right) {
			return left.Kind == right.Kind && left.Format == right.Format && left.Width == right.Width &&
				   left.Height == right.Height && left.Divisor == right.Divisor;
		}

		NodeScope ScopeOf(const RenderGraph &graph, ResourceId resource) {
			NodeScope found = NodeScope::Frame;
			for (uint32_t value = 1; value <= graph.Count(); value++) {
				const Node *node = graph.Find(NodeId{value});
				if (node == nullptr || !TouchesResource(node->Writes, resource)) {
					continue;
				}
				if (node->Scope == NodeScope::View) {
					return NodeScope::View;
				}
				if (node->Scope == NodeScope::World) {
					found = NodeScope::World;
				}
			}
			return found;
		}

		std::vector<NodeId> OrderedNodes(const CompiledGraph &compiled) {
			std::vector<NodeId> ordered;
			ordered.reserve(compiled.Shared.size() + compiled.PerView.size() + compiled.Final.size());
			ordered.insert(ordered.end(), compiled.Shared.begin(), compiled.Shared.end());
			ordered.insert(ordered.end(), compiled.PerView.begin(), compiled.PerView.end());
			ordered.insert(ordered.end(), compiled.Final.begin(), compiled.Final.end());
			return ordered;
		}
	}

	ResourceAliasPlan BuildResourceAliases(const RenderGraph &graph, const CompiledGraph &compiled) {
		ResourceAliasPlan plan;
		plan.Allocations.resize(graph.ResourceCount());
		const std::vector<NodeId> ordered = OrderedNodes(compiled);

		struct Lifetime {
			uint32_t First = ProfileResource::NEVER;
			uint32_t Last = ProfileResource::NEVER;
		};
		std::vector<Lifetime> lifetimes(graph.ResourceCount());
		for (size_t pass = 0; pass < ordered.size(); pass++) {
			const Node *node = graph.Find(ordered[pass]);
			if (node == nullptr) {
				continue;
			}
			for (const ResourceId resource : node->Writes) {
				Lifetime &life = lifetimes[resource.Value - 1];
				if (life.First == ProfileResource::NEVER) {
					life.First = static_cast<uint32_t>(pass);
				}
				life.Last = static_cast<uint32_t>(pass);
			}
			for (const ResourceId resource : node->Reads) {
				Lifetime &life = lifetimes[resource.Value - 1];
				life.Last = static_cast<uint32_t>(pass);
			}
		}

		struct Slot {
			ResourceId Owner;
			uint32_t Last = 0;
		};
		std::vector<Slot> slots;
		for (size_t index = 0; index < graph.ResourceCount(); index++) {
			const ResourceId resource{static_cast<uint32_t>(index + 1)};
			const ResourceDesc *desc = graph.FindResource(resource);
			const Lifetime life = lifetimes[index];
			if (desc == nullptr || !AllocatedTarget(*desc) || life.First == ProfileResource::NEVER) {
				continue;
			}

			auto reusable = std::find_if(slots.begin(), slots.end(), [&](const Slot &slot) {
				const ResourceDesc *owner = graph.FindResource(slot.Owner);
				return slot.Last < life.First && owner != nullptr && Compatible(*owner, *desc) &&
					   ScopeOf(graph, slot.Owner) == ScopeOf(graph, resource);
			});
			if (reusable == slots.end()) {
				plan.Allocations[index] = resource;
				slots.push_back(Slot{resource, life.Last});
				plan.PhysicalTargets++;
				continue;
			}

			plan.Allocations[index] = reusable->Owner;
			reusable->Last = life.Last;
			plan.AliasedResources++;
		}
		return plan;
	}

	const char *Describe(Access access) {
		switch (access) {
		case Access::None:
			return "none";
		case Access::Read:
			return "read";
		case Access::Write:
			return "write";
		case Access::ReadWrite:
			return "read-write";
		}
		return "?";
	}

	PipelineProfile ProfilePipeline(
		const RenderGraph &graph, const CompiledGraph &compiled, uint32_t viewWidth, uint32_t viewHeight
	) {
		PipelineProfile profile;
		const ResourceAliasPlan aliases = BuildResourceAliases(graph, compiled);
		profile.AliasedResources = aliases.AliasedResources;

		// **The three blocks in the order a frame runs them**, which is what
		// `Compile` decided and is not this function's to re-derive. A grid that
		// ordered its own columns would be a second opinion about the frame.
		Columns(graph, compiled.Shared, Band::Shared, profile.Passes);
		Columns(graph, compiled.PerView, Band::PerView, profile.Passes);
		Columns(graph, compiled.Final, Band::Final, profile.Passes);

		if (profile.Passes.empty()) {
			return profile;
		}

		for (size_t index = 0; index < graph.ResourceCount(); index++) {
			const ResourceId id{static_cast<uint32_t>(index + 1)};
			const ResourceDesc *desc = graph.FindResource(id);
			if (desc == nullptr) {
				continue;
			}

			ProfileResource row;
			row.Id = id;
			row.Name = desc->Name;
			row.Kind = desc->Kind;
			row.Format = desc->Format;
			row.External = desc->External;
			row.Allocation = aliases.AllocationOf(id);
			desc->Resolve(viewWidth, viewHeight, row.Width, row.Height);

			// Bits per pixel rather than bytes per pixel, so a block-compressed
			// format at four bits does not round to nothing.
			if (row.Kind != ResourceKind::Camera && row.Kind != ResourceKind::Entities) {
				row.Bytes =
					(static_cast<uint64_t>(row.Width) * row.Height * BitsPerPixel(row.Format) + 7) / 8;
			}

			profile.Resources.push_back(row);
		}

		profile.Cells.assign(profile.Resources.size() * profile.Passes.size(), Access::None);

		for (size_t at = 0; at < profile.Resources.size(); at++) {
			ProfileResource &row = profile.Resources[at];
			profile.TotalBytes += row.Bytes;
			if (row.Allocation == row.Id) {
				profile.AllocatedBytes += row.Bytes;
			}

			for (size_t pass = 0; pass < profile.Passes.size(); pass++) {
				const Node *node = graph.Find(profile.Passes[pass].Node);
				if (node == nullptr) {
					continue;
				}

				const bool reads = TouchesResource(node->Reads, row.Id);
				const bool writes = TouchesResource(node->Writes, row.Id);
				if (!reads && !writes) {
					continue;
				}

				profile.Cells[at * profile.Passes.size() + pass] =
					reads && writes ? Access::ReadWrite : (reads ? Access::Read : Access::Write);

				if (writes && row.FirstWrite == ProfileResource::NEVER) {
					row.FirstWrite = static_cast<uint32_t>(pass);
				}
				if (reads) {
					row.LastRead = static_cast<uint32_t>(pass);
				}
			}
		}

		// **The peak, walked rather than summed.** The sum is what an engine
		// with no transient allocator pays; the peak is what one with a good one
		// pays, and the gap between them is the whole argument for having one.
		for (size_t pass = 0; pass < profile.Passes.size(); pass++) {
			uint64_t live = 0;
			for (const ProfileResource &row : profile.Resources) {
				if (row.LiveAt(static_cast<uint32_t>(pass))) {
					live += row.Bytes;
				}
			}
			profile.PeakBytes = std::max(profile.PeakBytes, live);
		}

		return profile;
	}
}
