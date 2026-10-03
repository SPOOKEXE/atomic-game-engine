#pragma once

// Owned source Strand geometry and motion; durable replay ownership stays with the caller.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace engine::imagegraph {
	struct SourceStrandPoint {
		std::array<double, 2> Position{}, Previous{}, PreviousPrevious{}, Delta{};
		std::optional<double> IkX{}, IkY{};
		double AirResistance = .5;
		bool operator==(const SourceStrandPoint &) const = default;
	};
	struct SourceStrandHair {
		// The source ID is a per-hair RNG seed. OriginNodeId identifies its replay owner.
		uint32_t SourceId = 0;
		std::vector<SourceStrandPoint> Points;
		std::vector<double> Lengths, RestAngles;
		double Direction = 0, CurlFrequency = 0, CurlSize = 1;
		double Tension = .8, Spring = .1, AngularTension = .1;
		double RootStrength = -1, RootForce = 0, Restitution = .01;
		bool Free = false;
		bool operator==(const SourceStrandHair &) const = default;
	};
	struct SourceStrandState {
		std::vector<SourceStrandHair> Hairs;
		bool Loop = false;
		bool operator==(const SourceStrandState &) const = default;
	};

	struct StrandData2D {
		std::string OriginNodeId;
		size_t OriginProcessorRow = 0;
		uint64_t AuthoringRevision = 0;
		SourceStrandState State;
		bool operator==(const StrandData2D &) const = default;
	};
} // namespace engine::imagegraph
