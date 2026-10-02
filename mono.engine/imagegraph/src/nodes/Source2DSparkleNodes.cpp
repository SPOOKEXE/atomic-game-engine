#include "Processor.hpp"
#include "SourceBuiltinRandomContext.hpp"

#include <algorithm>
#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		struct BuiltinDrawReader {
			NodeContext &Context;
			const SourceBuiltinRandomCapture &Capture;
			size_t Cursor = 0;
			double Read(SourceBuiltinRandomOperation operation, double lower, double upper) {
				if (Cursor >= Capture.Draws.size()) {
					Context.Fail(
						Status::UnsupportedExecution, "MK Sparkle builtin RNG recording is incomplete", "seed"
					);
					return 0;
				}
				const auto &draw = Capture.Draws[Cursor++];
				const double minimum = std::min(lower, upper), maximum = std::max(lower, upper);
				const bool integer = operation != SourceBuiltinRandomOperation::Random;
				if (draw.Operation != operation || draw.Lower != lower || draw.Upper != upper ||
					!std::isfinite(draw.Result) || (integer && std::trunc(draw.Result) != draw.Result) ||
					draw.Result < (integer ? std::floor(minimum) : minimum) ||
					(integer ? draw.Result > std::floor(maximum) : draw.Result >= maximum)) {
					Context.Fail(
						Status::InvalidValue,
						"MK Sparkle builtin RNG draw does not match its source call",
						"seed"
					);
					return 0;
				}
				return draw.Result;
			}
		};
		void Over(Image &target, uint32_t x, uint32_t y, const Rgba &source, bool additive = false) {
			const Rgba previous = ReadPixel(target, x, y);
			Rgba result{};
			for (size_t channel = 0; channel < 4; ++channel)
				result[channel] =
					source[channel] * source[3] + previous[channel] * (additive ? 1 : 1 - source[3]);
			WritePixel(target, x, y, result);
		}
		// Pixel centers and a one-pixel butt-ended segment form the bounded CPU reference profile.
		bool Line(
			NodeContext &context,
			Image &target,
			Vector2 a,
			Vector2 b,
			const Rgba &colour,
			bool additive,
			uint64_t &work
		) {
			if (!std::isfinite(a.X) || !std::isfinite(a.Y) || !std::isfinite(b.X) || !std::isfinite(b.Y))
				return context.Fail(Status::InvalidValue, "MK Sparkle line geometry is nonfinite");
			const double length = std::hypot(b.X - a.X, b.Y - a.Y);
			if (length == 0) return true;
			const uint32_t
				left = uint32_t(std::clamp(std::floor(std::min(a.X, b.X) - .5), 0., double(target.Width))),
				right = uint32_t(std::clamp(std::ceil(std::max(a.X, b.X) + .5), 0., double(target.Width))),
				top = uint32_t(std::clamp(std::floor(std::min(a.Y, b.Y) - .5), 0., double(target.Height))),
				bottom = uint32_t(std::clamp(std::ceil(std::max(a.Y, b.Y) + .5), 0., double(target.Height)));
			const uint64_t count = uint64_t(right - left) * (bottom - top);
			if (count > 64000000 - work)
				return context.Fail(Status::LimitExceeded, "MK Sparkle line raster exceeds work budget");
			work += count;
			const double dx = (b.X - a.X) / length, dy = (b.Y - a.Y) / length;
			for (uint32_t y = top; y < bottom; ++y)
				for (uint32_t x = left; x < right; ++x) {
					const double px = x + .5 - a.X, py = y + .5 - a.Y;
					const double along = px * dx + py * dy, perpendicular = -px * dy + py * dx;
					if (along >= 0 && along < length && perpendicular >= -.5 && perpendicular < .5)
						Over(target, x, y, colour, additive);
				}
			return true;
		}
		Rgba PixelOrClear(const Image &image, int64_t x, int64_t y) {
			if (x < 0 || y < 0 || x >= image.Width || y >= image.Height) return {};
			return ReadPixel(image, uint32_t(x), uint32_t(y));
		}
	}
	bool MkSparkle(NodeContext &context) {
		if (context.Request.RequireSourceGpuRasterCoverage)
			return context.Fail(
				Status::UnsupportedExecution,
				"MK Sparkle source builtin GPU edge coverage requires a renderer observation"
			);
		const int64_t sizeValue = context.Integer("size", 5);
		if (sizeValue < 1 || sizeValue > Limits::MaximumDimension ||
			sizeValue > context.Request.MaximumImageDimension)
			return context.Fail(
				Status::LimitExceeded, "MK Sparkle size must fit the bounded native surface", "size"
			);
		if (!context.Find("seed"))
			return context.Fail(Status::InvalidValue, "MK Sparkle requires its resolved source seed", "seed");
		const SourceBuiltinRandomCapture *capture = nullptr;
		if (!FindSourceBuiltinRandomCapture(context, capture)) return false;
		const Value *paletteValue = context.Find("colors");
		const auto *palette = paletteValue ? std::get_if<ArrayValue>(paletteValue) : nullptr;
		if (!palette || palette->Elements.empty() || palette->Elements.size() > 256 ||
			!palette->Items.empty() || !palette->Nested.empty())
			return context.Fail(
				Status::InvalidValue, "MK Sparkle palette requires one to 256 colors", "colors"
			);
		for (const auto &item : palette->Elements)
			if (!std::holds_alternative<Colour>(item))
				return context.Fail(Status::TypeMismatch, "MK Sparkle palette requires colors", "colors");
		const bool array = context.Boolean("array");
		const int64_t lengthValue = array ? context.Integer("array_length", 1) : 1;
		if (lengthValue < 0 || uint64_t(lengthValue) > Limits::MaximumArrayElements)
			return context.Fail(
				Status::LimitExceeded, "MK Sparkle array length exceeds bounded elements", "array_length"
			);
		const uint32_t size = uint32_t(sizeValue), quarterSize = (size + 1) / 2, halfPosition = size / 2;
		const size_t count = size_t(lengthValue);
		if (uint64_t(size) * size * (count + 3) > 64000000)
			return context.Fail(Status::LimitExceeded, "MK Sparkle symmetry raster exceeds work budget");
		const uint64_t frameBytes = uint64_t(size) * size * 4;
		auto scratchCharge =
			context.ReserveWorkspace(uint64_t(quarterSize) * quarterSize * 4 + frameBytes * 2);
		if (!scratchCharge) return false;
		Image quarter{
			quarterSize, quarterSize, std::vector<uint8_t>(size_t(quarterSize) * quarterSize * 4), 0
		},
			horizontal{size, size, std::vector<uint8_t>(size_t(frameBytes)), 0},
			vertical{size, size, std::vector<uint8_t>(size_t(frameBytes)), 0};
		ImageArray frames;
		if (array) {
			if (!context.ReserveOutput(
					count * (sizeof(Image) + sizeof(ImageArrayItem)) + count * frameBytes, "surface_out"
				))
				return false;
			frames.Images.reserve(count);
			frames.Items.reserve(count);
		}
		uint64_t work = 0;
		for (size_t frameIndex = 0; frameIndex < count; ++frameIndex) {
			BuiltinDrawReader draws{context, *capture};
			double frame = array ? double(frameIndex)
								 : (double(context.Request.Tick) + context.Request.Subframe) *
									   (context.Request.NegativeFrame ? -1 : 1);
			const int64_t loop = context.Integer("loop");
			const double loopLength = context.Integer("loop_length", 4);
			if (loop != 0) {
				const double period = loop == 1 ? loopLength : 2 * loopLength;
				frame = period == 0 ? 0 : std::fmod(frame, period);
				if (loop == 2 && frame >= loopLength) frame = 2 * loopLength - 2 - frame;
			}
			frame = frame * context.Scalar("speed", 1) - context.Integer("frame_shift");
			std::fill(quarter.Pixels.begin(), quarter.Pixels.end(), 0);
			std::fill(horizontal.Pixels.begin(), horizontal.Pixels.end(), 0);
			std::fill(vertical.Pixels.begin(), vertical.Pixels.end(), 0);
			const double amountValue = draws.Read(
				SourceBuiltinRandomOperation::IRandom, 0, halfPosition * context.Scalar("amount", .5)
			);
			if (context.FailureCode != Status::Ok) return false;
			if (amountValue < 0 || amountValue > 65535 - 3)
				return context.Fail(
					Status::UnsupportedExecution,
					"MK Sparkle observed repeat count is outside the bounded source profile",
					"amount"
				);
			const uint32_t amount = uint32_t(3 + amountValue);
			const double scatter = 25 - 24 * std::pow(context.Scalar("scatter", .5), .1);
			const double diagonal = context.Scalar("diagonal", .2);
			for (uint32_t index = 0; index < amount; ++index) {
				Rgba colour{1, 1, 1, 1};
				if (context.Boolean("shade")) {
					const size_t selected =
						size_t((palette->Elements.size() - 1) * double(index) / (amount - 1));
					const auto &value = std::get<Colour>(palette->Elements[selected]);
					colour = {value.Red / 255., value.Green / 255., value.Blue / 255., 1};
				}
				const double dy = std::pow(draws.Read(SourceBuiltinRandomOperation::Random, 0, 1), scatter) *
								  (halfPosition / 2.),
							 dx = std::pow(draws.Read(SourceBuiltinRandomOperation::Random, 0, 1), scatter) *
								  (halfPosition / 2.),
							 speed =
								 draws.Read(SourceBuiltinRandomOperation::IRandomRange, 1, halfPosition / 4.),
							 delta = -draws.Read(
								 SourceBuiltinRandomOperation::IRandomRange, 1, halfPosition / 4.
							 ),
							 initial =
								 draws.Read(SourceBuiltinRandomOperation::IRandomRange, 1, halfPosition / 2.),
							 length = std::max(0., initial + frame * delta);
				const bool diamond = draws.Read(SourceBuiltinRandomOperation::Random, 0, 1) < diagonal * .2,
						   diagonalLine = draws.Read(SourceBuiltinRandomOperation::Random, 0, 1) < diagonal;
				if (context.FailureCode != Status::Ok) return false;
				if (length <= 0) continue;
				Vector2 start{-1 + dx + frame * speed, quarterSize - 1 - dy - frame * speed};
				Vector2 end{start.X + length, start.Y};
				if (diamond) {
					start.X = -1 + dx - frame * speed;
					end = {start.X - length, start.Y - length};
				} else if (diagonalLine)
					end = {start.X + length, start.Y - length};
				else {
					start.Y = quarterSize - 1 - dy;
					end.Y = start.Y;
				}
				if (!Line(context, quarter, start, end, colour, context.Boolean("additive"), work))
					return false;
			}
			if (draws.Cursor != capture->Draws.size())
				return context.Fail(
					Status::InvalidValue, "MK Sparkle builtin RNG recording has trailing draws", "seed"
				);
			Image *output = nullptr;
			if (array) {
				frames.Items.push_back({frames.Images.size()});
				frames.Images.push_back({size, size, std::vector<uint8_t>(size_t(frameBytes)), 0});
				output = &frames.Images.back();
			} else
				output = context.NewImage("surface_out", size, size, SurfaceFormat::RGBA8Unorm);
			if (!output) return false;
			for (uint32_t y = 0; y < size; ++y)
				for (uint32_t x = 0; x < size; ++x) {
					Over(horizontal, x, y, PixelOrClear(quarter, int64_t(x) - halfPosition, y));
					Over(horizontal, x, y, PixelOrClear(quarter, int64_t(quarterSize) - 1 - x, y));
				}
			for (uint32_t y = 0; y < size; ++y)
				for (uint32_t x = 0; x < size; ++x) {
					Over(vertical, x, y, ReadPixel(horizontal, x, y));
					Over(vertical, x, y, ReadPixel(horizontal, x, size - 1 - y));
				}
			for (uint32_t y = 0; y < size; ++y)
				for (uint32_t x = 0; x < size; ++x) {
					Over(*output, x, y, ReadPixel(vertical, x, y));
					Over(*output, x, y, ReadPixel(vertical, size - 1 - y, x));
				}
		}
		if (array) context.OutputImageArrays.emplace_back("surface_out", std::move(frames));
		return context.FailureCode == Status::Ok;
	}
}
