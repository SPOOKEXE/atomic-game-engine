#pragma once
#include <engine/imagegraph/Surface.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace engine::imagegraph {
	struct ParticleState2D {
		std::array<double, 2> Position{}, Scale{1, 1};
		double RotationDegrees = 0, Alpha = 1;
		uint32_t Blend = 0x00ffffff;
		bool Active = false;
		std::vector<std::optional<double>> XHistory, YHistory;
		std::optional<uint32_t> SpriteSlot;
		size_t SourceSlot = 0;
		bool operator==(const ParticleState2D &) const = default;
	};
	// Owned snapshot of one source __particleObject; sprite slots are internal
	// to this message. Pools use ArrayValue with Particle elements.
	struct ParticleData2D {
		std::string OriginNodeId;
		size_t OriginProcessorRow = 0;
		ParticleState2D State;
		std::vector<Image> Sprites;
		bool operator==(const ParticleData2D &) const = default;
	};
} // namespace engine::imagegraph
