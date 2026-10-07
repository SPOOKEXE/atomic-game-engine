#include <engine/imagegraph/Particle3D.hpp>

#include <cmath>
#include <cstdint>

namespace engine::imagegraph {
	bool ValidParticleRecord3D(const ParticleRecord3D &record) noexcept {
		for (float value :
			 {record.Active, record.MeshIndex, record.LifeMaximum, record.LifeTime, record.RenderFlags})
			if (!std::isfinite(value)) return false;
		for (const auto *words : {&record.Colour, &record.Velocity})
			for (float value : *words)
				if (!std::isfinite(value)) return false;
		for (float value : record.Reserved)
			if (!std::isfinite(value)) return false;
		return record.RenderFlags >= -0x1p31f && record.RenderFlags < 0x1p31f;
	}
	bool ValidParticleRecords3D(std::span<const ParticleRecord3D> records, size_t maximumRecords) noexcept {
		if (records.size() > maximumRecords) return false;
		for (const auto &record : records)
			if (!ValidParticleRecord3D(record)) return false;
		return true;
	}
	bool ParticleBillboard3D(const ParticleRecord3D &record, bool &billboard) noexcept {
		if (!ValidParticleRecord3D(record)) return false;
		const double lower = std::floor(double(record.RenderFlags));
		const double fraction = double(record.RenderFlags) - lower;
		const double rounded = fraction < .5						 ? lower
							   : fraction > .5						 ? lower + 1
							   : std::fmod(std::abs(lower), 2.) == 0 ? lower
																	 : lower + 1;
		billboard = (static_cast<int32_t>(rounded) & 1) != 0;
		return true;
	}
}
