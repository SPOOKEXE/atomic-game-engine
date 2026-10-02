#pragma once
#include "MeshPayload.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/Particle.hpp>

#include <cmath>
namespace engine::imagegraph::detail {
	template <bool Retained> uint64_t ParticleDataBytes(const ParticleData2D &data) {
		uint64_t bytes = MeshAddBytes(
			sizeof(ParticleData2D), Retained ? data.OriginNodeId.capacity() : data.OriginNodeId.size()
		);
		bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(data.Sprites));
		{
			const auto &particle = data.State;
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(particle.XHistory));
			bytes = MeshAddBytes(bytes, MeshVectorBytes<Retained>(particle.YHistory));
		}
		for (const auto &sprite : data.Sprites)
			bytes = MeshAddBytes(bytes, Retained ? sprite.Pixels.capacity() : sprite.Pixels.size());
		return bytes;
	}
	template <bool Retained> uint64_t ParticleStorageBytes(const ParticleValue &value) {
		return value.Data ? ParticleDataBytes<Retained>(*value.Data) : 0;
	}
	inline bool ValidParticlePayload(const ParticleValue &value) {
		if (!value.Data) return true;
		const auto &data = *value.Data;
		if (data.OriginProcessorRow >= Limits::MaximumArrayElements ||
			data.OriginNodeId.size() > Limits::MaximumTextBytes ||
			data.Sprites.size() > Limits::MaximumArrayElements ||
			ParticleDataBytes<true>(data) > Limits::MaximumEvaluationBytes)
			return false;
		uint64_t totalHistory = 0, totalPixels = 0;
		{
			const auto &particle = data.State;
			if (particle.SourceSlot >= Limits::MaximumArrayElements || particle.Blend > 0x00ffffff ||
				!std::isfinite(particle.RotationDegrees) || !std::isfinite(particle.Alpha) ||
				particle.XHistory.size() != particle.YHistory.size())
				return false;
			for (const auto value : particle.Position)
				if (!std::isfinite(value)) return false;
			for (const auto value : particle.Scale)
				if (!std::isfinite(value)) return false;
			if (particle.SpriteSlot && *particle.SpriteSlot >= data.Sprites.size()) return false;
			for (const auto *history : {&particle.XHistory, &particle.YHistory}) {
				if (history->size() > Limits::MaximumRangeFrames) return false;
				totalHistory = MeshAddBytes(totalHistory, history->size());
				if (totalHistory > Limits::MaximumArrayElements) return false;
				for (const auto &value : *history)
					if (value && !std::isfinite(*value)) return false;
			}
		}
		for (const auto &sprite : data.Sprites) {
			if (!ValidSurfaceLayout(sprite, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
				!FiniteSurfaceSamples(sprite))
				return false;
			totalPixels = MeshAddBytes(totalPixels, sprite.Pixels.size());
			if (totalPixels > Limits::MaximumOutputBytes) return false;
		}
		return true;
	}
} // namespace engine::imagegraph::detail
