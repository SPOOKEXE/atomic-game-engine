#pragma once

#include "Sampler.hpp"
#include "SourceProjectionMath.hpp"

namespace engine::imagegraph::detail {
	struct SurfaceProjectionControls {
		ProjectionMatrix Inverse{};
		ProjectionVector Position{};
		int Projection = 1, BlendType = 0, Except = 0;
		float Fov = 60, Distance = 2, Scale = 3.46f;
		float NoiseSeed = 0, NoiseThreshold = .5f;
		Vector2 DepthRange{0, 1};
		bool BothSides = true, Noise = false, Filtered = false;
		std::array<bool, 3> Back{};
		std::span<const Colour> Palette;
	};
	using SurfaceProjectionFragments = std::array<Rgba, 3>;

	// The source's packed face names differ from its wrapper's physical atlas order.
	inline Rgba SurfaceProjectionPacked(const Image &atlas, int index, float u, float v, bool filtered) {
		auto pixel = Texture(
			atlas,
			(float(index % 3) + std::clamp(u, 0.f, 1.f)) / 3,
			(float(index / 3) + std::clamp(v, 0.f, 1.f)) / 3,
			filtered
		);
		for (double &channel : pixel)
			channel = float(channel);
		return pixel;
	}
	inline bool SurfaceProjectionPixel(
		const SurfaceProjectionControls &c,
		const Image &atlas,
		uint32_t width,
		uint32_t height,
		float u,
		float v,
		SurfaceProjectionFragments &output
	) {
		output = {};
		ProjectionVector eye{}, direction{};
		if (!ProjectionRay(
				c.Inverse,
				c.Position,
				u,
				v,
				float(width) / height,
				c.Projection,
				c.Fov,
				c.Distance,
				c.Scale,
				eye,
				direction
			))
			return false;
		const float voxel = 2 / float(std::max(width, height));
		for (float &d : direction)
			if (std::abs(d) < .001f) d = .001f;
		ProjectionVector origin{}, cell{}, reciprocal{}, sign{}, dis{}, mask{};
		for (size_t k = 0; k < 3; ++k) {
			origin[k] = eye[k] / voxel;
			cell[k] = std::floor(origin[k]);
			reciprocal[k] = 1 / direction[k];
			sign[k] = direction[k] > 0 ? 1 : -1;
			dis[k] = (cell[k] - origin[k] + .5f + sign[k] * .5f) * reciprocal[k];
		}
		if (!ProjectionFinite(origin) || !ProjectionFinite(dis)) return false;
		const float maximum =
			std::sqrt(3.f) * std::max(width, height) * 2 * (c.Projection == 0 ? c.Distance : 1);
		std::array<std::array<float, 2>, 3> hitUv{};
		bool hit = false;
		for (uint64_t i = 0; float(i) < maximum; ++i) {
			ProjectionVector world{}, sample{};
			for (size_t k = 0; k < 3; ++k) {
				world[k] = (cell[k] + .5f) * voxel;
				sample[k] = world[k] * .5f + .5f;
			}
			if (sample[0] >= 0 && sample[0] < 1 && sample[1] >= 0 && sample[1] < 1 && sample[2] >= 0 &&
				sample[2] < 1) {
				const std::array<std::array<float, 2>, 3> uv{
					{{sample[0], sample[1]}, {1 - sample[2], sample[1]}, {sample[0], sample[2]}}
				};
				const auto top = SurfaceProjectionPacked(atlas, 0, uv[0][0], uv[0][1], c.Filtered),
						   front = SurfaceProjectionPacked(atlas, 1, uv[1][0], uv[1][1], c.Filtered),
						   side = SurfaceProjectionPacked(atlas, 2, uv[2][0], uv[2][1], c.Filtered);
				hit = top[3] > sample[2] && front[3] > sample[0] && side[3] > 1 - sample[1];
				if (c.BothSides)
					hit = hit && top[3] > 1 - sample[2] && front[3] > 1 - sample[0] && side[3] > sample[1];
				if (c.Noise) {
					ProjectionVector noise{};
					for (size_t k = 0; k < 3; ++k) {
						float p = world[k] * (c.NoiseSeed / 10000 + .3183099f) + .1f;
						noise[k] = (p - std::floor(p)) * 17;
					}
					float n = noise[0] * noise[1] * noise[2] * (noise[0] + noise[1] + noise[2]);
					if (!std::isfinite(n)) return false;
					n -= std::floor(n);
					if (n > c.NoiseThreshold) hit = false;
				}
				if (hit) {
					hitUv = uv;
					break;
				}
			}
			for (size_t k = 0; k < 3; ++k)
				mask[k] = dis[k] <= dis[(k + 1) % 3] && dis[k] <= dis[(k + 2) % 3] ? 1 : 0;
			for (size_t k = 0; k < 3; ++k) {
				dis[k] += mask[k] * sign[k] * reciprocal[k];
				cell[k] += mask[k] * sign[k];
			}
			if (!ProjectionFinite(cell) || !ProjectionFinite(dis)) return false;
		}
		if (!hit) return true;
		std::array<Rgba, 3> faces;
		for (size_t k = 0; k < 3; ++k) {
			const size_t axis = k == 0 ? 2 : k == 1 ? 0 : 1;
			const int index = int(k) + (c.Back[k] && sign[axis] >= 0 ? 3 : 0);
			faces[k] = SurfaceProjectionPacked(atlas, index, hitUv[k][0], hitUv[k][1], c.Filtered);
		}
		int blendIndex = 0;
		if (c.BlendType == 0) {
			const size_t k = mask[2] > .5f ? 0 : mask[0] > .5f ? 1 : 2;
			const size_t axis = k == 0 ? 2 : k == 1 ? 0 : 1;
			output[0] = faces[k];
			blendIndex = int(k) + (sign[axis] >= 0 ? 0 : 3);
		} else if (c.BlendType == 1) {
			for (size_t k = 0; k < 4; ++k)
				output[0][k] = (float(faces[0][k]) + float(faces[1][k]) + float(faces[2][k])) / 3.f;
		} else {
			const size_t selected = c.Except == 0 ? 0 : c.Except == 1 ? 1 : 2;
			const size_t axis = selected == 0 ? 2 : selected == 1 ? 0 : 1;
			if (mask[axis] > .5f)
				output[0] = faces[selected];
			else
				for (size_t k = 0; k < 4; ++k)
					output[0][k] =
						(float(faces[(selected + 1) % 3][k]) + float(faces[(selected + 2) % 3][k])) / 2.f;
		}
		output[0][3] = 1;
		const auto &tint = c.Palette[size_t(blendIndex) % c.Palette.size()];
		output[0][0] = float(output[0][0]) * (tint.Red / 255.f);
		output[0][1] = float(output[0][1]) * (tint.Green / 255.f);
		output[0][2] = float(output[0][2]) * (tint.Blue / 255.f);
		ProjectionVector near{};
		for (size_t k = 0; k < 3; ++k)
			near[k] = (cell[k] - origin[k] + .5f - .5f * sign[k]) * reciprocal[k];
		const float time = std::max(near[0], std::max(near[1], near[2]));
		float squared = 0;
		for (size_t k = 0; k < 3; ++k) {
			const float point = (origin[k] + direction[k] * time) * voxel;
			const float delta = eye[k] - point;
			squared += delta * delta;
		}
		float depth = std::sqrt(squared) / c.Scale;
		depth = (depth - float(c.DepthRange.X)) / (float(c.DepthRange.Y) - float(c.DepthRange.X));
		if (!std::isfinite(depth)) return false;
		output[1] = {depth, depth, depth, 1};
		output[2] = {mask[0], mask[1], mask[2], 1};
		for (const auto &pixel : output)
			for (double channel : pixel)
				if (!std::isfinite(channel)) return false;
		return true;
	}
}
