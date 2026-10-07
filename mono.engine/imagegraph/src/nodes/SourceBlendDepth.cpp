#include "Families.hpp"
#include "SourceRefractClean.hpp"

namespace engine::imagegraph::detail {
	namespace {
		template <class V> uint64_t BlendDepthPixels(const V &value) {
			if (const auto *surface = std::get_if<SurfaceValue>(&value))
				return uint64_t(surface->Data.Width) * surface->Data.Height;
			if (const auto *atlas = std::get_if<AtlasValue>(&value); atlas && atlas->Data)
				return uint64_t(atlas->Data->Surface.Data.Width) * atlas->Data->Surface.Data.Height;
			if constexpr (std::is_same_v<V, Value>) {
				if (const auto *array = std::get_if<ArrayValue>(&value)) {
					uint64_t largest = 0;
					for (const auto &item : array->Elements)
						largest = std::max(largest, BlendDepthPixels(item));
					for (const auto &row : array->Nested)
						for (const auto &item : row)
							largest = std::max(largest, BlendDepthPixels(item));
					const auto visit =
						[&](auto &&self, const std::vector<SourceArrayItem> &items, size_t depth) -> void {
						if (depth > Limits::MaximumArrayDepth) {
							largest = UINT64_MAX;
							return;
						}
						for (const auto &item : items) {
							if (const auto *image = std::get_if<Image>(&item.Data))
								largest = std::max(largest, uint64_t(image->Width) * image->Height);
							else if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
								largest = std::max(largest, BlendDepthPixels(*leaf));
							else
								self(self, std::get<std::vector<SourceArrayItem>>(item.Data), depth + 1);
						}
					};
					visit(visit, array->Items, 0);
					return largest;
				}
			}
			return 0;
		}
		template <class V> uint64_t BlendDepthSampleWork(const V &value, int64_t inherited) {
			double mode = 6;
			if (const auto *v = std::get_if<EnumValue>(&value)) mode = double(v->Value);
			if (const auto *v = std::get_if<int64_t>(&value)) mode = double(*v);
			if (const auto *v = std::get_if<double>(&value)) mode = *v;
			if constexpr (std::is_same_v<V, Value>) {
				if (const auto *array = std::get_if<ArrayValue>(&value)) {
					uint64_t work = 64;
					for (const auto &item : array->Elements)
						work = std::max(work, BlendDepthSampleWork(item, inherited));
					for (const auto &row : array->Nested)
						for (const auto &item : row)
							work = std::max(work, BlendDepthSampleWork(item, inherited));
					if (!array->Items.empty()) return 4096;
					return work;
				}
			}
			if (mode == 0) mode = double(inherited);
			return mode == 6 || !std::isfinite(mode) ? 4096 : mode == 4 ? 256 : 64;
		}

		struct DepthTransform {
			Vector2 Position{}, Anchor{.5, .5}, Scale{1, 1}, Range{0, 1};
			double Cosine = 1, Sine = 0;
		};
		bool
		ReadDepthTransform(NodeContext &context, size_t index, const Image &source, DepthTransform &out) {
			const std::string suffix = std::to_string(index);
			for (const auto prefix : {"position_", "anchor_", "scale_", "depth_range_"}) {
				const std::string port = prefix + suffix;
				if (const auto *value = context.Find(port);
					value && !std::holds_alternative<Vector2>(*value) &&
					!std::holds_alternative<double>(*value) && !std::holds_alternative<int64_t>(*value))
					return context.Fail(Status::InvalidValue, "Blend Depth requires a numeric vector", port);
			}
			out.Position = context.Vec2("position_" + suffix);
			out.Anchor = context.Vec2("anchor_" + suffix, {.5, .5});
			out.Scale = context.Vec2("scale_" + suffix, {1, 1});
			out.Range = context.Vec2("depth_range_" + suffix, {0, 1});
			const int64_t unit = context.Integer("position_" + suffix + "_unit", 1);
			if (unit < 0 || unit > 1)
				return context.Fail(
					Status::InvalidValue,
					"Blend Depth position unit is invalid",
					"position_" + suffix + "_unit"
				);
			out.Position.X /= unit ? 1. : source.Width;
			out.Position.Y /= unit ? 1. : source.Height;
			const std::string rotationPort = "rotation_" + suffix;
			if (const auto *value = context.Find(rotationPort);
				value && !std::holds_alternative<double>(*value) && !std::holds_alternative<int64_t>(*value))
				return context.Fail(
					Status::InvalidValue, "Blend Depth rotation requires a number", rotationPort
				);
			const double radians = context.Scalar(rotationPort) * std::numbers::pi / 180;
			out.Cosine = std::cos(radians);
			out.Sine = std::sin(radians);
			for (const auto &[port, vector] : std::array<std::pair<std::string, Vector2>, 4>{
					 {{"position_" + suffix, out.Position},
					  {"anchor_" + suffix, out.Anchor},
					  {"scale_" + suffix, out.Scale},
					  {"depth_range_" + suffix, out.Range}}
				 })
				if (!std::isfinite(vector.X) || !std::isfinite(vector.Y))
					return context.Fail(
						Status::InvalidValue, "Blend Depth transform or range is nonfinite", port
					);
			if (!std::isfinite(radians))
				return context.Fail(Status::InvalidValue, "Blend Depth rotation is nonfinite", rotationPort);
			if (!out.Scale.X || !out.Scale.Y)
				return context.Fail(
					Status::UnsupportedExecution,
					"Blend Depth zero scale has undefined source coordinates",
					"scale_" + suffix
				);
			return context.FailureCode == Status::Ok;
		}
		Vector2 DepthCoordinate(const DepthTransform &transform, double u, double v) {
			const double x = u - transform.Anchor.X, y = v - transform.Anchor.Y;
			return {
				(x * transform.Cosine + y * transform.Sine) / transform.Scale.X + transform.Anchor.X -
					transform.Position.X,
				(-x * transform.Sine + y * transform.Cosine) / transform.Scale.Y + transform.Anchor.Y -
					transform.Position.Y
			};
		}
		double SampleDepth(const Image *image, Vector2 coordinate, Vector2 range) {
			double depth = coordinate.Y;
			if (image) {
				// grug shader_set_s forces nearest on depth stages; plain texture2D ignores Oversample.
				const auto sample = Texture(*image, coordinate.X, coordinate.Y, false);
				depth = (sample[0] + sample[1] + sample[2]) / 3 * sample[3];
			}
			return range.X * (1 - depth) + range.Y * depth;
		}
	}
	bool SourceBlendDepth(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.source.blend_depth");
		std::array<const Image *, 4> surfaces{};
		constexpr std::array<std::string_view, 4> ports{"surface_1", "surface_2", "depth_1", "depth_2"};
		for (size_t index = 0; index < ports.size(); ++index) {
			const auto port = ports[index];
			if (const auto *value = context.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return context.Fail(
					Status::UnsupportedExecution,
					"Blend Depth source texture binding does not support Atlas",
					port
				);
			surfaces[index] = context.Input(port);
			if (surfaces[index] && (!ValidSurfaceLayout(
										*surfaces[index], Limits::MaximumDimension, Limits::MaximumOutputBytes
									) ||
									!FiniteSurfaceSamples(*surfaces[index])))
				return context.Fail(
					Status::InvalidValue, "Blend Depth requires a finite owned surface", port
				);
		}
		const Image *first = surfaces[0], *second = surfaces[1];
		if (!first) return context.Fail(Status::InvalidValue, "Blend Depth requires Surface 1", "surface_1");
		auto sampler = ReadSampler(context);
		sampler.ForceNearest = true;
		if (context.FailureCode != Status::Ok) return false;
		if (context.ProcessorRow == 0) {
			uint64_t pixels = uint64_t(first->Width) * first->Height;
			for (const auto &[port, array] : context.ImageArrays)
				if (port == "surface_1" && array)
					for (const auto &image : array->Images)
						pixels = std::max(pixels, uint64_t(image.Width) * image.Height);
			for (const auto &[port, value] : context.ProcessorOriginalValues)
				if (port == "surface_1" && value) pixels = std::max(pixels, BlendDepthPixels(*value));
			uint64_t sampleWork = sampler.Interpolation == 6 ? 4096 : sampler.Interpolation == 4 ? 256 : 64;
			for (const auto &[port, value] : context.ProcessorOriginalValues)
				if (port == "interpolate" && value)
					sampleWork =
						std::max(sampleWork, BlendDepthSampleWork(*value, context.InheritedInterpolation));
			// grug charge both colour kernels, two depth reads and blend work for every original row.
			const uint64_t work = sampleWork * 2 + 8;
			const uint64_t rows = std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
			if (rows > 64000000 / work || pixels > 64000000 / work / rows)
				return context.Fail(
					Status::LimitExceeded, "Blend Depth complete batch exceeds work limit", "surface_1"
				);
		}
		const int64_t mode = context.Integer("depth_mode");
		if (context.FailureCode != Status::Ok) return false;
		if (mode < 0 || mode > 3)
			return context.Fail(Status::InvalidValue, "Blend Depth comparison mode is invalid", "depth_mode");
		std::array<DepthTransform, 2> transforms;
		if (!ReadDepthTransform(context, 1, *first, transforms[0]) ||
			(second && !ReadDepthTransform(context, 2, *first, transforms[1])))
			return false;
		for (size_t index = 0; index < (second ? 2u : 1u); ++index)
			for (double u : {0., 1.})
				for (double v : {0., 1.}) {
					const auto coordinate = DepthCoordinate(transforms[index], u, v);
					if (!std::isfinite(coordinate.X) || !std::isfinite(coordinate.Y))
						return context.Fail(
							Status::InvalidValue,
							"Blend Depth sampling coordinate is undefined",
							index ? "scale_2" : "scale_1"
						);
				}
		Image *colourOutput = context.NewImage("surface_out", first->Width, first->Height);
		if (!colourOutput) return false;
		Image *depthOutput = context.NewImage("depth_out", first->Width, first->Height);
		if (!depthOutput) return false;
		const Vector2 sampleDimension{double(first->Width), double(first->Height)};
		for (uint32_t y = 0; y < first->Height; ++y)
			for (uint32_t x = 0; x < first->Width; ++x) {
				const double u = (double(x) + .5) / first->Width, v = (double(y) + .5) / first->Height;
				const auto firstCoordinate = DepthCoordinate(transforms[0], u, v);
				auto colour = source_refract_clean::Sample(
					*first, firstCoordinate.X, firstCoordinate.Y, sampler, sampleDimension
				);
				const double firstDepth = SampleDepth(surfaces[2], firstCoordinate, transforms[0].Range);
				double depth = firstDepth;
				if (second) {
					const auto secondCoordinate = DepthCoordinate(transforms[1], u, v);
					const auto secondColour = source_refract_clean::Sample(
						*second, secondCoordinate.X, secondCoordinate.Y, sampler, sampleDimension
					);
					const double secondDepth =
						SampleDepth(surfaces[3], secondCoordinate, transforms[1].Range);
					if (secondColour[3] != 0) {
						const bool pass = mode == 0	  ? firstDepth < secondDepth
										  : mode == 1 ? firstDepth <= secondDepth
										  : mode == 2 ? firstDepth > secondDepth
													  : firstDepth >= secondDepth;
						const auto &foreground = pass ? colour : secondColour;
						const auto &background = pass ? secondColour : colour;
						Rgba blended{};
						// grug source multiplies all four foreground channels by alpha, including alpha
						// itself.
						for (size_t channel = 0; channel < 4; ++channel)
							blended[channel] = background[channel] * (1 - foreground[3]) +
											   foreground[channel] * foreground[3];
						colour = blended;
						depth = std::min(firstDepth, secondDepth);
					}
				}
				if (!std::isfinite(depth) || !std::all_of(colour.begin(), colour.end(), [](double channel) {
						return std::isfinite(channel);
					}))
					return context.Fail(Status::InvalidValue, "Blend Depth sample is nonfinite", "depth_out");
				if (!WritePixel(*colourOutput, x, y, colour) ||
					!WritePixel(*depthOutput, x, y, {depth, depth, depth, 1}))
					return context.Fail(
						Status::InvalidValue, "Blend Depth sample exceeds output range", "surface_out"
					);
			}
		return true;
	}
	std::span<const ExecutorEntry> SourceBlendDepthExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.blend_depth", SourceBlendDepth, true}};
		return entries;
	}
}
