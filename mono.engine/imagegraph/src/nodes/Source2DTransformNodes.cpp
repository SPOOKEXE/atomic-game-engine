#include "Sampler.hpp"
#include "Source2DMath.hpp"

namespace engine::imagegraph::detail {
	using source2d::PixelPosition;
	using source2d::Rotate;

	double SourceRound(double value) {
		const double floor = std::floor(value), fraction = value - floor;
		return floor + (fraction > 0.5 || (fraction == 0.5 && std::fmod(floor, 2.0) != 0.0));
	}

	bool SurfaceSize(NodeContext &context, double x, double y, uint32_t &width, uint32_t &height) {
		if (!std::isfinite(x) || !std::isfinite(y))
			return context.Fail(Status::InvalidValue, "Surface dimensions must be finite");
		x = std::max(1.0, SourceRound(x));
		y = std::max(1.0, SourceRound(y));
		if (x > Limits::MaximumDimension || y > Limits::MaximumDimension)
			return context.Fail(Status::LimitExceeded, "Surface dimensions exceed native limits");
		width = static_cast<uint32_t>(x);
		height = static_cast<uint32_t>(y);
		return true;
	}

	bool Scale(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source) return context.Fail(Status::InvalidValue, "Surface In is required", "surface_in");
		const int64_t mode = context.Integer("mode"), fit = context.Integer("fit_mode");
		if (mode < 0 || mode > 1 || fit < 0 || fit > 2)
			return context.Fail(Status::InvalidValue, "Scale mode is invalid");
		double x = source->Width * context.Scalar("scale", 1),
			   y = source->Height * context.Scalar("scale", 1);
		if (mode == 1) {
			Vector2 target = context.Vec2("target_dimension", {32, 32});
			if (!context.IsLinked("target_dimension") && context.Integer("target_dimension_unit") == 1) {
				target.X *= source->Width;
				target.Y *= source->Height;
			}
			x = target.X;
			y = target.Y;
			if (fit != 0) {
				const double factor = fit == 1 ? std::min(x / source->Width, y / source->Height)
											   : std::max(x / source->Width, y / source->Height);
				x = factor * source->Width;
				y = factor * source->Height;
			}
		}
		uint32_t width = 0, height = 0;
		if (!SurfaceSize(context, x, y, width, height)) return false;
		const SamplerSettings sampler = ReadSampler(context);
		if (!SupportedSampler(context, sampler)) return false;
		const auto format = ResolveProcessorSurfaceFormat(context, source);
		if (!format) return false;
		Image *output = context.NewImage("surface_out", width, height, *format);
		if (!output) return false;
		for (uint32_t row = 0; row < height; ++row)
			for (uint32_t column = 0; column < width; ++column) {
				if (!WritePixel(
						*output,
						column,
						row,
						SampleTexture(*source, (column + 0.5) / width, (row + 0.5) / height, sampler)
					))
					return context.Fail(
						Status::InvalidValue, "Scaled sample exceeds surface range", "surface_out"
					);
			}
		return true;
	}

	bool Crop(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source) return context.Fail(Status::InvalidValue, "Surface In is required", "surface_in");
		const int64_t aspect = context.Integer("aspect_ratio"), fit = context.Integer("fit_mode");
		if (aspect < 0 || aspect > 5 || fit < 0 || fit > 3)
			return context.Fail(Status::InvalidValue, "Crop mode is invalid");
		double x = 0, y = 0, requestedWidth = source->Width, requestedHeight = source->Height;
		if (aspect == 0) {
			Vector4 padding = context.Get<Vector4>("crop", {});
			if (!context.IsLinked("crop") && context.Integer("crop_unit", 1) == 1) {
				padding.X *= source->Width;
				padding.Z *= source->Width;
				padding.Y *= source->Height;
				padding.W *= source->Height;
			}
			requestedWidth -= padding.X + padding.Z;
			requestedHeight -= padding.Y + padding.W;
			x = padding.Z;
			y = padding.Y;
		} else {
			const Vector2 authoredRatio = context.Vec2("ratio", {1, 1});
			const double ratio = aspect == 1   ? authoredRatio.X / authoredRatio.Y
								 : aspect == 2 ? 1.0
								 : aspect == 3 ? 1.5
								 : aspect == 4 ? 4.0 / 3.0
											   : 16.0 / 9.0;
			if (!(ratio > 0) || !std::isfinite(ratio))
				return context.Fail(Status::InvalidValue, "Crop ratio must be positive", "ratio");
			if (fit == 0) {
				requestedWidth = std::abs(context.Scalar("width", 8));
				requestedHeight = std::ceil(requestedWidth / ratio);
				const Vector2 center = PixelPosition(context, "center", source->Width, source->Height);
				x = SourceRound(center.X * source->Width - requestedWidth / 2);
				y = SourceRound(center.Y * source->Height - requestedHeight / 2);
			} else {
				if (fit == 1) {
					requestedWidth = source->Width;
					requestedHeight = requestedWidth * ratio;
				} else if (fit == 2) {
					requestedHeight = source->Height;
					requestedWidth = requestedHeight / ratio;
				} else {
					requestedWidth = std::min(double(source->Width), source->Height * ratio);
					requestedHeight = requestedWidth * ratio;
				}
				x = SourceRound(source->Width / 2.0 - requestedWidth / 2.0);
				y = SourceRound(source->Height / 2.0 - requestedHeight / 2.0);
			}
		}
		uint32_t width = 0, height = 0;
		if (!SurfaceSize(context, requestedWidth, requestedHeight, width, height)) return false;
		Image *output = context.NewImage("surface_out", width, height, SurfaceFormat::RGBA8Unorm);
		if (!output) return false;
		for (uint32_t row = 0; row < height; ++row)
			for (uint32_t column = 0; column < width; ++column) {
				const double sourceX = column + 0.5 + x, sourceY = row + 0.5 + y;
				if (sourceX < 0 || sourceY < 0 || sourceX >= source->Width || sourceY >= source->Height)
					continue;
				if (!WritePixel(
						*output,
						column,
						row,
						SampleNearest(*source, sourceX / source->Width, sourceY / source->Height)
					))
					return context.Fail(
						Status::InvalidValue, "Cropped sample exceeds surface range", "surface_out"
					);
			}
		return true;
	}

}
