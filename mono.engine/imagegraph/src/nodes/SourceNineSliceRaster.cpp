#include "../SourceNineSlice.hpp"
#include "Sampler.hpp"

#include <algorithm>
#include <cmath>

namespace engine::imagegraph::detail {
	bool StageSourceNineSlice(
		NodeContext &context,
		const SourceNineSliceRecipe &recipe,
		Vector2 dimension,
		Image &target,
		std::array<double, 4> tint,
		bool render
	) {
		const auto &source = recipe.Source;
		const auto p = recipe.Splice;
		for (double value : {p.X, p.Y, p.Z, p.W, dimension.X, dimension.Y})
			if (!std::isfinite(value) || std::abs(value) > Limits::MaximumDimension * 4)
				return context.Fail(
					Status::LimitExceeded, "Nine Slice geometry exceeds bounded finite dimensions"
				);
		if (recipe.FillingMode < 0 || recipe.FillingMode > 1)
			return context.Fail(Status::InvalidValue, "Nine Slice filling mode is invalid");
		const double left = p.Z, top = p.Y, right = p.X, bottom = p.W;
		const double centerWidth = std::max(double(source.Width) - left - right, 1.),
					 centerHeight = std::max(double(source.Height) - top - bottom, 1.);
		const double outputWidth = dimension.X - left - right, outputHeight = dimension.Y - top - bottom;
		if (!source.Width || !source.Height || !target.Width || !target.Height ||
			target.Format != SurfaceFormat::RGBA8Unorm ||
			!ValidSurfaceLayout(source, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
			(render && !ValidSurfaceLayout(target, Limits::MaximumDimension, Limits::MaximumEvaluationBytes)))
			return context.Fail(
				Status::InvalidValue, "Nine Slice staging requires nonempty input and RGBA8 target"
			);
		// Signed builtin part rectangles are outside the native positive-rectangle
		// raster profile. They must not be silently replaced by normalized splices.
		if (left < 0 || top < 0 || right < 0 || bottom < 0 || outputWidth < 0 || outputHeight < 0)
			return context.Fail(
				Status::UnsupportedExecution,
				"Nine Slice signed part geometry requires licensed raster coverage",
				"splice"
			);
		const double columns = std::ceil(outputWidth / centerWidth),
					 rows = std::ceil(outputHeight / centerHeight);
		const uint64_t pixels = uint64_t(target.Width) * target.Height;
		const uint64_t batches = std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
		const uint64_t sampleCost = recipe.Interpolation == 4	? 144
									: recipe.Interpolation == 3 ? 64
									: recipe.Interpolation == 2 ? 4
																: 1;
		const uint64_t pixelCost = 9 * sampleCost + 1;
		const double tileVisits = recipe.FillingMode == 1 ? columns * rows + columns + rows : 0;
		if (!std::isfinite(tileVisits) || tileVisits > 64000000 || pixels > 64000000 / pixelCost ||
			(pixelCost * pixels + uint64_t(tileVisits) * 2) > 64000000 / batches)
			return context.Fail(
				Status::LimitExceeded, "Nine Slice whole processor batch exceeds work budget"
			);
		const SamplerSettings sampler{recipe.Interpolation, recipe.Oversample};
		if (!SupportedSampler(context, sampler)) return false;
		if (!render) return true;
		const Rgba multiply = tint;
		const auto drawPart = [&](double sourceX,
								  double sourceY,
								  double width,
								  double height,
								  double x,
								  double y,
								  double scaleX,
								  double scaleY) {
			if (width == 0 || height == 0 || scaleX == 0 || scaleY == 0) return true;
			const double endX = x + width * scaleX, endY = y + height * scaleY;
			const uint32_t firstX = uint32_t(std::clamp(std::ceil(x - .5), 0., double(target.Width)));
			const uint32_t firstY = uint32_t(std::clamp(std::ceil(y - .5), 0., double(target.Height)));
			const uint32_t lastX = uint32_t(std::clamp(std::ceil(endX - .5), 0., double(target.Width)));
			const uint32_t lastY = uint32_t(std::clamp(std::ceil(endY - .5), 0., double(target.Height)));
			for (uint32_t yy = firstY; yy < lastY; ++yy)
				for (uint32_t xx = firstX; xx < lastX; ++xx) {
					const double u = (sourceX + (xx + .5 - x) / scaleX) / source.Width,
								 v = (sourceY + (yy + .5 - y) / scaleY) / source.Height;
					Rgba value;
					if (DescribeSurfaceFormat(source.Format)->Channels == 1) {
						const auto sample = Texture(source, u, v, Filtered(sampler));
						value = {sample[0], sample[0], sample[0], 1};
					} else
						value = SampleTexture(source, u, v, sampler);
					for (size_t channel = 0; channel < 4; ++channel)
						value[channel] *= multiply[channel];
					if (!WritePixel(target, xx, yy, value)) return false;
				}
			return true;
		};
		if (!drawPart(0, 0, left, top, 0, 0, 1, 1) ||
			!drawPart(source.Width - right, 0, right, top, dimension.X - right, 0, 1, 1) ||
			!drawPart(0, source.Height - bottom, left, bottom, 0, dimension.Y - bottom, 1, 1) ||
			!drawPart(
				source.Width - right,
				source.Height - bottom,
				right,
				bottom,
				dimension.X - right,
				dimension.Y - bottom,
				1,
				1
			))
			return context.Fail(Status::InvalidValue, "Nine Slice corner staging exceeds numeric range");
		if (recipe.FillingMode == 0) {
			const double sx = outputWidth / centerWidth, sy = outputHeight / centerHeight;
			if (!drawPart(left, 0, centerWidth, top, left, 0, sx, 1) ||
				!drawPart(
					left, source.Height - bottom, centerWidth, bottom, left, dimension.Y - bottom, sx, 1
				) ||
				!drawPart(0, top, left, centerHeight, 0, top, 1, sy) ||
				!drawPart(source.Width - right, top, right, centerHeight, dimension.X - right, top, 1, sy) ||
				!drawPart(left, top, centerWidth, centerHeight, left, top, sx, sy))
				return context.Fail(Status::InvalidValue, "Nine Slice scaled staging exceeds numeric range");
		} else {
			for (uint64_t c = 0; c < uint64_t(columns); ++c) {
				const double x = left + c * centerWidth,
							 width = std::min(centerWidth, dimension.X - right - x);
				if (!drawPart(left, 0, width, top, x, 0, 1, 1) ||
					!drawPart(left, source.Height - bottom, width, bottom, x, dimension.Y - bottom, 1, 1))
					return context.Fail(
						Status::InvalidValue, "Nine Slice repeated edge staging exceeds numeric range"
					);
			}
			for (uint64_t r = 0; r < uint64_t(rows); ++r) {
				const double y = top + r * centerHeight,
							 height = std::min(centerHeight, dimension.Y - bottom - y);
				if (!drawPart(0, top, left, height, 0, y, 1, 1) ||
					!drawPart(source.Width - right, top, right, height, dimension.X - right, y, 1, 1))
					return context.Fail(
						Status::InvalidValue, "Nine Slice repeated edge staging exceeds numeric range"
					);
			}
			for (uint64_t c = 0; c < uint64_t(columns); ++c)
				for (uint64_t r = 0; r < uint64_t(rows); ++r) {
					const double x = left + c * centerWidth, y = top + r * centerHeight;
					if (!drawPart(
							left,
							top,
							std::min(centerWidth, dimension.X - right - x),
							std::min(centerHeight, dimension.Y - bottom - y),
							x,
							y,
							1,
							1
						))
						return context.Fail(
							Status::InvalidValue, "Nine Slice repeated center staging exceeds numeric range"
						);
				}
		}
		return true;
	}
}
