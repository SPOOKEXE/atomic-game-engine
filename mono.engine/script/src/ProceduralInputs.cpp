#include "ProceduralState.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/ProceduralInputs.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace engine::script {
	namespace procedural_input_detail {
		using namespace procedural_detail;

		bool Intersects(procedural::Bounds left, procedural::Bounds right) {
			return left.Min.X < right.Max.X && right.Min.X < left.Max.X && left.Min.Y < right.Max.Y &&
				   right.Min.Y < left.Max.Y;
		}

		const ProceduralSource *
		FindSource(const ProceduralAuthority &authority, procedural::SourceKind kind, std::string_view name) {
			const auto found =
				std::find_if(authority.Sources.begin(), authority.Sources.end(), [&](const auto &source) {
					return source.Kind == kind && source.Name == name;
				});
			return found == authority.Sources.end() ? nullptr : &*found;
		}

		struct SplineShape {
			procedural::Point Entrance{};
			procedural::Point Village{};
			procedural::Bounds Influence{};
		};

		bool ShapeOf(
			const ProceduralAuthority &authority,
			const procedural::CompiledRecipe &recipe,
			const procedural::AuthoredSpline &spline,
			SplineShape &out,
			procedural::Diagnostic &diagnostic
		) {
			const auto *entrance =
				FindSource(authority, procedural::SourceKind::Entrance, spline.EntranceName);
			if (!entrance) return Refuse(diagnostic, procedural::Status::MissingSource, "authored.entrance");
			const auto *village =
				FindSource(authority, procedural::SourceKind::Village, entrance->TargetVillage);
			if (!village) return Refuse(diagnostic, procedural::Status::MissingSource, "authored.village");
			out.Entrance = entrance->Anchor;
			out.Village = village->Anchor;
			return procedural::SplineInfluence(
					   recipe, spline, out.Entrance, out.Village, out.Influence, diagnostic
				   ) == procedural::Status::Ok;
		}

		bool Same(const procedural::AuthoredSpline &left, const procedural::AuthoredSpline &right) {
			return left.Name == right.Name && left.EntranceName == right.EntranceName &&
				   left.Control == right.Control;
		}

		bool Same(const procedural::PlacementOverride &left, const procedural::PlacementOverride &right) {
			return left.TargetId == right.TargetId && left.Action == right.Action &&
				   left.Position == right.Position &&
				   left.RotationQuarterTurns == right.RotationQuarterTurns && left.Variant == right.Variant &&
				   left.Material == right.Material && left.SourceName == right.SourceName;
		}

		bool SourceChangedIn(
			const ProceduralAuthority &previous,
			const ProceduralAuthority &candidate,
			procedural::Bounds query
		) {
			for (const auto &source : previous.Sources) {
				const auto *next = FindSource(candidate, source.Kind, source.Name);
				if ((!next || next->Anchor != source.Anchor || next->TargetVillage != source.TargetVillage) &&
					Contains(query, source.Anchor))
					return true;
			}
			for (const auto &source : candidate.Sources) {
				const auto *old = FindSource(previous, source.Kind, source.Name);
				if ((!old || old->Anchor != source.Anchor || old->TargetVillage != source.TargetVillage) &&
					Contains(query, source.Anchor))
					return true;
			}
			return false;
		}

		bool SplineChangedIn(
			const ProceduralAuthority &previous,
			const ProceduralAuthority &candidate,
			const procedural::CompiledRecipe &recipe,
			procedural::Bounds owner,
			procedural::Diagnostic &diagnostic
		) {
			for (const auto &spline : previous.Splines) {
				const auto next =
					std::find_if(candidate.Splines.begin(), candidate.Splines.end(), [&](const auto &value) {
						return value.Name == spline.Name;
					});
				SplineShape oldShape;
				if (!ShapeOf(previous, recipe, spline, oldShape, diagnostic)) return false;
				if (next == candidate.Splines.end()) {
					if (Intersects(oldShape.Influence, owner)) return true;
					continue;
				}
				SplineShape newShape;
				if (!ShapeOf(candidate, recipe, *next, newShape, diagnostic)) return false;
				if ((!Same(spline, *next) || oldShape.Entrance != newShape.Entrance ||
					 oldShape.Village != newShape.Village) &&
					(Intersects(oldShape.Influence, owner) || Intersects(newShape.Influence, owner)))
					return true;
			}
			for (const auto &spline : candidate.Splines) {
				const auto old =
					std::find_if(previous.Splines.begin(), previous.Splines.end(), [&](const auto &value) {
						return value.Name == spline.Name;
					});
				if (old != previous.Splines.end()) continue;
				SplineShape newShape;
				if (!ShapeOf(candidate, recipe, spline, newShape, diagnostic)) return false;
				if (Intersects(newShape.Influence, owner)) return true;
			}
			return false;
		}

		bool OverrideChangedIn(
			const ProceduralAuthority &previous,
			const ProceduralAuthority &candidate,
			procedural::Bounds owner,
			procedural::Diagnostic &diagnostic
		) {
			for (const auto &value : previous.Overrides) {
				const auto next = std::find_if(
					candidate.Overrides.begin(), candidate.Overrides.end(), [&](const auto &row) {
						return row.TargetId == value.TargetId;
					}
				);
				if (next != candidate.Overrides.end() && Same(value, *next)) continue;
				procedural::Bounds home, destination;
				if (procedural::OverrideInfluence(value, home, destination, diagnostic) !=
					procedural::Status::Ok)
					return false;
				if (Intersects(home, owner) || Intersects(destination, owner)) return true;
			}
			for (const auto &value : candidate.Overrides) {
				const auto old =
					std::find_if(previous.Overrides.begin(), previous.Overrides.end(), [&](const auto &row) {
						return row.TargetId == value.TargetId;
					});
				if (old != previous.Overrides.end() && Same(*old, value)) continue;
				procedural::Bounds home, destination;
				if (procedural::OverrideInfluence(value, home, destination, diagnostic) !=
					procedural::Status::Ok)
					return false;
				if (Intersects(home, owner) || Intersects(destination, owner)) return true;
			}
			return false;
		}
	}

	namespace procedural_detail {
		bool ValidateAuthoredFacts(
			const ProceduralAuthority &authority,
			const procedural::CompiledRecipe &recipe,
			procedural::Diagnostic &diagnostic
		) {
			using namespace procedural_input_detail;
			if (authority.Sources.size() + authority.Splines.size() + authority.Overrides.size() >
				ProceduralHostLimits::MaximumFacts)
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "authority.facts");
			try {
				std::vector<uint8_t> encoded;
				for (size_t index = 0; index < authority.Splines.size(); ++index) {
					const auto &spline = authority.Splines[index];
					if (index && !procedural::StableNameLess(authority.Splines[index - 1].Name, spline.Name))
						return Refuse(diagnostic, procedural::Status::InvalidInput, "authority.splines");
					for (size_t earlier = 0; earlier < index; ++earlier)
						if (authority.Splines[earlier].EntranceName == spline.EntranceName)
							return Refuse(
								diagnostic, procedural::Status::InvalidInput, "authority.spline_entrance"
							);
					if (procedural::EncodeAuthoredSpline(spline, encoded, diagnostic) !=
						procedural::Status::Ok)
						return false;
					SplineShape shape;
					if (!ShapeOf(authority, recipe, spline, shape, diagnostic)) return false;
				}
				for (size_t index = 0; index < authority.Overrides.size(); ++index) {
					const auto &value = authority.Overrides[index];
					if (index &&
						!procedural::StableNameLess(authority.Overrides[index - 1].TargetId, value.TargetId))
						return Refuse(diagnostic, procedural::Status::InvalidInput, "authority.overrides");
					if (procedural::ValidatePlacementOverride(recipe, value, diagnostic) !=
							procedural::Status::Ok ||
						procedural::EncodePlacementOverride(value, encoded, diagnostic) !=
							procedural::Status::Ok)
						return false;
				}
				diagnostic = {};
				return true;
			} catch (const std::bad_alloc &) {
				return Refuse(diagnostic, procedural::Status::AllocationFailed, "authority.facts");
			}
		}

		bool InvalidateChangedFacts(
			const ProceduralAuthority &previous,
			ProceduralAuthority &candidate,
			const procedural::CompiledRecipe &recipe,
			procedural::Diagnostic &diagnostic
		) {
			using namespace procedural_input_detail;
			for (auto &region : candidate.Regions) {
				procedural::Bounds owner, query;
				if (procedural::RequiredRegionBounds(recipe, region.Scope, owner, query, diagnostic) !=
					procedural::Status::Ok)
					return false;
				const bool changed = SourceChangedIn(previous, candidate, query) ||
									 SplineChangedIn(previous, candidate, recipe, owner, diagnostic) ||
									 OverrideChangedIn(previous, candidate, owner, diagnostic);
				if (!changed) {
					if (diagnostic.Code != procedural::Status::Ok) return false;
					continue;
				}
				if (region.SourceRevision == std::numeric_limits<uint64_t>::max())
					return Refuse(diagnostic, procedural::Status::LimitExceeded, "authority.revision");
				++region.SourceRevision;
			}
			diagnostic = {};
			return true;
		}

		bool CaptureAuthoredFacts(
			const ProceduralAuthority &authority,
			const procedural::CompiledRecipe &recipe,
			procedural::Bounds owner,
			procedural::Bounds query,
			procedural::AuthoredInputQuery &out,
			procedural::Diagnostic &diagnostic
		) {
			using namespace procedural_input_detail;
			try {
				procedural::AuthoredInputQuery captured;
				captured.Covered = query;
				std::vector<uint8_t> encoded;
				for (const auto &spline : authority.Splines) {
					SplineShape shape;
					if (!ShapeOf(authority, recipe, spline, shape, diagnostic)) return false;
					if (!Intersects(shape.Influence, owner)) continue;
					if (procedural::EncodeAuthoredSpline(spline, encoded, diagnostic) !=
						procedural::Status::Ok)
						return false;
					captured.Splines.push_back({spline, Hash(encoded)});
				}
				for (const auto &value : authority.Overrides) {
					procedural::Bounds home, destination;
					if (procedural::OverrideInfluence(value, home, destination, diagnostic) !=
						procedural::Status::Ok)
						return false;
					if (!Intersects(home, owner) && !Intersects(destination, owner)) continue;
					if (procedural::EncodePlacementOverride(value, encoded, diagnostic) !=
						procedural::Status::Ok)
						return false;
					captured.Overrides.push_back({value, Hash(encoded)});
				}
				core::ByteWriter writer(0, ProceduralHostLimits::MaximumAuthorityBytes);
				writer.WriteString("procedural.authored.coverage.v1");
				writer.WriteInt64(query.Min.X);
				writer.WriteInt64(query.Min.Y);
				writer.WriteInt64(query.Max.X);
				writer.WriteInt64(query.Max.Y);
				writer.WriteUInt16(static_cast<uint16_t>(captured.Splines.size()));
				for (const auto &fact : captured.Splines) {
					writer.WriteString(fact.Value.Name);
					writer.WriteRaw(fact.ContentDigest.Bytes.data(), fact.ContentDigest.Bytes.size());
				}
				writer.WriteUInt16(static_cast<uint16_t>(captured.Overrides.size()));
				for (const auto &fact : captured.Overrides) {
					writer.WriteString(fact.Value.TargetId);
					writer.WriteRaw(fact.ContentDigest.Bytes.data(), fact.ContentDigest.Bytes.size());
				}
				const auto raw = writer.Bytes();
				captured.CoverageDigest =
					Hash(std::span(reinterpret_cast<const uint8_t *>(raw.data()), raw.size()));
				captured.Complete = true;
				out = std::move(captured);
				diagnostic = {};
				return true;
			} catch (const std::bad_alloc &) {
				return Refuse(diagnostic, procedural::Status::AllocationFailed, "authored.capture");
			} catch (const std::length_error &) {
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "authored.capture");
			}
		}
	}

	bool SetProceduralSpline(
		ecs::Store &store, const procedural::AuthoredSpline &spline, procedural::Diagnostic &diagnostic
	) {
		using namespace procedural_detail;
		if (!procedural_source_detail::MayEdit(store, diagnostic)) return false;
		const auto *current = procedural_source_detail::Authority(store);
		if (!current) return Refuse(diagnostic, procedural::Status::MissingSource, "authority");
		try {
			procedural::CompiledRecipe recipe;
			if (!CompileAuthority(*current, recipe, diagnostic)) return false;
			ProceduralAuthority candidate = *current;
			auto found = std::lower_bound(
				candidate.Splines.begin(),
				candidate.Splines.end(),
				spline.Name,
				[](const auto &row, std::string_view name) {
					return procedural::StableNameLess(row.Name, name);
				}
			);
			if (found != candidate.Splines.end() && found->Name == spline.Name)
				*found = spline;
			else
				candidate.Splines.insert(found, spline);
			if (!ValidateAuthoredFacts(candidate, recipe, diagnostic) ||
				!InvalidateChangedFacts(*current, candidate, recipe, diagnostic))
				return false;
			return procedural_source_detail::ReplaceAuthority(store, std::move(candidate), diagnostic);
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, procedural::Status::AllocationFailed, "authored.spline");
		}
	}

	bool
	RemoveProceduralSpline(ecs::Store &store, std::string_view name, procedural::Diagnostic &diagnostic) {
		using namespace procedural_detail;
		if (!procedural_source_detail::MayEdit(store, diagnostic)) return false;
		const auto *current = procedural_source_detail::Authority(store);
		if (!current) return Refuse(diagnostic, procedural::Status::MissingSource, "authority");
		try {
			procedural::CompiledRecipe recipe;
			if (!CompileAuthority(*current, recipe, diagnostic)) return false;
			ProceduralAuthority candidate = *current;
			const auto found =
				std::find_if(candidate.Splines.begin(), candidate.Splines.end(), [&](const auto &row) {
					return row.Name == name;
				});
			if (found == candidate.Splines.end())
				return Refuse(diagnostic, procedural::Status::MissingSource, "authored.spline");
			candidate.Splines.erase(found);
			if (!InvalidateChangedFacts(*current, candidate, recipe, diagnostic)) return false;
			return procedural_source_detail::ReplaceAuthority(store, std::move(candidate), diagnostic);
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, procedural::Status::AllocationFailed, "authored.spline");
		}
	}

	bool SetProceduralOverride(
		ecs::Store &store,
		const procedural::PlacementOverride &overrideValue,
		procedural::Diagnostic &diagnostic
	) {
		using namespace procedural_detail;
		if (!procedural_source_detail::MayEdit(store, diagnostic)) return false;
		const auto *current = procedural_source_detail::Authority(store);
		if (!current) return Refuse(diagnostic, procedural::Status::MissingSource, "authority");
		try {
			procedural::CompiledRecipe recipe;
			if (!CompileAuthority(*current, recipe, diagnostic)) return false;
			ProceduralAuthority candidate = *current;
			auto found = std::lower_bound(
				candidate.Overrides.begin(),
				candidate.Overrides.end(),
				overrideValue.TargetId,
				[](const auto &row, std::string_view name) {
					return procedural::StableNameLess(row.TargetId, name);
				}
			);
			if (found != candidate.Overrides.end() && found->TargetId == overrideValue.TargetId)
				*found = overrideValue;
			else
				candidate.Overrides.insert(found, overrideValue);
			if (!ValidateAuthoredFacts(candidate, recipe, diagnostic) ||
				!InvalidateChangedFacts(*current, candidate, recipe, diagnostic))
				return false;
			return procedural_source_detail::ReplaceAuthority(store, std::move(candidate), diagnostic);
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, procedural::Status::AllocationFailed, "authored.override");
		}
	}

	bool RemoveProceduralOverride(
		ecs::Store &store, std::string_view targetId, procedural::Diagnostic &diagnostic
	) {
		using namespace procedural_detail;
		if (!procedural_source_detail::MayEdit(store, diagnostic)) return false;
		const auto *current = procedural_source_detail::Authority(store);
		if (!current) return Refuse(diagnostic, procedural::Status::MissingSource, "authority");
		try {
			procedural::CompiledRecipe recipe;
			if (!CompileAuthority(*current, recipe, diagnostic)) return false;
			ProceduralAuthority candidate = *current;
			const auto found =
				std::find_if(candidate.Overrides.begin(), candidate.Overrides.end(), [&](const auto &row) {
					return row.TargetId == targetId;
				});
			if (found == candidate.Overrides.end())
				return Refuse(diagnostic, procedural::Status::MissingSource, "authored.override");
			candidate.Overrides.erase(found);
			if (!InvalidateChangedFacts(*current, candidate, recipe, diagnostic)) return false;
			return procedural_source_detail::ReplaceAuthority(store, std::move(candidate), diagnostic);
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, procedural::Status::AllocationFailed, "authored.override");
		}
	}
}
