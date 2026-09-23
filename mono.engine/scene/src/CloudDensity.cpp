#include <engine/scene/CloudDensity.hpp>

#include <algorithm>
#include <cmath>

namespace engine::scene {

	namespace {
		float Saturate(float value) {
			return std::clamp(value, 0.0f, 1.0f);
		}
		float Bell(float value) {
			return std::exp(-(value * value));
		}
	}

	float WindLineCloudDensity(
		const PreparedTornadoField &field, const core::Vector3 &localPosition, float timeSeconds
	) {
		const TornadoParameters &parameters = field.Parameters;
		if (localPosition.Y < 0.0f || localPosition.Y > parameters.TopHeight * 1.16f) return 0.0f;
		const StormSample sample = SampleTornadoField(field, core::Vector3::Zero, localPosition, timeSeconds);
		const float radius = std::hypot(localPosition.X, localPosition.Z);
		const float height = Saturate(localPosition.Y / parameters.TopHeight);
		const float pressureMoisture = Saturate(
			parameters.Humidity * (0.30f + parameters.PressureDrop / 100.0f) * std::sqrt(parameters.Energy)
		);
		const float windNormalizer = std::max(
			parameters.PeakInflowSpeed + parameters.PeakUpdraftSpeed * 0.75f +
				parameters.PeakTangentialSpeed * 0.18f,
			1.0f
		);
		const float windLineDensity = Saturate(
			(sample.InflowSpeed + std::max(sample.VerticalSpeed, 0.0f) * 0.75f +
			 sample.TangentialSpeed * 0.18f) /
			windNormalizer * 2.2f
		);
		const float lowerColumn = 1.0f - Saturate((height - 0.78f) / 0.24f);
		const float funnel = sample.Condensation * lowerColumn * (0.38f + windLineDensity * 0.78f);
		const float wallRadius =
			(radius - parameters.CoreRadius * 1.8f) / std::max(parameters.CoreRadius * 1.7f, 1.0f);
		const float wallCloud = pressureMoisture * Bell(wallRadius) * Bell((height - 0.24f) / 0.22f) *
								(0.32f + windLineDensity * 0.78f);
		const float phase = std::atan2(localPosition.Z, localPosition.X) * 5.0f * field.RotationSign +
							radius * field.InverseCoreRadius * 1.45f - height * 5.2f -
							timeSeconds * (0.28f + parameters.Turbulence * 0.006f) * field.RotationSign;
		const float streamlineBands = 0.62f + 0.38f * (0.5f + 0.5f * std::sin(phase));
		const float cellularVariation =
			0.82f + 0.18f * std::sin(
								localPosition.X * 0.043f + localPosition.Y * 0.029f -
								localPosition.Z * 0.037f + timeSeconds * 0.11f
							);
		// The funnel carries its own taper through the upper cell. A separate
		// high anvil in this sparse octree resolves as a detached box at distance;
		// authored cloud volumes provide the broader storm deck instead.
		return Saturate(std::max(funnel, wallCloud) * streamlineBands * cellularVariation);
	}

	CloudDensityBuildStats BuildCloudDensity(
		CloudDensityOctree &tree,
		const PreparedTornadoField &field,
		float timeSeconds,
		CloudDensityBuildConfig config
	) {
		CloudDensityBuildStats stats;
		const CloudDensityOctreeConfig &treeConfig = tree.Config();
		if (!std::isfinite(timeSeconds) || config.CoarseDepth == 0 || config.CoarseDepth > config.FineDepth ||
			config.FineDepth > treeConfig.MaximumDepth || config.FineDepth > 8 ||
			!std::isfinite(config.FineRadiusInCoreRadii) || config.FineRadiusInCoreRadii <= 0.0f ||
			!std::isfinite(config.MinimumDensity) || config.MinimumDensity < 0.0f ||
			config.MinimumDensity >= 1.0f)
			return stats;
		tree.Clear();
		const uint32_t coarseResolution = uint32_t{1} << config.CoarseDepth;
		const uint32_t refinement = uint32_t{1} << (config.FineDepth - config.CoarseDepth);
		const core::Vector3 coarseSize = treeConfig.RootSize / static_cast<float>(coarseResolution);
		const core::Vector3 fineSize = coarseSize / static_cast<float>(refinement);
		const float fineRadius = field.Parameters.CoreRadius * config.FineRadiusInCoreRadii;
		const float coarseMargin = std::hypot(coarseSize.X, coarseSize.Z) * 0.5f;
		for (uint32_t z = 0; z < coarseResolution; ++z)
			for (uint32_t y = 0; y < coarseResolution; ++y)
				for (uint32_t x = 0; x < coarseResolution; ++x) {
					const core::Vector3 coarseMinimum =
						treeConfig.RootMinimum + core::Vector3{
													 static_cast<float>(x) * coarseSize.X,
													 static_cast<float>(y) * coarseSize.Y,
													 static_cast<float>(z) * coarseSize.Z
												 };
					const core::Vector3 coarseCenter = coarseMinimum + coarseSize * 0.5f;
					const bool refine =
						std::hypot(coarseCenter.X, coarseCenter.Z) <= fineRadius + coarseMargin &&
						coarseCenter.Y <= field.Parameters.TopHeight * 0.76f;
					if (!refine) {
						++stats.EvaluatedCells;
						const float density = WindLineCloudDensity(field, coarseCenter, timeSeconds);
						if (density >= config.MinimumDensity &&
							tree.SetDensity(coarseCenter, density, config.CoarseDepth))
							++stats.CoarseCells;
						continue;
					}
					for (uint32_t fineZ = 0; fineZ < refinement; ++fineZ)
						for (uint32_t fineY = 0; fineY < refinement; ++fineY)
							for (uint32_t fineX = 0; fineX < refinement; ++fineX) {
								const core::Vector3 fineCenter =
									coarseMinimum + core::Vector3{
														(static_cast<float>(fineX) + 0.5f) * fineSize.X,
														(static_cast<float>(fineY) + 0.5f) * fineSize.Y,
														(static_cast<float>(fineZ) + 0.5f) * fineSize.Z
													};
								++stats.EvaluatedCells;
								const float density = WindLineCloudDensity(field, fineCenter, timeSeconds);
								if (density >= config.MinimumDensity &&
									tree.SetDensity(fineCenter, density, config.FineDepth))
									++stats.FineCells;
							}
				}
		stats.StoredLeaves = tree.StoredVoxelCount();
		stats.Nodes = tree.NodeCount();
		return stats;
	}
}
