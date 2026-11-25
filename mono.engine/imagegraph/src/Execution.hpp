#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace engine::imagegraph::detail {
	// Intermediate bytes per pixel for a document's precision.
	inline size_t StorageBytes(Precision precision) {
		return precision == Precision::Wide ? 8 : 4;
	}
	// Declared work per output pixel, checked against Limits::MaximumPixelWork.
	inline uint64_t WorkPerPixel(const Operation &operation) {
		return std::visit(
			[](const auto &value) -> uint64_t {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, Blend>) return 2;
				if constexpr (std::is_same_v<T, Resize> || std::is_same_v<T, Transform> ||
							  std::is_same_v<T, Warp> || std::is_same_v<T, Lookup>)
					return value.Filter == Sampling::Bilinear ? 4 : 1;
				if constexpr (std::is_same_v<T, Noise>)
					return uint64_t(value.Octaves) *
						   (value.Type == NoiseType::Gabor ? 9 * uint64_t(value.Impulses) : 4);
				if constexpr (std::is_same_v<T, Voronoi>)
					return value.Feature == VoronoiFeature::Edge ? 34 : 9;
				if constexpr (std::is_same_v<T, Wave>)
					return value.Distortion != 0 ? 1 + uint64_t(value.Detail) * 4 : 1;
				if constexpr (std::is_same_v<T, Blur>) return 2 * (2 * uint64_t(std::ceil(value.Radius)) + 1);
				if constexpr (std::is_same_v<T, NormalMap>) return 4;
				if constexpr (std::is_same_v<T, MeshAttribute>) return 1 + uint64_t(value.Padding);
				return 1;
			},
			operation
		);
	}
	// Which input supplies a non-generator's extent: image.lookup takes its Vector input's.
	inline size_t ExtentInput(const Operation &operation) {
		return std::holds_alternative<Lookup>(operation) ? 1 : 0;
	}
	// Generators and resampling operations declare their extent; the rest take the first input's.
	inline ImageExtent Extent(const Operation &operation, const ImageExtent &input, size_t bytesPerPixel) {
		return std::visit(
			[&](const auto &value) -> ImageExtent {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, Solid> || std::is_same_v<T, Resize> ||
							  std::is_same_v<T, Crop> || std::is_same_v<T, Transform> ||
							  std::is_same_v<T, Noise> || std::is_same_v<T, Voronoi> ||
							  std::is_same_v<T, Gradient> || std::is_same_v<T, Wave> ||
							  std::is_same_v<T, MeshAttribute>)
					return {value.Width, value.Height, size_t(value.Width) * value.Height * bytesPerPixel};
				else
					return {input.Width, input.Height, size_t(input.Width) * input.Height * bytesPerPixel};
			},
			operation
		);
	}
	// Whether every connected input must share the first input's canvas.
	inline bool MatchingInputs(const Operation &operation) {
		return std::holds_alternative<Blend>(operation) || std::holds_alternative<Math>(operation) ||
			   std::holds_alternative<Channels>(operation) || std::holds_alternative<Warp>(operation);
	}
	// A depth-first order over the nodes `target` reaches that keeps few images live:
	// at each node the input whose subtree peaks highest above what it leaves runs first.
	inline std::vector<size_t>
	LiveOrder(const Plan &plan, size_t target, const std::vector<ImageExtent> &extents) {
		std::vector<size_t> peak(extents.size());
		std::vector<uint8_t> needed(extents.size());
		std::vector<std::vector<size_t>> inputs(extents.size());
		needed[target] = 1;
		for (auto node = plan.Order.rbegin(); node != plan.Order.rend(); ++node)
			if (needed[*node])
				for (const size_t input : plan.Inputs[*node])
					needed[input] = 1;
		for (const size_t node : plan.Order) {
			if (!needed[node]) continue;
			inputs[node] = plan.Inputs[node];
			std::stable_sort(inputs[node].begin(), inputs[node].end(), [&](size_t a, size_t b) {
				return peak[a] - extents[a].Bytes > peak[b] - extents[b].Bytes;
			});
			size_t held = 0;
			for (const size_t input : inputs[node]) {
				peak[node] = std::max(peak[node], held + peak[input]);
				held += extents[input].Bytes;
			}
			peak[node] = std::max(peak[node], held + extents[node].Bytes);
		}
		std::vector<size_t> order;
		std::vector<uint8_t> visited(extents.size());
		const std::function<void(size_t)> visit = [&](size_t node) {
			if (visited[node]) return;
			visited[node] = 1;
			for (const size_t input : inputs[node])
				visit(input);
			order.push_back(node);
		};
		visit(target);
		return order;
	}
	// Readers of each node inside `order`, counting one per connected input.
	inline std::vector<uint32_t> Readers(const Plan &plan, const std::vector<size_t> &order) {
		std::vector<uint32_t> readers(plan.Inputs.size());
		for (const size_t node : order)
			for (const size_t input : plan.Inputs[node])
				++readers[input];
		return readers;
	}
	// Peak bytes when `order` runs and each image is freed after its last reader.
	inline size_t
	LiveBytes(const Plan &plan, const std::vector<size_t> &order, const std::vector<ImageExtent> &extents) {
		std::vector<uint32_t> readers = Readers(plan, order);
		size_t live = 0, peak = 0;
		for (const size_t node : order) {
			live += extents[node].Bytes;
			peak = std::max(peak, live);
			for (const size_t input : plan.Inputs[node])
				if (--readers[input] == 0) live -= extents[input].Bytes;
		}
		return peak;
	}
}
