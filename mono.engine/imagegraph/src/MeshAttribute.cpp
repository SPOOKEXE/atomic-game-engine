#include "Properties.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>

namespace engine::imagegraph {
	namespace {
		bool Refuse(Diagnostic &diagnostic, std::string_view message) {
			diagnostic = {{}, std::string(message)};
			return false;
		}
	}

	// Pixel centres inside a triangle (either winding) take its barycentric value;
	// later triangles overwrite earlier ones. Padding then grows covered pixels into
	// uncovered neighbours, one ring per pass, averaging the covered 4-neighbours.
	bool RasteriseMeshAttribute(
		const MeshAttribute &node,
		const MeshAttributeData &mesh,
		std::vector<float> &out,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph mesh attribute rasterize");
		const std::string failure = detail::Validate(node);
		if (!failure.empty()) return Refuse(diagnostic, failure);
		if (mesh.Indices.size() % 3 != 0) return Refuse(diagnostic, "mesh triangle list is incomplete");
		if (mesh.Values.empty())
			return Refuse(diagnostic, "mesh has no values in the selected attribute channel");
		const size_t vertices = std::min(mesh.UVs.size(), mesh.Values.size());
		if (std::any_of(mesh.Indices.begin(), mesh.Indices.end(), [&](uint32_t index) {
				return index >= vertices;
			}))
			return Refuse(diagnostic, "mesh index exceeds its vertex streams");
		const uint32_t width = node.Width, height = node.Height;
		const size_t pixels = size_t(width) * height;
		std::array<size_t, 3> components{};
		const size_t count = node.Components.size();
		for (size_t index = 0; index < count; ++index)
			components[index] = std::string_view("xyzw").find(node.Components[index]);
		const double range = node.Maximum - node.Minimum;
		std::vector<float> rgba(pixels * 4, 0.0f);
		uint64_t work = 0;
		for (size_t triangle = 0; triangle < mesh.Indices.size(); triangle += 3) {
			std::array<double, 3> xs{}, ys{};
			for (size_t corner = 0; corner < 3; ++corner) {
				const auto &uv = mesh.UVs[mesh.Indices[triangle + corner]];
				xs[corner] = (double(uv[0]) - node.TileU) * width;
				ys[corner] = (double(uv[1]) - node.TileV) * height;
			}
			const double area = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0]);
			if (!std::isfinite(area) || area == 0) continue;
			const double left = std::max(0.0, std::floor(*std::min_element(xs.begin(), xs.end())));
			const double right = std::min(double(width), std::ceil(*std::max_element(xs.begin(), xs.end())));
			const double top = std::max(0.0, std::floor(*std::min_element(ys.begin(), ys.end())));
			const double bottom =
				std::min(double(height), std::ceil(*std::max_element(ys.begin(), ys.end())));
			if (left >= right || top >= bottom) continue;
			work += uint64_t(right - left) * uint64_t(bottom - top);
			if (work > Limits::MaximumPixelWork)
				return Refuse(diagnostic, "mesh rasterization work budget exceeded");
			for (auto y = uint32_t(top); y < uint32_t(bottom); ++y)
				for (auto x = uint32_t(left); x < uint32_t(right); ++x) {
					const double px = x + 0.5, py = y + 0.5;
					std::array<double, 3> weights{
						((xs[1] - px) * (ys[2] - py) - (xs[2] - px) * (ys[1] - py)) / area,
						((xs[2] - px) * (ys[0] - py) - (xs[0] - px) * (ys[2] - py)) / area,
						0
					};
					weights[2] = 1 - weights[0] - weights[1];
					if (weights[0] < 0 || weights[1] < 0 || weights[2] < 0) continue;
					float *pixel = &rgba[(size_t(y) * width + x) * 4];
					std::array<double, 3> values{};
					for (size_t component = 0; component < count; ++component) {
						double value = 0;
						for (size_t corner = 0; corner < 3; ++corner)
							value += weights[corner] *
									 mesh.Values[mesh.Indices[triangle + corner]][components[component]];
						values[component] = std::clamp((value - node.Minimum) / range, 0.0, 1.0);
					}
					for (size_t channel = 0; channel < 3; ++channel)
						pixel[channel] = static_cast<float>(count == 1 ? values[0] : values[channel]);
					pixel[3] = 1.0f;
				}
		}
		std::vector<float> previous;
		for (uint32_t pass = 0; pass < node.Padding; ++pass) {
			previous = rgba;
			bool grew = false;
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x) {
					float *pixel = &rgba[(size_t(y) * width + x) * 4];
					if (pixel[3] != 0) continue;
					std::array<double, 3> sum{};
					int covered = 0;
					const auto take = [&](int64_t nx, int64_t ny) {
						if (nx < 0 || ny < 0 || nx >= width || ny >= height) return;
						const float *neighbour = &previous[(size_t(ny) * width + size_t(nx)) * 4];
						if (neighbour[3] == 0) return;
						for (size_t channel = 0; channel < 3; ++channel)
							sum[channel] += neighbour[channel];
						++covered;
					};
					take(int64_t(x) - 1, y);
					take(int64_t(x) + 1, y);
					take(x, int64_t(y) - 1);
					take(x, int64_t(y) + 1);
					if (covered == 0) continue;
					for (size_t channel = 0; channel < 3; ++channel)
						pixel[channel] = static_cast<float>(sum[channel] / covered);
					pixel[3] = 1.0f;
					grew = true;
				}
			if (!grew) break;
		}
		out = std::move(rgba);
		diagnostic = {};
		return true;
	}
}
