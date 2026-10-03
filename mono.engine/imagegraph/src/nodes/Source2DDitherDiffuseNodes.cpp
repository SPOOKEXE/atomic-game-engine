#include "Processor.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace engine::imagegraph::detail {
	namespace {
		struct DiffusionTap {
			int X;
			int Y;
			double Weight;
		};
		constexpr std::array FLOYD{
			DiffusionTap{1, 0, 7. / 16},
			DiffusionTap{-1, 1, 3. / 16},
			DiffusionTap{0, 1, 5. / 16},
			DiffusionTap{1, 1, 1. / 16}
		};
		// Preserve the pinned JJN coefficients, including its asymmetric second row.
		constexpr std::array JARVIS{
			DiffusionTap{1, 0, 7. / 48},
			DiffusionTap{2, 0, 5. / 48},
			DiffusionTap{-2, 1, 3. / 48},
			DiffusionTap{-1, 1, 1. / 48},
			DiffusionTap{0, 1, 7. / 48},
			DiffusionTap{1, 1, 5. / 48},
			DiffusionTap{2, 1, 3. / 48},
			DiffusionTap{-2, 2, 1. / 48},
			DiffusionTap{-1, 2, 3. / 48},
			DiffusionTap{0, 2, 5. / 48},
			DiffusionTap{1, 2, 3. / 48},
			DiffusionTap{2, 2, 1. / 48}
		};
		constexpr std::array ATKINSON{
			DiffusionTap{1, 0, 1. / 8},
			DiffusionTap{2, 0, 1. / 8},
			DiffusionTap{-1, 1, 1. / 8},
			DiffusionTap{0, 1, 1. / 8},
			DiffusionTap{1, 1, 1. / 8},
			DiffusionTap{0, 2, 1. / 8}
		};
		constexpr std::array LINEAR{DiffusionTap{1, 0, 1}};
		const Value *OriginalInput(const NodeContext &context, std::string_view port) {
			for (auto i = context.ProcessorOriginalValues.rbegin();
				 i != context.ProcessorOriginalValues.rend();
				 ++i)
				if (i->first == port) return i->second;
			for (const auto &[id, value] : context.Values)
				if (id == port) return &value;
			return context.Find(port);
		}
		struct NumericBounds {
			double Minimum = std::numeric_limits<double>::infinity(),
				   Maximum = -std::numeric_limits<double>::infinity();
			bool Found = false;
		};
		template <class Leaf> void BoundNumber(const Leaf &leaf, NumericBounds &out) {
			const auto add = [&](double n) {
				out.Found = true;
				out.Minimum = std::min(out.Minimum, n);
				out.Maximum = std::max(out.Maximum, n);
				if (!std::isfinite(n)) out.Maximum = std::numeric_limits<double>::infinity();
			};
			if (const auto *n = std::get_if<double>(&leaf))
				add(*n);
			else if (const auto *n = std::get_if<int64_t>(&leaf))
				add(double(*n));
			else if (const auto *n = std::get_if<EnumValue>(&leaf))
				add(double(n->Value));
			else if (const auto *n = std::get_if<bool>(&leaf))
				add(*n ? 1 : 0);
		}
		NumericBounds AllNumbers(const NodeContext &context, std::string_view port, double fallback) {
			NumericBounds out;
			if (const Value *value = OriginalInput(context, port)) {
				if (const auto *array = std::get_if<ArrayValue>(value)) {
					for (const auto &leaf : array->Elements)
						BoundNumber(leaf, out);
					for (const auto &row : array->Nested)
						for (const auto &leaf : row)
							BoundNumber(leaf, out);
					const auto visit =
						[&](auto &&self, const std::vector<SourceArrayItem> &items, size_t depth) -> void {
						if (depth > Limits::MaximumArrayDepth) return;
						for (const auto &item : items) {
							if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
								BoundNumber(*leaf, out);
							else if (const auto *nested =
										 std::get_if<std::vector<SourceArrayItem>>(&item.Data))
								self(self, *nested, depth + 1);
						}
					};
					visit(visit, array->Items, 0);
				} else
					BoundNumber(*value, out);
			}
			if (!out.Found) {
				out.Minimum = out.Maximum = fallback;
			}
			return out;
		}
		uint64_t LargestImagePixels(const NodeContext &context, std::string_view port) {
			uint64_t pixels = 0;
			if (const Image *image = context.Input(port)) pixels = uint64_t(image->Width) * image->Height;
			for (const auto &[id, images] : context.ImageArrays)
				if (id == port && images)
					for (const auto &image : images->Images)
						pixels = std::max(pixels, uint64_t(image.Width) * image.Height);
			return pixels;
		}
		bool AdmitBatch(NodeContext &context) {
			if (context.ProcessorRow != 0) return true;
			const auto active = AllNumbers(context, "active", 1);
			if (active.Minimum == 0 && active.Maximum == 0) return true;
			const auto grey = AllNumbers(context, "greyscale", 0);
			const auto types = AllNumbers(context, "type", 0);
			const double minimumType = std::floor(types.Minimum), maximumType = std::ceil(types.Maximum);
			uint64_t taps = 1;
			if (minimumType <= 0 && maximumType >= 0) taps = 4;
			if (minimumType <= 1 && maximumType >= 1) taps = 12;
			if (minimumType <= 2 && maximumType >= 2) taps = std::max(taps, uint64_t{6});
			const uint64_t channels = grey.Minimum != 0 && grey.Maximum != 0 ? 1 : 4;
			const uint64_t perPixel = 12 + channels * (2 + taps);
			const uint64_t rows = std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
			const uint64_t pixels = LargestImagePixels(context, "surface_in");
			if (rows > 64000000 / perPixel || pixels > 64000000 / (rows * perPixel))
				return context.Fail(
					Status::LimitExceeded,
					"Error Diffuse Dither complete processor batch exceeds work budget",
					"surface_in"
				);
			const uint64_t work = pixels * perPixel;
			const uint64_t maskPixels = LargestImagePixels(context, "mask");
			const auto feather = AllNumbers(context, "mask_feather", 0);
			if (maskPixels && feather.Maximum > 0) {
				if (!std::isfinite(feather.Maximum) || feather.Maximum > 64)
					return context.Fail(
						Status::LimitExceeded,
						"Error Diffuse Dither mask feather exceeds work budget",
						"mask_feather"
					);
				const uint64_t cost = 16 + uint64_t(std::ceil(feather.Maximum)) * 16;
				if (maskPixels > (64000000 / rows - work) / cost)
					return context.Fail(
						Status::LimitExceeded,
						"Error Diffuse Dither mask exceeds complete batch work budget",
						"mask_feather"
					);
			}
			return true;
		}

		int16_t BufferSigned16(double value) {
			const int64_t truncated = static_cast<int64_t>(std::trunc(value));
			const int64_t low = ((truncated % 65536) + 65536) % 65536;
			return static_cast<int16_t>(low < 32768 ? low : low - 65536);
		}
	}
	bool SourceDitherDiffuse(NodeContext &context) {
		if (!AdmitBatch(context)) return false;
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		const Image *source = context.Input("surface_in");
		if (!source)
			return context.Fail(
				Status::InvalidValue, "Error Diffuse Dither requires its surface", "surface_in"
			);
		// The source reads four raw bytes per pixel from buffer_from_surface, without
		// format conversion. Other native readback layouts need a captured contract.
		if (source->Format != SurfaceFormat::RGBA8Unorm)
			return context.Fail(
				Status::UnsupportedExecution,
				"Error Diffuse Dither raw readback profile requires RGBA8",
				"surface_in"
			);
		const int64_t mode = context.Integer("type");
		if (mode < 0 || mode > 3)
			return context.Fail(Status::InvalidValue, "Error Diffuse Dither type is invalid", "type");
		const std::array<std::span<const DiffusionTap>, 4> taps{FLOYD, JARVIS, ATKINSON, LINEAR};
		const auto kernel = taps[size_t(mode)];
		const bool grey = context.Boolean("greyscale");
		const uint64_t channels = grey ? 1 : 4, pixels = uint64_t(source->Width) * source->Height;
		const uint64_t rows = std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
		const uint64_t perPixel = 12 + channels * (2 + kernel.size());
		if (rows > 64000000 / perPixel || pixels > 64000000 / (rows * perPixel))
			return context.Fail(
				Status::LimitExceeded,
				"Error Diffuse Dither complete processor batch exceeds work budget",
				"surface_in"
			);
		uint64_t work = pixels * perPixel;
		const Image *mask = context.Input("mask");
		const double feather = context.Scalar("mask_feather");
		if (mask && feather > 0) {
			if (!std::isfinite(feather) || feather > 64)
				return context.Fail(
					Status::LimitExceeded,
					"Error Diffuse Dither mask feather exceeds work budget",
					"mask_feather"
				);
			const uint64_t maskPixels = uint64_t(mask->Width) * mask->Height;
			const uint64_t maskCost = 16 + uint64_t(std::ceil(feather)) * 16;
			if (maskPixels > (64000000 / rows - work) / maskCost)
				return context.Fail(
					Status::LimitExceeded,
					"Error Diffuse Dither mask exceeds complete batch work budget",
					"mask_feather"
				);
			work += maskPixels * maskCost;
		}
		if (work > 64000000 / rows)
			return context.Fail(
				Status::LimitExceeded,
				"Error Diffuse Dither complete processor batch exceeds work budget",
				"surface_in"
			);
		auto scratch = context.ReserveWorkspace(pixels * channels * sizeof(int16_t), "surface_in");
		if (!scratch) return false;
		std::vector<int16_t> buffer(pixels * channels);
		const uint64_t retained = buffer.capacity() * sizeof(int16_t);
		if (retained > scratch->Bytes() && retained - scratch->Bytes() > context.AvailableBytes())
			return context.Fail(
				Status::LimitExceeded,
				"Error Diffuse Dither scratch capacity exceeds byte budget",
				"surface_in"
			);
		if (!scratch->Resize(retained))
			return context.Fail(
				Status::LimitExceeded,
				"Error Diffuse Dither scratch capacity exceeds byte budget",
				"surface_in"
			);
		for (size_t p = 0; p < pixels; ++p)
			for (size_t c = 0; c < channels; ++c)
				buffer[p * channels + c] = source->Pixels[p * 4 + c];
		Image *output =
			context.NewImage("surface_out", source->Width, source->Height, SurfaceFormat::RGBA8Unorm);
		if (!output) return false;
		for (uint32_t y = 0; y < source->Height; ++y)
			for (uint32_t x = 0; x < source->Width; ++x) {
				const size_t pixel = size_t(y) * source->Width + x;
				std::array<int, 4> error{};
				for (size_t c = 0; c < channels; ++c) {
					const int original = buffer[pixel * channels + c];
					const uint8_t quantized = original > 128 ? 255 : 0;
					output->Pixels[pixel * 4 + c] = quantized;
					error[c] = original - quantized;
				}
				if (grey) {
					output->Pixels[pixel * 4 + 1] = output->Pixels[pixel * 4];
					output->Pixels[pixel * 4 + 2] = output->Pixels[pixel * 4];
					output->Pixels[pixel * 4 + 3] = 255;
				}
				for (const auto &tap : kernel) {
					const int64_t nx = int64_t(x) + tap.X, ny = int64_t(y) + tap.Y;
					if (nx < 0 || ny < 0 || nx >= source->Width || ny >= source->Height) continue;
					const size_t at = (size_t(ny) * source->Width + size_t(nx)) * channels;
					for (size_t c = 0; c < channels; ++c)
						buffer[at + c] = BufferSigned16(buffer[at + c] + error[c] * tap.Weight);
				}
			}
		FinishProcessor(context, *source, *output);
		return context.FailureCode == Status::Ok;
	}
} // namespace engine::imagegraph::detail
