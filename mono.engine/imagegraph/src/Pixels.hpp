#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>

namespace engine::imagegraph::detail {
	using Pixel = std::array<double, 4>;
	inline Pixel At(const Image &image, int64_t x, int64_t y, bool clamp = false) {
		if (clamp) {
			x = std::clamp<int64_t>(x, 0, image.Width - 1);
			y = std::clamp<int64_t>(y, 0, image.Height - 1);
		}
		if (x < 0 || y < 0 || x >= image.Width || y >= image.Height) return {};
		const size_t offset = (static_cast<size_t>(y) * image.Width + static_cast<size_t>(x)) * 4;
		Pixel pixel{};
		for (size_t channel = 0; channel < 4; channel++)
			pixel[channel] = std::to_integer<uint8_t>(image.Pixels[offset + channel]);
		return pixel;
	}
	inline void Put(Image &image, uint32_t x, uint32_t y, const Pixel &pixel) {
		const size_t offset = (static_cast<size_t>(y) * image.Width + x) * 4;
		for (size_t channel = 0; channel < 4; channel++)
			image.Pixels[offset + channel] =
				std::byte{static_cast<uint8_t>(std::lround(std::clamp(pixel[channel], 0.0, 255.0)))};
	}
	// Coordinates address pixel centers. Bilinear interpolation weights premultiplied
	// channels so hidden colour in a transparent pixel cannot create an edge fringe.
	inline Pixel Sample(const Image &image, double x, double y, Sampling filter, bool clamp = false) {
		if (!std::isfinite(x) || !std::isfinite(y)) return {};
		if (filter == Sampling::Nearest) {
			if (x < 0 || y < 0 || x >= image.Width || y >= image.Height) return {};
			return At(image, static_cast<int64_t>(std::floor(x)), static_cast<int64_t>(std::floor(y)));
		}
		if (x < -0.5 || y < -0.5 || x > image.Width + 0.5 || y > image.Height + 0.5) return {};
		x -= 0.5;
		y -= 0.5;
		const int64_t left = static_cast<int64_t>(std::floor(x));
		const int64_t top = static_cast<int64_t>(std::floor(y));
		const double across = x - left, down = y - top;
		Pixel total{};
		for (int64_t row = 0; row < 2; row++)
			for (int64_t column = 0; column < 2; column++) {
				const Pixel pixel = At(image, left + column, top + row, clamp);
				const double weight = (column == 0 ? 1 - across : across) * (row == 0 ? 1 - down : down);
				const double alpha = pixel[3] / 255.0;
				for (size_t channel = 0; channel < 3; channel++)
					total[channel] += pixel[channel] * alpha * weight;
				total[3] += pixel[3] * weight;
			}
		if (total[3] > 0)
			for (size_t channel = 0; channel < 3; channel++)
				total[channel] *= 255.0 / total[3];
		return total;
	}
	inline Pixel Over(const Pixel &background, const Pixel &foreground, double opacity) {
		const double front = foreground[3] / 255.0 * opacity;
		const double back = background[3] / 255.0 * (1 - front);
		const double alpha = front + back;
		if (alpha == 0) return {};
		Pixel pixel{};
		for (size_t channel = 0; channel < 3; channel++)
			pixel[channel] = (foreground[channel] * front + background[channel] * back) / alpha;
		pixel[3] = alpha * 255.0;
		return pixel;
	}
}
