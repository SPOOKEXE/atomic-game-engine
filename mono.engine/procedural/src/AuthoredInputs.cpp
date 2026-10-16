#include "Encoding.hpp"
#include "RegionInternal.hpp"

#include <engine/procedural/Region.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <new>
#include <utility>

namespace engine::procedural {

	namespace authored_input_detail {
		// Covers every int32 region coordinate at the maximum 64-cell side length.
		constexpr int64_t WORLD_CELLS = int64_t{1} << 37;
		constexpr int64_t WORLD_SUBCELLS = WORLD_CELLS * SUBCELLS_PER_CELL;
		// The maximum legal recipe query halo is smaller than 128 cells.
		constexpr int64_t QUERY_SUBCELLS = WORLD_SUBCELLS + 128 * SUBCELLS_PER_CELL;
		constexpr size_t INPUT_BYTES = 512;

		bool Name(std::string_view value) {
			return value.size() <= 128 && ValidStableName(value);
		}

		bool Coordinate(Point value, int64_t limit) {
			return value.X >= -limit && value.X < limit && value.Y >= -limit && value.Y < limit;
		}

		bool ValidSpline(const AuthoredSpline &value) {
			return Name(value.Name) && Name(value.EntranceName) && Coordinate(value.Control, QUERY_SUBCELLS);
		}

		bool ValidOverride(const PlacementOverride &value) {
			PlacementKind kind;
			Point cell;
			if (!detail::OverrideTarget(value.TargetId, kind, cell)) return false;
			if (value.Action == OverrideAction::Delete)
				return value.Position == Point{} && value.RotationQuarterTurns == 0 && value.Variant == 0 &&
					   value.Material == 0 && value.SourceName.empty();
			return value.Action == OverrideAction::Replace && Coordinate(value.Position, WORLD_SUBCELLS) &&
				   value.RotationQuarterTurns < 4 && value.Variant < 64 && value.Material < 64 &&
				   (kind == PlacementKind::Tree ? value.SourceName.empty() : Name(value.SourceName));
		}

		bool ValidChoices(const PlacementOverride &value, const ForestVillageSettings &settings) {
			if (!ValidOverride(value)) return false;
			if (value.Action == OverrideAction::Delete) return true;
			const bool tree = value.TargetId.starts_with("tree/");
			return value.Variant < (tree ? settings.TreeVariants : settings.HouseVariants) &&
				   value.Material < (tree ? settings.TreeMaterials : settings.HouseMaterials);
		}

		Status SplineBounds(
			const ForestVillageSettings &settings,
			const AuthoredSpline &spline,
			Point entrance,
			Point village,
			Bounds &out,
			Diagnostic &diagnostic
		) {
			if (!ValidSpline(spline) || !Coordinate(entrance, QUERY_SUBCELLS) ||
				!Coordinate(village, QUERY_SUBCELLS))
				return detail::Refuse(diagnostic, Status::InvalidInput, "spline.input");
			const int64_t dx = village.X - entrance.X;
			const int64_t dy = village.Y - entrance.Y;
			const int64_t reach = int64_t{settings.MaximumRoadReachCells} * SUBCELLS_PER_CELL;
			if ((dx == 0 && dy == 0) || dx < -reach || dx > reach || dy < -reach || dy > reach ||
				dx * dx + dy * dy > reach * reach)
				return detail::Refuse(diagnostic, Status::InvalidInput, "spline.reach");
			const Point midpoint{entrance.X + dx / 2, entrance.Y + dy / 2};
			const int64_t bend = int64_t{settings.RoadBendCells} * SUBCELLS_PER_CELL;
			if (spline.Control.X < midpoint.X - bend || spline.Control.X > midpoint.X + bend ||
				spline.Control.Y < midpoint.Y - bend || spline.Control.Y > midpoint.Y + bend)
				return detail::Refuse(diagnostic, Status::InvalidInput, "spline.bend");
			// Sixteen chords deviate by less than 32 subcells at the eight-cell bend limit.
			const int64_t margin =
				(int64_t{std::max(settings.TreeMinimumDistanceCells, settings.HouseMinimumDistanceCells)} +
				 2 + settings.RoadClearanceCells) *
					SUBCELLS_PER_CELL +
				32;
			out = {
				{std::min({entrance.X, village.X, spline.Control.X}) - margin,
				 std::min({entrance.Y, village.Y, spline.Control.Y}) - margin},
				{std::max({entrance.X, village.X, spline.Control.X}) + margin + 1,
				 std::max({entrance.Y, village.Y, spline.Control.Y}) + margin + 1},
			};
			diagnostic = {};
			return Status::Ok;
		}

		bool WritePoint(detail::Writer &writer, Point point) {
			return writer.U64(std::bit_cast<uint64_t>(point.X)) &&
				   writer.U64(std::bit_cast<uint64_t>(point.Y));
		}

		struct Reader {
			std::span<const uint8_t> Bytes;
			size_t Offset = 4;
			bool Unsigned(size_t count, uint64_t &out) {
				if (count > Bytes.size() - Offset) return false;
				out = 0;
				for (size_t index = 0; index < count; ++index)
					out |= uint64_t{Bytes[Offset++]} << (index * 8);
				return true;
			}
			bool Text(std::string &out) {
				uint64_t size;
				if (!Unsigned(2, size) || size > 128 || size > Bytes.size() - Offset) return false;
				out.assign(reinterpret_cast<const char *>(Bytes.data() + Offset), static_cast<size_t>(size));
				Offset += static_cast<size_t>(size);
				return true;
			}
			bool ReadPoint(Point &out) {
				uint64_t x;
				uint64_t y;
				if (!Unsigned(8, x) || !Unsigned(8, y)) return false;
				out = {std::bit_cast<int64_t>(x), std::bit_cast<int64_t>(y)};
				return true;
			}
		};

		bool Header(std::span<const uint8_t> bytes, std::string_view magic) {
			return bytes.size() >= 4 && bytes.size() <= INPUT_BYTES &&
				   std::equal(bytes.begin(), bytes.begin() + 4, magic.begin());
		}
	}

	namespace detail {
		bool Intersects(Bounds left, Bounds right) {
			return left.Min.X < right.Max.X && left.Max.X > right.Min.X && left.Min.Y < right.Max.Y &&
				   left.Max.Y > right.Min.Y;
		}

		bool OverrideTarget(std::string_view id, PlacementKind &kind, Point &cell) {
			if (id.size() > 128) return false;
			size_t start;
			if (id.starts_with("tree/")) {
				kind = PlacementKind::Tree;
				start = 5;
			} else if (id.starts_with("house/")) {
				kind = PlacementKind::House;
				start = 6;
			} else
				return false;
			const size_t slash = id.find('/', start);
			if (slash == std::string_view::npos || id.find('/', slash + 1) != std::string_view::npos)
				return false;
			const auto parse = [](std::string_view text, int64_t &value) {
				if (text.empty() || text == "-0" || (text.size() > 1 && text[0] == '0') ||
					(text.size() > 2 && text[0] == '-' && text[1] == '0'))
					return false;
				const auto read = std::from_chars(text.data(), text.data() + text.size(), value);
				return read.ec == std::errc{} && read.ptr == text.data() + text.size() &&
					   value >= -authored_input_detail::WORLD_CELLS &&
					   value < authored_input_detail::WORLD_CELLS;
			};
			return parse(id.substr(start, slash - start), cell.X) && parse(id.substr(slash + 1), cell.Y);
		}
	}

	Status
	EncodeAuthoredSpline(const AuthoredSpline &value, std::vector<uint8_t> &out, Diagnostic &diagnostic) {
		if (!authored_input_detail::ValidSpline(value))
			return detail::Refuse(diagnostic, Status::InvalidInput, "spline.input");
		try {
			detail::Writer writer{{}, authored_input_detail::INPUT_BYTES};
			if (!writer.Raw(std::array<uint8_t, 4>{'S', 'P', 'L', '1'}) || !writer.Text(value.Name) ||
				!writer.Text(value.EntranceName) || !authored_input_detail::WritePoint(writer, value.Control))
				return detail::Refuse(diagnostic, Status::LimitExceeded, "spline.bytes");
			out = std::move(writer.Bytes);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::Refuse(diagnostic, Status::AllocationFailed);
		}
	}

	Status DecodeAuthoredSpline(std::span<const uint8_t> bytes, AuthoredSpline &out, Diagnostic &diagnostic) {
		if (!authored_input_detail::Header(bytes, "SPL1"))
			return detail::Refuse(diagnostic, Status::InvalidInput, "spline.bytes");
		try {
			authored_input_detail::Reader reader{bytes};
			AuthoredSpline candidate;
			if (!reader.Text(candidate.Name) || !reader.Text(candidate.EntranceName) ||
				!reader.ReadPoint(candidate.Control) || reader.Offset != bytes.size() ||
				!authored_input_detail::ValidSpline(candidate))
				return detail::Refuse(diagnostic, Status::InvalidInput, "spline.bytes");
			out = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::Refuse(diagnostic, Status::AllocationFailed);
		}
	}

	Status EncodePlacementOverride(
		const PlacementOverride &value, std::vector<uint8_t> &out, Diagnostic &diagnostic
	) {
		if (!authored_input_detail::ValidOverride(value))
			return detail::Refuse(diagnostic, Status::InvalidInput, "override.input");
		try {
			detail::Writer writer{{}, authored_input_detail::INPUT_BYTES};
			if (!writer.Raw(std::array<uint8_t, 4>{'O', 'V', 'R', '1'}) || !writer.Text(value.TargetId) ||
				!writer.U8(static_cast<uint8_t>(value.Action)) ||
				(value.Action == OverrideAction::Replace &&
				 (!authored_input_detail::WritePoint(writer, value.Position) ||
				  !writer.U8(value.RotationQuarterTurns) || !writer.U16(value.Variant) ||
				  !writer.U16(value.Material) || !writer.Text(value.SourceName))))
				return detail::Refuse(diagnostic, Status::LimitExceeded, "override.bytes");
			out = std::move(writer.Bytes);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::Refuse(diagnostic, Status::AllocationFailed);
		}
	}

	Status
	DecodePlacementOverride(std::span<const uint8_t> bytes, PlacementOverride &out, Diagnostic &diagnostic) {
		if (!authored_input_detail::Header(bytes, "OVR1"))
			return detail::Refuse(diagnostic, Status::InvalidInput, "override.bytes");
		try {
			authored_input_detail::Reader reader{bytes};
			PlacementOverride candidate;
			uint64_t action;
			if (!reader.Text(candidate.TargetId) || !reader.Unsigned(1, action) || action > 1)
				return detail::Refuse(diagnostic, Status::InvalidInput, "override.bytes");
			candidate.Action = static_cast<OverrideAction>(action);
			if (candidate.Action == OverrideAction::Replace) {
				uint64_t rotation;
				uint64_t variant;
				uint64_t material;
				if (!reader.ReadPoint(candidate.Position) || !reader.Unsigned(1, rotation) ||
					!reader.Unsigned(2, variant) || !reader.Unsigned(2, material) ||
					!reader.Text(candidate.SourceName))
					return detail::Refuse(diagnostic, Status::InvalidInput, "override.bytes");
				candidate.RotationQuarterTurns = static_cast<uint8_t>(rotation);
				candidate.Variant = static_cast<uint16_t>(variant);
				candidate.Material = static_cast<uint16_t>(material);
			}
			if (reader.Offset != bytes.size() || !authored_input_detail::ValidOverride(candidate))
				return detail::Refuse(diagnostic, Status::InvalidInput, "override.bytes");
			out = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::Refuse(diagnostic, Status::AllocationFailed);
		}
	}

	Status ValidatePlacementOverride(
		const CompiledRecipe &recipe, const PlacementOverride &value, Diagnostic &diagnostic
	) {
		if (recipe.CanonicalBytes_.empty() || !authored_input_detail::ValidChoices(value, recipe.Settings_))
			return detail::Refuse(diagnostic, Status::InvalidInput, "override.choices");
		diagnostic = {};
		return Status::Ok;
	}

	Status SplineInfluence(
		const CompiledRecipe &recipe,
		const AuthoredSpline &spline,
		Point entrance,
		Point village,
		Bounds &out,
		Diagnostic &diagnostic
	) {
		if (recipe.CanonicalBytes_.empty())
			return detail::Refuse(diagnostic, Status::InvalidRecipe, "spline.recipe");
		return authored_input_detail::SplineBounds(
			recipe.Settings_, spline, entrance, village, out, diagnostic
		);
	}

	Status OverrideInfluence(
		const PlacementOverride &value, Bounds &home, Bounds &destination, Diagnostic &diagnostic
	) {
		if (!authored_input_detail::ValidOverride(value))
			return detail::Refuse(diagnostic, Status::InvalidInput, "override.input");
		PlacementKind kind;
		Point cell;
		detail::OverrideTarget(value.TargetId, kind, cell);
		const Bounds original{
			{cell.X * SUBCELLS_PER_CELL, cell.Y * SUBCELLS_PER_CELL},
			{(cell.X + 1) * SUBCELLS_PER_CELL, (cell.Y + 1) * SUBCELLS_PER_CELL}
		};
		const Bounds moved{value.Position, {value.Position.X + 1, value.Position.Y + 1}};
		home = original;
		destination = value.Action == OverrideAction::Delete ? original : moved;
		diagnostic = {};
		return Status::Ok;
	}

	Status detail::ValidateAuthoredInputs(
		const RegionRequest &request,
		Bounds owner,
		Bounds required,
		const ForestVillageSettings &settings,
		QueryView &queries,
		Diagnostic &diagnostic
	) {
		try {
			const auto &authored = request.Authored;
			if (!authored.Complete || authored.Covered != required)
				return Refuse(diagnostic, Status::InvalidCoverage, "region.authored");
			size_t count = queries.VillageLeaves.size() + queries.EntranceLeaves.size();
			if (authored.Splines.size() > request.Limits.MaximumQueryLeaves - count)
				return Refuse(diagnostic, Status::LimitExceeded, "region.query_leaves");
			count += authored.Splines.size();
			if (authored.Overrides.size() > request.Limits.MaximumQueryLeaves - count)
				return Refuse(diagnostic, Status::LimitExceeded, "region.query_leaves");
			const auto find = [](const std::vector<const SpatialLeaf *> &leaves, std::string_view name) {
				const auto found = std::lower_bound(
					leaves.begin(), leaves.end(), name, [](const SpatialLeaf *leaf, std::string_view key) {
						return StableNameLess(leaf->Name, key);
					}
				);
				return found != leaves.end() && (*found)->Name == name ? *found : nullptr;
			};
			queries.SplineLeaves.clear();
			queries.OverrideLeaves.clear();
			for (const SplineFact &fact : authored.Splines) {
				const auto *entrance = find(queries.EntranceLeaves, fact.Value.EntranceName);
				const auto *village =
					entrance ? find(queries.VillageLeaves, entrance->TargetVillage) : nullptr;
				Bounds influence;
				if (!entrance || !village)
					return Refuse(diagnostic, Status::MissingSource, "spline.endpoint");
				const Status status = authored_input_detail::SplineBounds(
					settings, fact.Value, entrance->Anchor, village->Anchor, influence, diagnostic
				);
				if (status != Status::Ok) return status;
				if (!Intersects(owner, influence))
					return Refuse(diagnostic, Status::InvalidCoverage, "spline.influence");
				for (const auto *previous : queries.SplineLeaves)
					if (previous->Value.EntranceName == fact.Value.EntranceName ||
						previous->Value.Name == fact.Value.Name)
						return Refuse(diagnostic, Status::InvalidInput, "spline.duplicate");
				queries.SplineLeaves.push_back(&fact);
			}
			for (const OverrideFact &fact : authored.Overrides) {
				if (!authored_input_detail::ValidChoices(fact.Value, settings))
					return Refuse(diagnostic, Status::InvalidInput, "override.choices");
				Bounds home;
				Bounds destination;
				const Status status = OverrideInfluence(fact.Value, home, destination, diagnostic);
				if (status != Status::Ok) return status;
				if (!Intersects(owner, home) && !Intersects(owner, destination))
					return Refuse(diagnostic, Status::InvalidCoverage, "override.influence");
				queries.OverrideLeaves.push_back(&fact);
			}
			std::sort(
				queries.SplineLeaves.begin(), queries.SplineLeaves.end(), [](const auto *a, const auto *b) {
					return StableNameLess(a->Value.Name, b->Value.Name);
				}
			);
			std::sort(
				queries.OverrideLeaves.begin(),
				queries.OverrideLeaves.end(),
				[](const auto *a, const auto *b) {
					return StableNameLess(a->Value.TargetId, b->Value.TargetId);
				}
			);
			for (size_t index = 1; index < queries.OverrideLeaves.size(); ++index)
				if (queries.OverrideLeaves[index - 1]->Value.TargetId ==
					queries.OverrideLeaves[index]->Value.TargetId)
					return Refuse(diagnostic, Status::InvalidInput, "override.duplicate");
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, Status::AllocationFailed);
		}
	}
}
