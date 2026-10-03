#include "../ValuePayload.hpp"
#include "Families.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t SurfaceBufferHeader = 24, SurfaceBufferWork = 64 * 1024 * 1024;
		struct SurfaceBufferFormat {
			SurfaceFormat Native;
			uint8_t Source;
		};
		// Raw GameMaker format IDs, not Pixel Composer precision selector values.
		constexpr std::array<SurfaceBufferFormat, 7> SurfaceBufferFormats{
			{SurfaceBufferFormat{SurfaceFormat::RGBA8Unorm, 6},
			 {SurfaceFormat::R16Float, 9},
			 {SurfaceFormat::R32Float, 10},
			 {SurfaceFormat::RGBA4Unorm, 11},
			 {SurfaceFormat::R8Unorm, 12},
			 {SurfaceFormat::RGBA16Float, 14},
			 {SurfaceFormat::RGBA32Float, 15}}
		};
		struct SurfaceBufferLayout {
			uint32_t Width = 0, Height = 0;
			SurfaceFormat Format = SurfaceFormat::RGBA8Unorm;
			uint64_t Pixels = 0;
		};
		const SurfaceBufferFormat *SurfaceBufferFormatFor(SurfaceFormat format) {
			for (const auto &entry : SurfaceBufferFormats)
				if (entry.Native == format) return &entry;
			return nullptr;
		}
		const Image *SurfaceBufferInput(const NodeContext &c) {
			if (const auto *image = c.Input("surface")) return image;
			if (const auto *value = c.Find("surface"))
				if (const auto *surface = std::get_if<SurfaceValue>(value)) return &surface->Data;
			return nullptr;
		}
		bool SurfaceBufferRead(NodeContext &c, const BufferValue &buffer, SurfaceBufferLayout &layout) {
			const auto &bytes = buffer.Bytes;
			if (bytes.size() > Limits::MaximumArrayBytes)
				return c.Fail(Status::LimitExceeded, "PXCS buffer exceeds native byte bound", "input_0");
			if (bytes.size() < SurfaceBufferHeader || !std::equal(bytes.begin(), bytes.begin() + 4, "PXCS"))
				return c.Fail(
					Status::UnsupportedExecution,
					"source PXCS buffer has no represented surface result",
					"input_0"
				);
			layout.Width = uint32_t(bytes[4]) | (uint32_t(bytes[5]) << 8);
			layout.Height = uint32_t(bytes[6]) | (uint32_t(bytes[7]) << 8);
			if (!layout.Width || !layout.Height)
				return c.Fail(
					Status::UnsupportedExecution,
					"source PXCS zero dimension has no surface result",
					"input_0"
				);
			const SurfaceBufferFormat *format = nullptr;
			for (const auto &entry : SurfaceBufferFormats)
				if (entry.Source == bytes[8]) format = &entry;
			if (!format)
				return c.Fail(
					Status::UnsupportedExecution,
					"PXCS source surface format has no native representation",
					"input_0"
				);
			layout.Format = format->Native;
			if (layout.Width > c.Request.MaximumImageDimension ||
				layout.Height > c.Request.MaximumImageDimension)
				return c.Fail(Status::LimitExceeded, "PXCS dimensions exceed request bounds", "input_0");
			const auto checked = CheckedSurfaceLayout(
				layout.Width, layout.Height, layout.Format, Limits::MaximumArrayBytes - SurfaceBufferHeader
			);
			if (!checked)
				return c.Fail(Status::LimitExceeded, "PXCS surface layout exceeds byte bounds", "input_0");
			layout.Pixels = checked->Bytes;
			if (layout.Pixels > bytes.size() - SurfaceBufferHeader)
				return c.Fail(
					Status::UnsupportedExecution,
					"truncated PXCS transfer leaves unobserved source surface pixels",
					"input_0"
				);
			return true;
		}
		bool
		SurfaceBufferFinite(NodeContext &c, const BufferValue &buffer, const SurfaceBufferLayout &layout) {
			const auto info = *DescribeSurfaceFormat(layout.Format);
			if (!info.FloatingPoint) return true;
			const uint8_t step = info.BitsPerChannel / 8;
			for (size_t i = SurfaceBufferHeader; i < SurfaceBufferHeader + layout.Pixels; i += step) {
				uint32_t word = 0;
				for (uint8_t k = 0; k < step; ++k)
					word |= uint32_t(buffer.Bytes[i + k]) << (8 * k);
				if (step == 2 ? (word & 0x7c00) == 0x7c00 : (word & 0x7f800000) == 0x7f800000)
					return c.Fail(
						Status::UnsupportedExecution,
						"PXCS nonfinite samples have no native finite surface representation",
						"input_0"
					);
			}
			return true;
		}
		bool SurfaceBufferImage(NodeContext &c, const Image &image, uint64_t &largest, uint64_t &scan) {
			if (!ValidSurfaceLayout(
					image, c.Request.MaximumImageDimension, Limits::MaximumArrayBytes - SurfaceBufferHeader
				) ||
				image.Width > 65535 || image.Height > 65535)
				return c.Fail(
					Status::LimitExceeded, "surface cannot fit bounded PXCS header and payload", "surface"
				);
			largest = std::max(largest, uint64_t(image.Pixels.size()) + SurfaceBufferHeader);
			if (image.Pixels.size() > SurfaceBufferWork - scan)
				return c.Fail(
					Status::LimitExceeded, "PXCS original surfaces exceed batch scan bound", "surface"
				);
			scan += image.Pixels.size();
			return true;
		}
		bool SurfaceBufferAdmit(NodeContext &c, uint64_t largest, uint64_t scan, std::string_view port) {
			const uint64_t rows = std::max(size_t{1}, c.ProcessorCount);
			if (scan > SurfaceBufferWork || largest > (SurfaceBufferWork - scan) / 3 ||
				rows > (SurfaceBufferWork - scan) / (3 * std::max(uint64_t{1}, largest)))
				return c.Fail(
					Status::LimitExceeded, "PXCS complete processor batch exceeds copy work budget", port
				);
			return true;
		}
		bool SurfaceBufferArray(NodeContext &c, const ArrayValue &array, uint64_t &largest, uint64_t &scan) {
			size_t count = 0;
			const auto leaf = [&](const ElementValue &value) {
				if (++count > Limits::MaximumArrayElements)
					return c.Fail(
						Status::LimitExceeded, "PXCS buffer batch exceeds element bound", "input_0"
					);
				const auto *buffer = std::get_if<BufferValue>(&value);
				if (!buffer)
					return c.Fail(
						Status::UnsupportedExecution,
						"PXCS buffer batch contains an unrepresented source operand",
						"input_0"
					);
				SurfaceBufferLayout layout;
				if (!SurfaceBufferRead(c, *buffer, layout)) return false;
				largest = std::max(largest, uint64_t(buffer->Bytes.size()));
				if (buffer->Bytes.size() > SurfaceBufferWork - scan)
					return c.Fail(
						Status::LimitExceeded, "PXCS original buffers exceed batch scan bound", "input_0"
					);
				scan += buffer->Bytes.size();
				return true;
			};
			const auto tree =
				[&](const auto &self, const std::vector<SourceArrayItem> &items, size_t depth) -> bool {
				if (depth > Limits::MaximumArrayDepth || items.size() > Limits::MaximumArrayElements - count)
					return c.Fail(
						Status::LimitExceeded, "PXCS buffer batch tree exceeds shape bound", "input_0"
					);
				for (const auto &item : items) {
					if (const auto *value = std::get_if<ElementValue>(&item.Data)) {
						if (!leaf(*value)) return false;
					} else if (const auto *children = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
						++count;
						if (!self(self, *children, depth + 1)) return false;
					} else
						return c.Fail(
							Status::UnsupportedExecution,
							"PXCS buffer batch contains a surface instead of buffer",
							"input_0"
						);
				}
				return true;
			};
			if (!array.Items.empty()) return tree(tree, array.Items, 1);
			if (!array.Nested.empty())
				for (const auto &row : array.Nested) {
					if (++count > Limits::MaximumArrayElements)
						return c.Fail(
							Status::LimitExceeded, "PXCS buffer row count exceeds bound", "input_0"
						);
					for (const auto &value : row)
						if (!leaf(value)) return false;
				}
			else
				for (const auto &value : array.Elements)
					if (!leaf(value)) return false;
			return true;
		}
		bool SurfaceBufferPreflight(NodeContext &c, bool encode) {
			if (c.ProcessorRow) return true;
			uint64_t largest = 0, scan = 0;
			if (encode) {
				bool found = false;
				for (const auto &[port, array] : c.ImageArrays)
					if (port == "surface" && array) {
						if (array->Images.size() > Limits::MaximumArrayElements)
							return c.Fail(
								Status::LimitExceeded, "PXCS surface batch exceeds image bound", "surface"
							);
						for (const auto &image : array->Images)
							if (!SurfaceBufferImage(c, image, largest, scan)) return false;
						found = true;
					}
				const auto inspect = [&](const Value *value) {
					if (!value) return true;
					if (const auto *surface = std::get_if<SurfaceValue>(value))
						return SurfaceBufferImage(c, surface->Data, largest, scan);
					if (const auto *atlas = std::get_if<AtlasValue>(value); atlas && atlas->Data)
						return SurfaceBufferImage(c, atlas->Data->Surface.Data, largest, scan);
					if (const auto *array = std::get_if<ArrayValue>(value)) {
						size_t count = 0;
						const auto tree = [&](const auto &self,
											  const std::vector<SourceArrayItem> &items,
											  size_t depth) -> bool {
							if (depth > Limits::MaximumArrayDepth ||
								items.size() > Limits::MaximumArrayElements - count)
								return c.Fail(
									Status::LimitExceeded,
									"PXCS source surface tree exceeds bounds",
									"surface"
								);
							count += items.size();
							for (const auto &item : items) {
								const Image *image = std::get_if<Image>(&item.Data);
								if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
									if (const auto *surface = std::get_if<SurfaceValue>(leaf))
										image = &surface->Data;
								if (const auto *leaf = std::get_if<ElementValue>(&item.Data))
									if (const auto *atlas = std::get_if<AtlasValue>(leaf);
										atlas && atlas->Data)
										image = &atlas->Data->Surface.Data;
								if (image) {
									if (!SurfaceBufferImage(c, *image, largest, scan)) return false;
								} else if (const auto *children =
											   std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
									if (!self(self, *children, depth + 1)) return false;
								} else
									return c.Fail(
										Status::UnsupportedExecution,
										"PXCS surface batch contains an unrepresented source operand",
										"surface"
									);
							}
							return true;
						};
						if (!array->Items.empty()) return tree(tree, array->Items, 1);
						const auto leaf = [&](const ElementValue &item) {
							if (++count > Limits::MaximumArrayElements)
								return c.Fail(
									Status::LimitExceeded,
									"PXCS source surface rows exceed element bound",
									"surface"
								);
							const Image *image = nullptr;
							if (const auto *surface = std::get_if<SurfaceValue>(&item))
								image = &surface->Data;
							if (const auto *atlas = std::get_if<AtlasValue>(&item); atlas && atlas->Data)
								image = &atlas->Data->Surface.Data;
							if (!image)
								return c.Fail(
									Status::UnsupportedExecution,
									"PXCS source array requires surface leaves",
									"surface"
								);
							return SurfaceBufferImage(c, *image, largest, scan);
						};
						if (!array->Nested.empty())
							for (const auto &row : array->Nested) {
								if (++count > Limits::MaximumArrayElements)
									return c.Fail(
										Status::LimitExceeded,
										"PXCS source rows exceed element bound",
										"surface"
									);
								for (const auto &item : row)
									if (!leaf(item)) return false;
							}
						else
							for (const auto &item : array->Elements)
								if (!leaf(item)) return false;
					}
					return true;
				};
				for (const auto &[port, value] : c.ProcessorOriginalValues)
					if (port == "surface") {
						if (!inspect(value)) return false;
						found = true;
					}
				if (!found) {
					if (const auto *image = SurfaceBufferInput(c)) {
						if (!SurfaceBufferImage(c, *image, largest, scan)) return false;
					} else if (!inspect(c.Find("surface")))
						return false;
				}
			} else {
				const Value *original = nullptr;
				for (const auto &[port, value] : c.ProcessorOriginalValues)
					if (port == "input_0") original = value;
				if (!original) original = c.Find("input_0");
				if (const auto *array = original ? std::get_if<ArrayValue>(original) : nullptr) {
					if (!SurfaceBufferArray(c, *array, largest, scan)) return false;
				} else if (const auto *buffer = original ? std::get_if<BufferValue>(original) : nullptr) {
					SurfaceBufferLayout layout;
					if (!SurfaceBufferRead(c, *buffer, layout)) return false;
					largest = scan = buffer->Bytes.size();
				}
			}
			return SurfaceBufferAdmit(c, largest, scan, encode ? "surface" : "input_0");
		}
		bool SurfaceToBuffer(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.surface_to_buffer");
			if (!SurfaceBufferPreflight(c, true)) return false;
			const auto *image = SurfaceBufferInput(c);
			if (!image)
				return c.Fail(
					Status::UnsupportedExecution,
					"source missing surface has no represented PXCS buffer",
					"surface"
				);
			uint64_t largest = 0, scan = 0;
			if (!SurfaceBufferImage(c, *image, largest, scan)) return false;
			if (!FiniteSurfaceSamples(*image))
				return c.Fail(
					Status::UnsupportedExecution,
					"PXCS nonfinite source samples have no native finite surface profile",
					"surface"
				);
			const auto *format = SurfaceBufferFormatFor(image->Format);
			if (!format)
				return c.Fail(
					Status::UnsupportedExecution,
					"surface format has no PXCS native representation",
					"surface"
				);
			if (!c.ReserveOutput(largest, "buffer")) return false;
			BufferValue result;
			result.Bytes.resize(size_t(largest), 0);
			std::copy_n("PXCS", 4, result.Bytes.begin());
			result.Bytes[4] = uint8_t(image->Width);
			result.Bytes[5] = uint8_t(image->Width >> 8);
			result.Bytes[6] = uint8_t(image->Height);
			result.Bytes[7] = uint8_t(image->Height >> 8);
			result.Bytes[8] = format->Source;
			std::copy(image->Pixels.begin(), image->Pixels.end(), result.Bytes.begin() + SurfaceBufferHeader);
			c.SetValue("buffer", std::move(result));
			return c.FailureCode == Status::Ok;
		}
		bool SurfaceFromBuffer(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.surface_from_buffer");
			if (!SurfaceBufferPreflight(c, false)) return false;
			const auto *value = c.Find("input_0");
			const auto *buffer = value ? std::get_if<BufferValue>(value) : nullptr;
			if (!buffer)
				return c.Fail(
					Status::UnsupportedExecution,
					"source missing or whole array buffer has no represented surface",
					"input_0"
				);
			SurfaceBufferLayout layout;
			if (!SurfaceBufferRead(c, *buffer, layout) || !SurfaceBufferFinite(c, *buffer, layout))
				return false;
			auto *image = c.NewImage("surface", layout.Width, layout.Height, layout.Format);
			if (!image) return false;
			std::copy_n(
				buffer->Bytes.begin() + SurfaceBufferHeader, size_t(layout.Pixels), image->Pixels.begin()
			);
			image->Hash = SurfaceHash(*image);
			return c.FailureCode == Status::Ok;
		}
	} // namespace
	std::span<const ExecutorEntry> SourceSurfaceBufferExecutors() {
		static constexpr ExecutorEntry entries[] = {
			{"pc.surface_to_buffer", SurfaceToBuffer, true},
			{"pc.surface_from_buffer", SurfaceFromBuffer, true}
		};
		return entries;
	}
} // namespace engine::imagegraph::detail
