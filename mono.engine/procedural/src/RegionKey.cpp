#include "Encoding.hpp"
#include "RegionInternal.hpp"

#include <engine/procedural/Region.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <new>

namespace engine::procedural {

	namespace detail {

		bool Contains(Bounds bounds, Point point) {
			return point.X >= bounds.Min.X && point.X < bounds.Max.X && point.Y >= bounds.Min.Y &&
				   point.Y < bounds.Max.Y;
		}

		Status ValidateQueries(
			const RegionRequest &request, Bounds required, QueryView &out, Diagnostic &diagnostic
		) {
			if (request.Queries.size() != 2 || request.Limits.MaximumCandidateCells > 65536 ||
				request.Limits.MaximumQueryLeaves > 512 || request.Limits.MaximumWorkUnits > 8000000 ||
				request.Limits.MaximumSpatialTests > 4000000 || request.Limits.MaximumPlacements > 4096 ||
				request.Limits.MaximumRoads > 256 || request.Limits.MaximumOutputBytes > 1048576)
				return Refuse(diagnostic, Status::LimitExceeded, "region.limits");
			try {
				QueryView candidate;
				size_t count = 0;
				for (const SourceQuery &query : request.Queries) {
					if (!query.Complete || query.Covered != required)
						return Refuse(diagnostic, Status::InvalidCoverage, query.Source);
					const bool villages = query.Source == "villages";
					const bool entrances = query.Source == "entrances";
					if ((!villages && !entrances) || (villages && candidate.Villages != nullptr) ||
						(entrances && candidate.Entrances != nullptr))
						return Refuse(diagnostic, Status::InvalidCoverage, query.Source);
					if (query.Matches.size() > request.Limits.MaximumQueryLeaves - count)
						return Refuse(diagnostic, Status::LimitExceeded, "region.query_leaves");
					count += query.Matches.size();
					auto &leaves = villages ? candidate.VillageLeaves : candidate.EntranceLeaves;
					for (const SpatialLeaf &leaf : query.Matches) {
						if (leaf.Kind != (villages ? SourceKind::Village : SourceKind::Entrance) ||
							leaf.Name.size() > 128 || !ValidStableName(leaf.Name) ||
							!Contains(required, leaf.Anchor) || (villages && !leaf.TargetVillage.empty()) ||
							(entrances &&
							 (leaf.TargetVillage.size() > 128 || !ValidStableName(leaf.TargetVillage))))
							return Refuse(diagnostic, Status::InvalidInput, leaf.Name);
						leaves.push_back(&leaf);
					}
					std::sort(leaves.begin(), leaves.end(), [](const SpatialLeaf *a, const SpatialLeaf *b) {
						return StableNameLess(a->Name, b->Name);
					});
					for (size_t index = 1; index < leaves.size(); ++index)
						if (leaves[index - 1]->Name == leaves[index]->Name)
							return Refuse(diagnostic, Status::InvalidInput, leaves[index]->Name);
					if (villages)
						candidate.Villages = &query;
					else
						candidate.Entrances = &query;
				}
				if (candidate.Villages == nullptr || candidate.Entrances == nullptr)
					return Refuse(diagnostic, Status::InvalidCoverage, "region.queries");
				out = std::move(candidate);
				diagnostic = {};
				return Status::Ok;
			} catch (const std::bad_alloc &) {
				return Refuse(diagnostic, Status::AllocationFailed);
			}
		}
	}

	Status BuildRegionKey(
		const CompiledRecipe &recipe,
		const RegionRequest &request,
		std::vector<uint8_t> &out,
		Diagnostic &diagnostic
	) {
		Bounds owner;
		Bounds required;
		const Status bounds = RequiredRegionBounds(recipe, request.Scope, owner, required, diagnostic);
		if (bounds != Status::Ok) return bounds;
		detail::QueryView queries;
		const Status coverage = detail::ValidateQueries(request, required, queries, diagnostic);
		if (coverage != Status::Ok) return coverage;
		try {
			detail::Writer writer{{}, std::min<size_t>(262144, request.Limits.MaximumOutputBytes)};
			if (!writer.Raw(std::array<uint8_t, 4>{'R', 'G', 'K', '1'}) ||
				!writer.U32(std::bit_cast<uint32_t>(request.Scope.X)) ||
				!writer.U32(std::bit_cast<uint32_t>(request.Scope.Y)) ||
				!writer.U16(request.Scope.CellsPerSide) || !writer.Raw(request.RecipeDigest.Bytes) ||
				!writer.U32(static_cast<uint32_t>(recipe.CanonicalBytes_.size())) ||
				!writer.Raw(recipe.CanonicalBytes_))
				return detail::Refuse(diagnostic, Status::LimitExceeded, "region.key");
			for (const SourceQuery *query : {queries.Entrances, queries.Villages}) {
				const auto &leaves =
					query == queries.Entrances ? queries.EntranceLeaves : queries.VillageLeaves;
				if (!writer.Text(query->Source) ||
					!writer.U64(std::bit_cast<uint64_t>(query->Covered.Min.X)) ||
					!writer.U64(std::bit_cast<uint64_t>(query->Covered.Min.Y)) ||
					!writer.U64(std::bit_cast<uint64_t>(query->Covered.Max.X)) ||
					!writer.U64(std::bit_cast<uint64_t>(query->Covered.Max.Y)) ||
					!writer.Raw(query->CoverageDigest.Bytes) ||
					!writer.U16(static_cast<uint16_t>(leaves.size())))
					return detail::Refuse(diagnostic, Status::LimitExceeded, "region.key");
				for (const SpatialLeaf *leaf : leaves) {
					if (!writer.Text(leaf->Name) || !writer.U8(static_cast<uint8_t>(leaf->Kind)) ||
						!writer.U64(std::bit_cast<uint64_t>(leaf->Anchor.X)) ||
						!writer.U64(std::bit_cast<uint64_t>(leaf->Anchor.Y)) ||
						!writer.Text(leaf->TargetVillage) || !writer.Raw(leaf->ContentDigest.Bytes))
						return detail::Refuse(diagnostic, Status::LimitExceeded, "region.key");
				}
			}
			out = std::move(writer.Bytes);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::Refuse(diagnostic, Status::AllocationFailed);
		}
	}
}
