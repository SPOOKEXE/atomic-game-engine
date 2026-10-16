#include "ProceduralState.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace engine::script {

	namespace procedural_source_detail {
		using namespace procedural_detail;

		const ProceduralAuthority *Authority(const ecs::Store &store) {
			return ecs::Components::Assigned<ProceduralAuthority>().IsValid()
					   ? store.Resource<ProceduralAuthority>()
					   : nullptr;
		}

		bool MayEdit(const ecs::Store &store, procedural::Diagnostic &diagnostic) {
			if (!store.IsOnOwningThread() || store.AdoptOnly())
				return Refuse(diagnostic, procedural::Status::InvalidInput, "authority.replica");
			if (!ecs::Components::Assigned<ProceduralAuthority>().IsValid() ||
				!ecs::Components::Assigned<ProceduralPublished>().IsValid())
				return Refuse(diagnostic, procedural::Status::InvalidInput, "authority.unregistered");
			return true;
		}

		bool ReplaceAuthority(
			ecs::Store &store, ProceduralAuthority candidate, procedural::Diagnostic &diagnostic
		) {
			std::vector<uint8_t> checked;
			if (!WriteAuthority(candidate, checked, diagnostic)) return false;
			if (!store.HasResource<ProceduralPublished>()) store.SetResource(ProceduralPublished{});
			if (auto *current = store.ResourceMutable<ProceduralAuthority>()) {
				static_assert(std::is_nothrow_swappable_v<ProceduralAuthority>);
				std::swap(*current, candidate);
			} else {
				store.SetResource(candidate);
			}
			diagnostic = {};
			return true;
		}

		bool Invalidate(
			ProceduralAuthority &authority,
			const procedural::CompiledRecipe &compiled,
			const ProceduralSource *previous,
			const ProceduralSource *next,
			procedural::Diagnostic &diagnostic
		) {
			for (auto &region : authority.Regions) {
				procedural::Bounds owner;
				procedural::Bounds query;
				if (procedural::RequiredRegionBounds(compiled, region.Scope, owner, query, diagnostic) !=
					procedural::Status::Ok)
					return false;
				if ((previous && Contains(query, previous->Anchor)) ||
					(next && Contains(query, next->Anchor))) {
					if (region.SourceRevision == std::numeric_limits<uint64_t>::max())
						return Refuse(diagnostic, procedural::Status::LimitExceeded, "authority.revision");
					++region.SourceRevision;
				}
			}
			return true;
		}

		procedural::SpatialLeaf CaptureLeaf(const ProceduralSource &source) {
			core::ByteWriter writer(0, 1024);
			writer.WriteString("procedural.source.v1");
			writer.WriteString(source.Kind == procedural::SourceKind::Village ? "village" : "entrance");
			writer.WriteString(source.Name);
			writer.WriteInt64(source.Anchor.X);
			writer.WriteInt64(source.Anchor.Y);
			writer.WriteString(source.TargetVillage);
			const auto raw = writer.Bytes();
			return {
				source.Kind,
				source.Name,
				source.Anchor,
				source.TargetVillage,
				Hash(std::span(reinterpret_cast<const uint8_t *>(raw.data()), raw.size()))
			};
		}

		procedural::Digest Coverage(const procedural::SourceQuery &query) {
			core::ByteWriter writer(0, ProceduralHostLimits::MaximumAuthorityBytes);
			writer.WriteString("procedural.coverage.v1");
			writer.WriteString(query.Source);
			writer.WriteInt64(query.Covered.Min.X);
			writer.WriteInt64(query.Covered.Min.Y);
			writer.WriteInt64(query.Covered.Max.X);
			writer.WriteInt64(query.Covered.Max.Y);
			writer.WriteUInt16(static_cast<uint16_t>(query.Matches.size()));
			for (const auto &leaf : query.Matches) {
				writer.WriteString(leaf.Name);
				writer.WriteRaw(leaf.ContentDigest.Bytes.data(), leaf.ContentDigest.Bytes.size());
			}
			const auto raw = writer.Bytes();
			return Hash(std::span(reinterpret_cast<const uint8_t *>(raw.data()), raw.size()));
		}
	}

	bool ConfigureProceduralWorld(
		ecs::Store &store,
		std::string_view owner,
		const procedural::Recipe &recipe,
		std::span<const procedural::RegionScope> regions,
		std::span<const ProceduralSource> sources,
		procedural::Diagnostic &diagnostic
	) {
		using namespace procedural_detail;
		using namespace procedural_source_detail;
		ENGINE_PROFILE("Procedural::Configure");
		if (!MayEdit(store, diagnostic)) return false;
		if (owner.size() > 128 || !procedural::ValidStableName(owner) ||
			regions.size() > ProceduralHostLimits::MaximumRegions ||
			sources.size() > ProceduralHostLimits::MaximumSources)
			return Refuse(diagnostic, procedural::Status::LimitExceeded, "authority.limits");
		for (const auto &source : sources)
			if (!ValidSource(source))
				return Refuse(diagnostic, procedural::Status::InvalidInput, "authority.source");
		try {
			ProceduralAuthority candidate;
			candidate.Owner.assign(owner);
			procedural::CompiledRecipe compiled;
			if (procedural::CompileForestVillage(recipe, compiled, diagnostic) != procedural::Status::Ok)
				return false;
			candidate.RecipeBytes = compiled.CanonicalBytes();
			candidate.Sources.assign(sources.begin(), sources.end());
			std::sort(candidate.Sources.begin(), candidate.Sources.end(), SourceLess);
			candidate.Regions.reserve(regions.size());
			for (const auto scope : regions)
				candidate.Regions.push_back({scope, 1});
			std::sort(candidate.Regions.begin(), candidate.Regions.end(), [](const auto &a, const auto &b) {
				return ScopeLess(a.Scope, b.Scope);
			});
			candidate.Incarnation = NextIncarnation();
			return ReplaceAuthority(store, std::move(candidate), diagnostic);
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, procedural::Status::AllocationFailed, "authority.configure");
		}
	}

	bool SetProceduralRecipe(
		ecs::Store &store, const procedural::Recipe &recipe, procedural::Diagnostic &diagnostic
	) {
		using namespace procedural_detail;
		using namespace procedural_source_detail;
		if (!MayEdit(store, diagnostic)) return false;
		const auto *current = Authority(store);
		if (!current) return Refuse(diagnostic, procedural::Status::MissingSource, "authority");
		try {
			procedural::CompiledRecipe compiled;
			if (procedural::CompileForestVillage(recipe, compiled, diagnostic) != procedural::Status::Ok)
				return false;
			if (current->RecipeBytes == compiled.CanonicalBytes()) {
				diagnostic = {};
				return true;
			}
			ProceduralAuthority candidate = *current;
			candidate.RecipeBytes = compiled.CanonicalBytes();
			for (auto &region : candidate.Regions) {
				if (region.SourceRevision == std::numeric_limits<uint64_t>::max())
					return Refuse(diagnostic, procedural::Status::LimitExceeded, "authority.revision");
				++region.SourceRevision;
			}
			return ReplaceAuthority(store, std::move(candidate), diagnostic);
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, procedural::Status::AllocationFailed, "authority.recipe");
		}
	}

	bool SetProceduralSource(
		ecs::Store &store, const ProceduralSource &source, procedural::Diagnostic &diagnostic
	) {
		using namespace procedural_detail;
		using namespace procedural_source_detail;
		if (!MayEdit(store, diagnostic)) return false;
		if (!ValidSource(source))
			return Refuse(diagnostic, procedural::Status::InvalidInput, "authority.source");
		const auto *current = Authority(store);
		if (!current) return Refuse(diagnostic, procedural::Status::MissingSource, "authority");
		try {
			procedural::CompiledRecipe compiled;
			if (!CompileAuthority(*current, compiled, diagnostic)) return false;
			ProceduralAuthority candidate = *current;
			auto found =
				std::lower_bound(candidate.Sources.begin(), candidate.Sources.end(), source, SourceLess);
			const bool existing =
				found != candidate.Sources.end() && found->Kind == source.Kind && found->Name == source.Name;
			if (existing && found->Anchor == source.Anchor && found->TargetVillage == source.TargetVillage) {
				diagnostic = {};
				return true;
			}
			if (!existing && candidate.Sources.size() >= ProceduralHostLimits::MaximumSources)
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "authority.sources");
			if (!Invalidate(candidate, compiled, existing ? &*found : nullptr, &source, diagnostic))
				return false;
			if (existing)
				*found = source;
			else
				candidate.Sources.insert(found, source);
			return ReplaceAuthority(store, std::move(candidate), diagnostic);
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, procedural::Status::AllocationFailed, "authority.source");
		}
	}

	bool RemoveProceduralSource(
		ecs::Store &store,
		procedural::SourceKind kind,
		std::string_view name,
		procedural::Diagnostic &diagnostic
	) {
		using namespace procedural_detail;
		using namespace procedural_source_detail;
		if (!MayEdit(store, diagnostic)) return false;
		const auto *current = Authority(store);
		if (!current) return Refuse(diagnostic, procedural::Status::MissingSource, "authority");
		try {
			procedural::CompiledRecipe compiled;
			if (!CompileAuthority(*current, compiled, diagnostic)) return false;
			ProceduralAuthority candidate = *current;
			const auto found =
				std::find_if(candidate.Sources.begin(), candidate.Sources.end(), [&](const auto &source) {
					return source.Kind == kind && source.Name == name;
				});
			if (found == candidate.Sources.end())
				return Refuse(diagnostic, procedural::Status::MissingSource, "authority.source");
			if (!Invalidate(candidate, compiled, &*found, nullptr, diagnostic)) return false;
			candidate.Sources.erase(found);
			return ReplaceAuthority(store, std::move(candidate), diagnostic);
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, procedural::Status::AllocationFailed, "authority.source");
		}
	}

	bool CaptureProceduralRegion(
		const ecs::Store &store,
		procedural::RegionScope scope,
		procedural::CompiledRecipe &recipe,
		procedural::RegionRequest &request,
		procedural::Diagnostic &diagnostic
	) {
		using namespace procedural_detail;
		using namespace procedural_source_detail;
		ENGINE_PROFILE("Procedural::Capture");
		if (!store.IsOnOwningThread())
			return Refuse(diagnostic, procedural::Status::InvalidInput, "request.owner");
		const auto *authority = Authority(store);
		if (!authority) return Refuse(diagnostic, procedural::Status::MissingSource, "authority");
		try {
			procedural::CompiledRecipe compiled;
			if (!CompileAuthority(*authority, compiled, diagnostic)) return false;
			const auto region =
				std::find_if(authority->Regions.begin(), authority->Regions.end(), [&](const auto &value) {
					return SameScope(value.Scope, scope);
				});
			if (region == authority->Regions.end())
				return Refuse(diagnostic, procedural::Status::InvalidInput, "region.unregistered");
			procedural::Bounds owner;
			procedural::Bounds queryBounds;
			if (procedural::RequiredRegionBounds(compiled, scope, owner, queryBounds, diagnostic) !=
				procedural::Status::Ok)
				return false;
			procedural::RegionRequest captured;
			captured.Scope = scope;
			captured.RecipeDigest = Hash(compiled.CanonicalBytes());
			captured.ExpectedSourceRevision = region->SourceRevision;
			size_t visits = 0;
			for (const auto kind : {procedural::SourceKind::Village, procedural::SourceKind::Entrance}) {
				procedural::SourceQuery query;
				query.Source = kind == procedural::SourceKind::Village ? "villages" : "entrances";
				query.Covered = queryBounds;
				for (const auto &source : authority->Sources) {
					if (++visits > ProceduralHostLimits::MaximumQueryVisits)
						return Refuse(diagnostic, procedural::Status::LimitExceeded, "region.query_visits");
					if (source.Kind == kind && Contains(queryBounds, source.Anchor))
						query.Matches.push_back(CaptureLeaf(source));
				}
				query.Complete = true;
				query.CoverageDigest = Coverage(query);
				captured.Queries.push_back(std::move(query));
			}
			std::vector<uint8_t> checkedKey;
			if (procedural::BuildRegionKey(compiled, captured, checkedKey, diagnostic) !=
				procedural::Status::Ok)
				return false;
			recipe = std::move(compiled);
			request = std::move(captured);
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, procedural::Status::AllocationFailed, "region.capture");
		}
	}

	bool EncodeProceduralAuthority(
		const ecs::Store &store, std::vector<uint8_t> &out, procedural::Diagnostic &diagnostic
	) {
		const auto *authority = procedural_source_detail::Authority(store);
		if (!authority)
			return procedural_detail::Refuse(diagnostic, procedural::Status::MissingSource, "authority");
		return procedural_detail::WriteAuthority(*authority, out, diagnostic);
	}

	bool DecodeProceduralAuthority(
		ecs::Store &store, std::span<const uint8_t> bytes, procedural::Diagnostic &diagnostic
	) {
		if (!procedural_source_detail::MayEdit(store, diagnostic)) return false;
		try {
			ProceduralAuthority authority;
			if (!procedural_detail::ReadAuthority(bytes, authority, diagnostic)) return false;
			return procedural_source_detail::ReplaceAuthority(store, std::move(authority), diagnostic);
		} catch (const std::bad_alloc &) {
			return procedural_detail::Refuse(
				diagnostic, procedural::Status::AllocationFailed, "authority.import"
			);
		}
	}
}
