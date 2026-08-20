#include <engine/graph/PipelineProfile.hpp>

#include <algorithm>
#include <unordered_map>

namespace engine::graph {

	namespace {
		bool Touches(const std::vector<ResourceId> &list, ResourceId resource) {
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
			desc->Resolve(viewWidth, viewHeight, row.Width, row.Height);

			// Bits per pixel rather than bytes per pixel, so a block-compressed
			// format at four bits does not round to nothing.
			row.Bytes = (static_cast<uint64_t>(row.Width) * row.Height * BitsPerPixel(row.Format) + 7) / 8;

			profile.Resources.push_back(row);
		}

		profile.Cells.assign(profile.Resources.size() * profile.Passes.size(), Access::None);

		for (size_t at = 0; at < profile.Resources.size(); at++) {
			ProfileResource &row = profile.Resources[at];
			profile.TotalBytes += row.Bytes;

			for (size_t pass = 0; pass < profile.Passes.size(); pass++) {
				const Node *node = graph.Find(profile.Passes[pass].Node);
				if (node == nullptr) {
					continue;
				}

				const bool reads = Touches(node->Reads, row.Id);
				const bool writes = Touches(node->Writes, row.Id);
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
