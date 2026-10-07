#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace engine::imagegraph {
	enum class ParticleBlend3D { Normal, Alpha, Add, Maximum };
	// Source ParticleData is a separate 64-byte f32 buffer, alongside each MeshInstance3D transform.
	// Colours retain floating RGBA, including alpha, rather than the ordinary instancer's RGB lanes.
	struct ParticleRecord3D {
		float Active = 0, MeshIndex = 0, LifeMaximum = 0, LifeTime = 0;
		float RenderFlags = 0;
		std::array<float, 3> Reserved{};
		std::array<float, 4> Colour{1, 1, 1, 1}, Velocity{};
		bool operator==(const ParticleRecord3D &) const = default;
	};
	static_assert(sizeof(ParticleRecord3D) == 64);
	static_assert(offsetof(ParticleRecord3D, RenderFlags) == 16);
	static_assert(offsetof(ParticleRecord3D, Colour) == 32);
	static_assert(offsetof(ParticleRecord3D, Velocity) == 48);
	// Finite source words and representable shader flag conversion. Unknown flag bits are retained.
	bool ValidParticleRecord3D(const ParticleRecord3D &record) noexcept;
	// Logical record bytes only; caller admits backing capacity and transform storage separately.
	bool ValidParticleRecords3D(std::span<const ParticleRecord3D> records, size_t maximumRecords) noexcept;
	// Source HLSL round uses nearest-even, independent of the host rounding mode.
	bool ParticleBillboard3D(const ParticleRecord3D &record, bool &billboard) noexcept;
}
