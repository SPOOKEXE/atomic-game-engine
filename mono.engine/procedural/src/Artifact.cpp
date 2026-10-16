#include "Encoding.hpp"

#include <engine/procedural/Artifact.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace engine::procedural {

	namespace {
		bool ValidLimits(const ArtifactLimits &limits) {
			return limits.MaximumKeyBytes <= 262144 && limits.MaximumPlacements <= 4096 &&
				   limits.MaximumRoads <= 256 && limits.MaximumNameBytes <= 128 &&
				   limits.MaximumBytes <= 1048576;
		}

		bool ValidName(std::string_view name, size_t maximum, bool optional = false) {
			return name.size() <= maximum && ((optional && name.empty()) || ValidStableName(name));
		}

		bool ValidKey(std::span<const uint8_t> key, const ArtifactLimits &limits) {
			return key.size() >= 4 && key.size() <= limits.MaximumKeyBytes &&
				   std::equal(key.begin(), key.begin() + 4, "RGK2");
		}

		std::string_view KindTag(PlacementKind kind) {
			switch (kind) {
			case PlacementKind::Tree:
				return "tree";
			case PlacementKind::VillageLot:
				return "village_lot";
			case PlacementKind::House:
				return "house";
			case PlacementKind::Entrance:
				return "entrance";
			default:
				return {};
			}
		}

		bool ParseKind(std::string_view tag, PlacementKind &kind) {
			if (tag == "tree")
				kind = PlacementKind::Tree;
			else if (tag == "village_lot")
				kind = PlacementKind::VillageLot;
			else if (tag == "house")
				kind = PlacementKind::House;
			else if (tag == "entrance")
				kind = PlacementKind::Entrance;
			else
				return false;
			return true;
		}

		bool WritePoint(detail::Writer &writer, Point point) {
			return writer.U64(std::bit_cast<uint64_t>(point.X)) &&
				   writer.U64(std::bit_cast<uint64_t>(point.Y));
		}

		struct ArtifactReader {
			std::span<const uint8_t> Bytes{};
			size_t Offset = 0;

			bool Raw(size_t size, std::span<const uint8_t> &out) {
				if (size > Bytes.size() - Offset) return false;
				out = Bytes.subspan(Offset, size);
				Offset += size;
				return true;
			}
			bool U8(uint8_t &value) {
				if (Offset == Bytes.size()) return false;
				value = Bytes[Offset++];
				return true;
			}
			bool U16(uint16_t &value) {
				uint8_t low;
				uint8_t high;
				if (!U8(low) || !U8(high)) return false;
				value = static_cast<uint16_t>(low | static_cast<uint16_t>(high) << 8);
				return true;
			}
			bool U32(uint32_t &value) {
				value = 0;
				for (unsigned shift = 0; shift < 32; shift += 8) {
					uint8_t byte;
					if (!U8(byte)) return false;
					value |= static_cast<uint32_t>(byte) << shift;
				}
				return true;
			}
			bool U64(uint64_t &value) {
				value = 0;
				for (unsigned shift = 0; shift < 64; shift += 8) {
					uint8_t byte;
					if (!U8(byte)) return false;
					value |= static_cast<uint64_t>(byte) << shift;
				}
				return true;
			}
			bool Text(std::string &value, size_t maximum) {
				uint16_t size;
				std::span<const uint8_t> raw;
				if (!U16(size) || size > maximum || !Raw(size, raw)) return false;
				if (raw.empty()) {
					value.clear();
					return true;
				}
				value.assign(reinterpret_cast<const char *>(raw.data()), raw.size());
				return true;
			}
			bool PointValue(Point &point) {
				uint64_t x;
				uint64_t y;
				if (!U64(x) || !U64(y)) return false;
				point = {std::bit_cast<int64_t>(x), std::bit_cast<int64_t>(y)};
				return true;
			}
		};
	}

	Status EncodeRegionResult(
		const RegionResult &result,
		std::vector<uint8_t> &out,
		Diagnostic &diagnostic,
		const ArtifactLimits &limits
	) {
		if (!ValidLimits(limits) || result.KeyBytes.size() > limits.MaximumKeyBytes ||
			result.Placements.size() > limits.MaximumPlacements || result.Roads.size() > limits.MaximumRoads)
			return detail::Refuse(diagnostic, Status::LimitExceeded, "artifact.limits");
		if (!ValidKey(result.KeyBytes, limits))
			return detail::Refuse(diagnostic, Status::InvalidInput, "artifact.key");
		try {
			std::vector<const Placement *> placements;
			placements.reserve(result.Placements.size());
			for (const Placement &placement : result.Placements) {
				if (!ValidName(placement.Id, limits.MaximumNameBytes) ||
					!ValidName(placement.SourceName, limits.MaximumNameBytes, true) ||
					KindTag(placement.Kind).empty() || placement.RotationQuarterTurns > 3)
					return detail::Refuse(diagnostic, Status::InvalidInput, "artifact.placement");
				placements.push_back(&placement);
			}
			std::sort(placements.begin(), placements.end(), [](const Placement *a, const Placement *b) {
				return StableNameLess(a->Id, b->Id);
			});
			for (size_t index = 1; index < placements.size(); ++index)
				if (placements[index - 1]->Id == placements[index]->Id)
					return detail::Refuse(diagnostic, Status::InvalidInput, placements[index]->Id);
			std::vector<const RoadSpline *> roads;
			roads.reserve(result.Roads.size());
			for (const RoadSpline &road : result.Roads) {
				if (!ValidName(road.Id, limits.MaximumNameBytes) ||
					!ValidName(road.EntranceName, limits.MaximumNameBytes) ||
					!ValidName(road.VillageName, limits.MaximumNameBytes))
					return detail::Refuse(diagnostic, Status::InvalidInput, "artifact.road");
				roads.push_back(&road);
			}
			std::sort(roads.begin(), roads.end(), [](const RoadSpline *a, const RoadSpline *b) {
				return StableNameLess(a->Id, b->Id);
			});
			for (size_t index = 1; index < roads.size(); ++index)
				if (roads[index - 1]->Id == roads[index]->Id)
					return detail::Refuse(diagnostic, Status::InvalidInput, roads[index]->Id);
			for (const Placement *placement : placements) {
				const auto found = std::lower_bound(
					roads.begin(),
					roads.end(),
					placement->Id,
					[](const RoadSpline *road, std::string_view id) { return StableNameLess(road->Id, id); }
				);
				if (found != roads.end() && (*found)->Id == placement->Id)
					return detail::Refuse(diagnostic, Status::InvalidInput, placement->Id);
			}
			detail::Writer writer{{}, limits.MaximumBytes};
			if (!writer.Raw(std::array<uint8_t, 4>{'R', 'G', 'A', '1'}) ||
				!writer.U32(static_cast<uint32_t>(result.KeyBytes.size())) || !writer.Raw(result.KeyBytes) ||
				!writer.U32(static_cast<uint32_t>(placements.size())))
				return detail::Refuse(diagnostic, Status::LimitExceeded, "artifact.bytes");
			for (const Placement *placement : placements) {
				if (!writer.Text(KindTag(placement->Kind)) || !writer.Text(placement->Id) ||
					!writer.Text(placement->SourceName) || !WritePoint(writer, placement->Position) ||
					!writer.U8(placement->RotationQuarterTurns) || !writer.U16(placement->Variant) ||
					!writer.U16(placement->Material))
					return detail::Refuse(diagnostic, Status::LimitExceeded, "artifact.bytes");
			}
			if (!writer.U32(static_cast<uint32_t>(roads.size())))
				return detail::Refuse(diagnostic, Status::LimitExceeded, "artifact.bytes");
			for (const RoadSpline *road : roads) {
				if (!writer.Text("quadratic_road") || !writer.Text(road->Id) ||
					!writer.Text(road->EntranceName) || !writer.Text(road->VillageName))
					return detail::Refuse(diagnostic, Status::LimitExceeded, "artifact.bytes");
				for (Point control : road->Controls)
					if (!WritePoint(writer, control))
						return detail::Refuse(diagnostic, Status::LimitExceeded, "artifact.bytes");
			}
			out = std::move(writer.Bytes);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::Refuse(diagnostic, Status::AllocationFailed);
		}
	}

	Status DecodeRegionResult(
		std::span<const uint8_t> bytes,
		RegionResult &out,
		Diagnostic &diagnostic,
		std::span<const uint8_t> expectedKey,
		const ArtifactLimits &limits
	) {
		if (!ValidLimits(limits) || bytes.size() > limits.MaximumBytes ||
			expectedKey.size() > limits.MaximumKeyBytes)
			return detail::Refuse(diagnostic, Status::LimitExceeded, "artifact.bytes");
		if (!expectedKey.empty() && !ValidKey(expectedKey, limits))
			return detail::Refuse(diagnostic, Status::InvalidInput, "artifact.expected_key");
		try {
			ArtifactReader reader{bytes};
			std::span<const uint8_t> magic;
			if (!reader.Raw(4, magic) || !std::equal(magic.begin(), magic.end(), "RGA1"))
				return detail::Refuse(diagnostic, Status::InvalidInput, "artifact.magic");
			RegionResult candidate;
			uint32_t keySize;
			std::span<const uint8_t> key;
			if (!reader.U32(keySize) || keySize > limits.MaximumKeyBytes || !reader.Raw(keySize, key) ||
				!ValidKey(key, limits))
				return detail::Refuse(diagnostic, Status::InvalidInput, "artifact.key");
			candidate.KeyBytes.assign(key.begin(), key.end());
			uint32_t placementCount;
			if (!reader.U32(placementCount) || placementCount > limits.MaximumPlacements)
				return detail::Refuse(diagnostic, Status::LimitExceeded, "artifact.placements");
			candidate.Placements.reserve(placementCount);
			for (uint32_t index = 0; index < placementCount; ++index) {
				std::string tag;
				Placement placement;
				if (!reader.Text(tag, 11) || !ParseKind(tag, placement.Kind) ||
					!reader.Text(placement.Id, limits.MaximumNameBytes) ||
					!reader.Text(placement.SourceName, limits.MaximumNameBytes) ||
					!reader.PointValue(placement.Position) || !reader.U8(placement.RotationQuarterTurns) ||
					!reader.U16(placement.Variant) || !reader.U16(placement.Material))
					return detail::Refuse(diagnostic, Status::InvalidInput, "artifact.placement");
				candidate.Placements.push_back(std::move(placement));
			}
			uint32_t roadCount;
			if (!reader.U32(roadCount) || roadCount > limits.MaximumRoads)
				return detail::Refuse(diagnostic, Status::LimitExceeded, "artifact.roads");
			candidate.Roads.reserve(roadCount);
			for (uint32_t index = 0; index < roadCount; ++index) {
				std::string tag;
				RoadSpline road;
				if (!reader.Text(tag, 14) || tag != "quadratic_road" ||
					!reader.Text(road.Id, limits.MaximumNameBytes) ||
					!reader.Text(road.EntranceName, limits.MaximumNameBytes) ||
					!reader.Text(road.VillageName, limits.MaximumNameBytes))
					return detail::Refuse(diagnostic, Status::InvalidInput, "artifact.road");
				for (Point &control : road.Controls)
					if (!reader.PointValue(control))
						return detail::Refuse(diagnostic, Status::InvalidInput, "artifact.road");
				candidate.Roads.push_back(std::move(road));
			}
			if (reader.Offset != bytes.size())
				return detail::Refuse(diagnostic, Status::InvalidInput, "artifact.trailing");
			std::vector<uint8_t> canonical;
			const Status status = EncodeRegionResult(candidate, canonical, diagnostic, limits);
			if (status != Status::Ok) return status;
			if (!std::equal(bytes.begin(), bytes.end(), canonical.begin(), canonical.end()))
				return detail::Refuse(diagnostic, Status::InvalidInput, "artifact.canonical");
			if (!expectedKey.empty() &&
				!std::equal(expectedKey.begin(), expectedKey.end(), key.begin(), key.end()))
				return detail::Refuse(diagnostic, Status::StaleArtifact, "artifact.key");
			out = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::Refuse(diagnostic, Status::AllocationFailed);
		}
	}
}
