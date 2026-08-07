#include <engine/effects/Particles.hpp>

#include <algorithm>
#include <cmath>

namespace engine::effects {

	namespace {
		// Sixteen bits over a sixty-four metre ceiling, which is a millimetre of
		// resolution. Both halves use the same scale, so a square particle stays
		// square through the round trip.
		constexpr float SIZE_SCALE = 65535.0f / MAX_PARTICLE_SIZE;

		uint32_t PackHalf(float metres) {
			// **Clamped rather than wrapped**, and the low end matters as much as
			// the high: a negative size comes out of a `Squash` past -1, and
			// wrapping it would make an over-squashed particle sixty-four metres
			// across for one frame — a full-screen white flash in the middle of an
			// effect, which reads as a shader fault rather than as a curve.
			const float clamped = std::clamp(metres, 0.0f, MAX_PARTICLE_SIZE);
			return static_cast<uint32_t>(clamped * SIZE_SCALE + 0.5f);
		}
	}

	uint32_t PackParticleSize(float width, float height) {
		return PackHalf(width) | (PackHalf(height) << 16);
	}

	float UnpackParticleWidth(uint32_t packed) {
		return static_cast<float>(packed & 0xFFFFu) / SIZE_SCALE;
	}

	float UnpackParticleHeight(uint32_t packed) {
		return static_cast<float>(packed >> 16) / SIZE_SCALE;
	}
}
