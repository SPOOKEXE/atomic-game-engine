#include "Families.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"
#include "SourceInterpret.hpp"

#include <bit>
#include <limits>
namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t SOURCE_DATA_WORK_LIMIT = 1u << 24;
		uint32_t SurfaceDataWord(const Image &image, size_t offset, size_t bytes = 4) {
			uint32_t word = 0;
			for (size_t i = 0; i < bytes; ++i)
				word |= uint32_t(image.Pixels[offset + i]) << (8 * i);
			return word;
		}
		const Image *SurfaceDataInput(NodeContext &c, std::string_view port) {
			const auto *source = c.Input(port);
			if (!source)
				c.Fail(Status::UnsupportedExecution, "Surface data requires a resolved source surface", port);
			return source;
		}
		bool SurfaceDataReserve(
			NodeContext &c, ArrayValue &result, size_t count, ValueType type, std::string_view port
		) {
			if (count > Limits::MaximumArrayElements)
				return c.Fail(Status::LimitExceeded, "Surface data exceeds bounded array elements", port);
			if (!c.ReserveOutput(
					count * sizeof(ElementValue) + std::max(port.size(), std::string{}.capacity()), port
				))
				return false;
			result.ElementType = type;
			result.Elements.reserve(count);
			return true;
		}
		bool SurfaceDataWords(NodeContext &c, const Image &image, size_t &count, std::string_view port) {
			count = size_t(image.Width) * image.Height;
			// The GML reads one u32 per texel, even for formats whose texels contain multiple words.
			if (count > image.Pixels.size() / 4)
				return c.Fail(
					Status::UnsupportedExecution, "Source u32 readback exceeds available surface bytes", port
				);
			// Extraction and Find All each make at most two complete word scans.
			if (count > SOURCE_DATA_WORK_LIMIT / std::max<size_t>(1, c.ProcessorCount) / 2)
				return c.Fail(
					Status::LimitExceeded, "Surface data processor batch exceeds traversal budget", port
				);
			return true;
		}
		bool SourcePixelExtract(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.pixel_extract");
			const auto *source = SurfaceDataInput(c, "surface_in");
			if (!source) return false;
			size_t count = 0;
			if (!SurfaceDataWords(c, *source, count, "surface_in")) return false;
			const int64_t skip = c.Integer("skip");
			size_t selected = 0;
			for (size_t i = 0; i < count; ++i) {
				const auto word = SurfaceDataWord(*source, i * 4);
				if ((skip == 1 && (word & 0xffffff) == 0) || (skip == 2 && word == 0)) continue;
				++selected;
			}
			ArrayValue result;
			if (!SurfaceDataReserve(c, result, selected, ValueType::Integer, "colors")) return false;
			for (size_t i = 0; i < count; ++i) {
				const auto word = SurfaceDataWord(*source, i * 4);
				if ((skip == 1 && (word & 0xffffff) == 0) || (skip == 2 && word == 0)) continue;
				result.Elements.emplace_back(int64_t(word));
			}
			c.SetValue("colors", std::move(result));
			return c.FailureCode == Status::Ok;
		}
		bool SourceFindPixel(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.find_pixel");
			const auto *source = SurfaceDataInput(c, "surface_in");
			if (!source) return false;
			size_t count = 0;
			if (!SurfaceDataWords(c, *source, count, "surface_in")) return false;
			const auto *raw = c.Find("search_color");
			const auto colour = raw ? std::visit([](const auto &v) { return InterpretPackedColour(v); }, *raw)
									: std::optional<Colour>{Colour{0, 0, 0, 255}};
			if (!colour)
				return c.Fail(
					Status::UnsupportedExecution,
					"Search colour needs a resolved packed representation",
					"search_color"
				);
			const double tolerance = c.Scalar("tolerance"), alphaTolerance = c.Scalar("alpha_tolerance", .2);
			if (!std::isfinite(tolerance) || !std::isfinite(alphaTolerance))
				return c.Fail(Status::InvalidValue, "Pixel tolerances must be finite", "tolerance");
			const bool includeAlpha = c.Boolean("include_alpha"), all = c.Boolean("find_all");
			const auto match = [&](size_t i) {
				const auto word = SurfaceDataWord(*source, i * 4);
				const double r = (word & 255) / 255., g = ((word >> 8) & 255) / 255.,
							 b = ((word >> 16) & 255) / 255., a = (word >> 24) / 255.;
				if (!includeAlpha && a == 0) return false;
				const double difference =
					(std::abs(colour->Red / 255. - r) + std::abs(colour->Green / 255. - g) +
					 std::abs(colour->Blue / 255. - b)) /
					3;
				return difference <= tolerance &&
					   (!includeAlpha || std::abs(colour->Alpha / 255. - a) <= alphaTolerance);
			};
			if (!all) {
				if (!c.ReserveOutput(std::string{}.capacity(), "position")) return false;
				Vector2 position{-1, -1};
				for (size_t i = 0; i < count; ++i)
					if (match(i)) {
						position = {double(i % source->Width), double(i / source->Width)};
						break;
					}
				c.SetValue("position", position);
			} else {
				size_t selected = 0;
				for (size_t i = 0; i < count; ++i)
					if (match(i)) ++selected;
				ArrayValue result;
				if (!SurfaceDataReserve(c, result, selected, ValueType::Vector2, "position")) return false;
				for (size_t i = 0; i < count; ++i)
					if (match(i))
						result.Elements.emplace_back(
							Vector2{double(i % source->Width), double(i / source->Width)}
						);
				c.SetValue("position", std::move(result));
			}
			return c.FailureCode == Status::Ok;
		}
		bool SourceSurfaceToPoints(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.surface_to_points");
			const auto *source = SurfaceDataInput(c, "surface");
			if (!source) return false;
			const auto format = DescribeSurfaceFormat(source->Format);
			if (!format || source->Format == SurfaceFormat::RGBA4Unorm ||
				source->Format == SurfaceFormat::R8Unorm)
				return c.Fail(
					Status::UnsupportedExecution,
					"Source point readback has no branch for this surface format",
					"surface"
				);
			size_t count = size_t(source->Width) * source->Height;
			if (format->Channels == 1) {
				if (count % 2)
					return c.Fail(
						Status::UnsupportedExecution,
						"Single-channel source point count is fractional for an odd texel count",
						"surface"
					);
				count /= 2;
			}
			const int64_t requested = c.Integer("max_amount");
			if (requested < 0)
				return c.Fail(
					Status::UnsupportedExecution,
					"Source negative point array length is unresolved",
					"max_amount"
				);
			if (requested) count = std::min(count, size_t(requested));
			const auto minimum = UnitVector(c, "range_min", c.Project.SurfaceWidth, c.Project.SurfaceHeight),
					   maximum = UnitVector(c, "range_max", c.Project.SurfaceWidth, c.Project.SurfaceHeight);
			if (!std::isfinite(minimum.X) || !std::isfinite(minimum.Y) || !std::isfinite(maximum.X) ||
				!std::isfinite(maximum.Y))
				return c.Fail(Status::InvalidValue, "Point ranges must be finite", "range_min");
			ArrayValue result;
			if (!SurfaceDataReserve(c, result, count, ValueType::Vector2, "points")) return false;
			const size_t bytes = format->BitsPerChannel / 8, stride = (format->Channels == 1 ? 2 : 4) * bytes;
			const auto component = [&](size_t offset) {
				if (!format->FloatingPoint) return source->Pixels[offset] / 255.;
				const auto word = SurfaceDataWord(*source, offset, bytes);
				return bytes == 2 ? double(DecodeHalf(uint16_t(word))) : double(std::bit_cast<float>(word));
			};
			for (size_t i = 0; i < count; ++i) {
				const double x = component(i * stride), y = component(i * stride + bytes);
				const Vector2 point{
					minimum.X + (maximum.X - minimum.X) * x, minimum.Y + (maximum.Y - minimum.Y) * y
				};
				if (!std::isfinite(point.X) || !std::isfinite(point.Y))
					return c.Fail(
						Status::InvalidValue, "Point readback produces a non-finite coordinate", "surface"
					);
				result.Elements.emplace_back(point);
			}
			c.SetValue("points", std::move(result));
			return c.FailureCode == Status::Ok;
		}
		uint8_t SurfaceDataByte(double value) {
			value = std::clamp(value, 0., 255.);
			const double floor = std::floor(value), fraction = value - floor;
			return uint8_t(floor + (fraction > .5 || (fraction == .5 && std::fmod(floor, 2) != 0)));
		}
		bool SurfaceDataPacked(NodeContext &c, const Image &image, double x, double y, uint32_t &packed) {
			// The inspected HTML5 builtin converts coordinates with yyGetInt32 before reading a texel.
			if (!std::isfinite(x) || !std::isfinite(y) || x < std::numeric_limits<int32_t>::min() ||
				x > std::numeric_limits<int32_t>::max() || y < std::numeric_limits<int32_t>::min() ||
				y > std::numeric_limits<int32_t>::max())
				return c.Fail(
					Status::InvalidValue,
					"Sampler coordinates exceed finite source integer bounds",
					"position"
				);
			const auto px = int32_t(x), py = int32_t(y);
			if (px < 0 || py < 0 || uint32_t(px) >= image.Width || uint32_t(py) >= image.Height)
				return c.Fail(
					Status::UnsupportedExecution,
					"Sampler source builtin out-of-bounds readback has no recorded desktop result",
					"oversample"
				);
			if (image.Format == SurfaceFormat::RGBA8Unorm) {
				packed = SurfaceDataWord(image, (size_t(py) * image.Width + uint32_t(px)) * 4);
				return true;
			}
			if (!DescribeSurfaceFormat(image.Format)->FloatingPoint)
				return c.Fail(
					Status::UnsupportedExecution,
					"Sampler packed readback for this unorm format is unresolved",
					"surface_in"
				);
			SurfacePixel pixel{};
			if (!LoadSurfacePixel(image, uint32_t(px), uint32_t(py), pixel))
				return c.Fail(Status::InvalidValue, "Sampler surface layout is invalid", "surface_in");
			// The wrapper rounds weighted channel words separately, not each channel byte independently.
			double sum = 0, weight = 255;
			for (double channel : pixel) {
				const double value = channel * weight;
				if (!std::isfinite(value) || std::abs(value) >= 0x1p61)
					return c.Fail(
						Status::UnsupportedExecution,
						"Sampler float packed conversion exceeds source numeric bounds",
						"surface_in"
					);
				const double floor = std::floor(value), fraction = value - floor;
				sum += floor + (fraction > .5 || (fraction == .5 && std::fmod(floor, 2) != 0));
				weight *= 256;
			}
			if (!std::isfinite(sum) || sum < -0x1p63 || sum >= 0x1p63)
				return c.Fail(
					Status::UnsupportedExecution,
					"Sampler packed sum exceeds source int64 bounds",
					"surface_in"
				);
			packed = uint32_t(int64_t(sum));
			return true;
		}
		bool SourceDataSampler(NodeContext &c) {
			ENGINE_PROFILE("imagegraph.source.sampler");
			const auto *source = SurfaceDataInput(c, "surface_in");
			if (!source) return false;
			const auto position = UnitVector(c, "position", source->Width, source->Height);
			const int64_t sampling = c.Integer("sampling_size", 1);
			if (sampling < 1) {
				if (!c.ReserveOutput(std::string{}.capacity(), "color")) return false;
				c.SetValue("color", Colour{0, 0, 0, c.Boolean("alpha") ? uint8_t(0) : uint8_t(255)});
				return c.FailureCode == Status::Ok;
			}
			if (sampling > 2048)
				return c.Fail(
					Status::LimitExceeded, "Sampler square size exceeds bounded work", "sampling_size"
				);
			const int64_t radius = sampling - 1, side = 2 * radius + 1;
			if (uint64_t(side) >
				SOURCE_DATA_WORK_LIMIT / std::max<size_t>(1, c.ProcessorCount) / uint64_t(side))
				return c.Fail(
					Status::LimitExceeded,
					"Sampler processor batch exceeds sample work budget",
					"sampling_size"
				);
			const int64_t oversample = ReadSampler(c).Oversample;
			double red = 0, green = 0, blue = 0, alpha = 0, count = 0;
			for (int64_t i = -radius; i <= radius; ++i)
				for (int64_t j = -radius; j <= radius; ++j) {
					double x = position.X + i, y = position.Y + j;
					if (x < 0 || y < 0 || x >= source->Width || y >= source->Height) {
						// Preserve the source's raw switch labels, including their mismatch with editor
						// labels.
						switch (oversample) {
						case 0:
							continue;
						case 1:
							x = std::clamp(x, 0., double(source->Width - 1));
							y = std::clamp(y, 0., double(source->Height - 1));
							break;
						case 2:
							x = source->Width == 1 ? 0 : std::fmod(x, source->Width - 1);
							y = source->Height == 1 ? 0 : std::fmod(y, source->Height - 1);
							break;
						case 3:
							alpha += 255;
							++count;
							continue;
						default:
							break;
						}
					}
					uint32_t packed = 0;
					if (!SurfaceDataPacked(c, *source, x, y, packed)) return false;
					red += packed & 255;
					green += (packed >> 8) & 255;
					blue += (packed >> 16) & 255;
					alpha += packed >> 24;
					++count;
				}
			if (count) {
				red /= count;
				green /= count;
				blue /= count;
				alpha /= count;
			}
			if (!c.ReserveOutput(std::string{}.capacity(), "color")) return false;
			c.SetValue(
				"color",
				Colour{
					SurfaceDataByte(red),
					SurfaceDataByte(green),
					SurfaceDataByte(blue),
					c.Boolean("alpha") ? SurfaceDataByte(alpha) : uint8_t(255)
				}
			);
			return c.FailureCode == Status::Ok;
		}

	}
	std::span<const ExecutorEntry> SourceSurfaceDataExecutors() {
		static constexpr ExecutorEntry entries[] = {
			{"pc.pixel_extract", SourcePixelExtract, true},
			{"pc.find_pixel", SourceFindPixel, true},
			{"pc.surface_to_points", SourceSurfaceToPoints, true},
			{"pc.sampler", SourceDataSampler, true}
		};
		return entries;
	}
}
