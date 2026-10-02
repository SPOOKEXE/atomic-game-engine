#include "GraphRasterHost.hpp"

#include <engine/bake/GifSequence.hpp>
#include <engine/bake/Image.hpp>
#include <engine/core/Metrics.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		const Value *Input(const HostNodeInvocation &in, std::string_view port) {
			for (const auto &v : in.Inputs)
				if (v.Port == port) return &v.Data;
			return nullptr;
		}
		// Source IPadding applies round-to-even after its unit conversion.
		std::optional<Vector4> RoundedPadding(const Vector4 &value) {
			Vector4 result;
			std::array<double *, 4> destination{&result.X, &result.Y, &result.Z, &result.W};
			const std::array source{value.X, value.Y, value.Z, value.W};
			for (size_t i = 0; i < source.size(); ++i) {
				const double n = source[i];
				if (!std::isfinite(n) || std::abs(n) > Limits::MaximumDimension) return std::nullopt;
				const double base = std::floor(n), fraction = n - base;
				if (fraction < .5)
					*destination[i] = base;
				else if (fraction > .5)
					*destination[i] = base + 1;
				else
					*destination[i] = std::fmod(base, 2) == 0 ? base : base + 1;
			}
			return result;
		}
		uint32_t Word(std::span<const std::byte> bytes, size_t at, size_t count, bool big) {
			uint32_t out = 0;
			for (size_t i = 0; i < count; ++i)
				out |= uint32_t(std::to_integer<uint8_t>(bytes[at + i])) << (8 * (big ? count - 1 - i : i));
			return out;
		}
		// Admit decoder workspace from the actual header before invoking the codec.
		bool Dimensions(
			std::span<std::byte> bytes, uint32_t &width, uint32_t &height, uint32_t &frames, bool &frameLimit
		) {
			frames = 1;
			frameLimit = false;
			using engine::bake::ImageFormat;
			switch (engine::bake::ImageFormatOfBytes(bytes)) {
			case ImageFormat::Png:
				if (bytes.size() < 24) return false;
				width = Word(bytes, 16, 4, true);
				height = Word(bytes, 20, 4, true);
				return true;
			case ImageFormat::Bmp: {
				if (bytes.size() < 26 || Word(bytes, 14, 4, false) < 40) return false;
				const int32_t w = static_cast<int32_t>(Word(bytes, 18, 4, false)),
							  h = static_cast<int32_t>(Word(bytes, 22, 4, false));
				if (w <= 0 || h == 0 || h == INT32_MIN) return false;
				width = uint32_t(w);
				height = uint32_t(h < 0 ? -h : h);
				return true;
			}
			case ImageFormat::Gif: {
				if (bytes.size() < 13 ||
					(std::memcmp(bytes.data(), "GIF87a", 6) && std::memcmp(bytes.data(), "GIF89a", 6)))
					return false;
				width = Word(bytes, 6, 2, false);
				height = Word(bytes, 8, 2, false);
				frames = 0;
				size_t at = 13;
				const auto skip = [&](size_t count) {
					if (count > bytes.size() - at) return false;
					at += count;
					return true;
				};
				const auto blocks = [&] {
					while (at < bytes.size()) {
						const size_t n = std::to_integer<uint8_t>(bytes[at++]);
						if (!n) return true;
						if (!skip(n)) return false;
					}
					return false;
				};
				const auto table = [&](uint8_t packed) {
					return !(packed & 128) || skip((size_t{1} << ((packed & 7) + 1)) * 3);
				};
				if (!table(std::to_integer<uint8_t>(bytes[10]))) return false;
				while (at < bytes.size()) {
					const auto marker = std::to_integer<uint8_t>(bytes[at++]);
					if (marker == 0x3b) return frames > 0 && at == bytes.size();
					if (marker == 0x21) {
						if (at == bytes.size()) return false;
						const auto label = std::to_integer<uint8_t>(bytes[at++]);
						if (label == 0xf9) {
							if (bytes.size() - at < 6 || bytes[at] != std::byte{4} ||
								bytes[at + 5] != std::byte{0})
								return false;
							// Still output ignores delays; avoid the sequence codec's float-time ceiling.
							bytes[at + 2] = std::byte{1};
							bytes[at + 3] = std::byte{0};
							at += 6;
						} else if (!blocks())
							return false;
						continue;
					}
					if (marker != 0x2c || bytes.size() - at < 9) return false;
					if (++frames > engine::assets::TextureSequence::MAXIMUM_FRAMES) {
						frameLimit = true;
						return false;
					}
					const auto left = Word(bytes, at, 2, false), top = Word(bytes, at + 2, 2, false),
							   w = Word(bytes, at + 4, 2, false), h = Word(bytes, at + 6, 2, false);
					const auto packed = std::to_integer<uint8_t>(bytes[at + 8]);
					at += 9;
					if (!w || !h || left + w > width || top + h > height || !table(packed) ||
						at == bytes.size())
						return false;
					const auto code = std::to_integer<uint8_t>(bytes[at++]);
					if (code < 2 || code > 8 || !blocks()) return false;
				}
				return false;
			}
			case ImageFormat::Jpeg: {
				size_t at = 2;
				while (at < bytes.size()) {
					if (std::to_integer<uint8_t>(bytes[at++]) != 255) return false;
					while (at < bytes.size() && bytes[at] == std::byte{255})
						++at;
					if (at == bytes.size()) return false;
					const auto marker = std::to_integer<uint8_t>(bytes[at++]);
					if (marker == 0xda || marker == 0xd9) return false;
					if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 1) continue;
					if (bytes.size() - at < 2) return false;
					const auto length = Word(bytes, at, 2, true);
					if (length < 2 || length > bytes.size() - at) return false;
					if (marker == 0xc0) {
						if (length < 8) return false;
						height = Word(bytes, at + 3, 2, true);
						width = Word(bytes, at + 5, 2, true);
						return true;
					}
					at += length;
				}
				return false;
			}
			default:
				return false;
			}
		}
	}
	static bool CaptureSequence(
		const engine::imagegraph::HostNodeInvocation &in,
		std::span<const GraphFileGrant> grants,
		const engine::assets::ContentPolicy &policy,
		engine::imagegraph::HostNodeCapture &out,
		std::string &failure
	) {
		using namespace engine::imagegraph;
		const auto fail = [&](const char *message) {
			failure = message;
			return false;
		};
		const auto *raw = Input(in, "paths"), *padRaw = Input(in, "padding");
		const auto *paths = raw ? std::get_if<ArrayValue>(raw) : nullptr;
		const auto *paddingValue = padRaw ? std::get_if<Vector4>(padRaw) : nullptr;
		const auto padding = paddingValue ? RoundedPadding(*paddingValue) : std::nullopt;
		const auto number = [&](std::string_view port) -> std::optional<int64_t> {
			const auto *value = Input(in, port);
			if (!value) return std::nullopt;
			if (const auto *v = std::get_if<EnumValue>(value)) return v->Value;
			if (const auto *v = std::get_if<int64_t>(value)) return *v;
			if (const auto *v = std::get_if<double>(value);
				v && std::isfinite(*v) && *v >= 0 && *v <= 2 && std::floor(*v) == *v)
				return int64_t(*v);
			return std::nullopt;
		};
		const auto canvas = number("canvas_size"), sizing = number("sizing_method");
		if (!paths || paths->ElementType != ValueType::Text || !paths->Nested.empty() ||
			!paths->Items.empty() || paths->Elements.size() > 256 || !padding || !canvas || *canvas < 0 ||
			*canvas > 2 || !sizing || *sizing < 0 || *sizing > 1)
			return fail(
				"image array requires bounded ordered text paths and resolved source canvas controls"
			);
		const auto authoredBytes = NodeClonePayloadBytes(in.Authored);
		const auto pathBytes = ValueClonePayloadBytes(*raw);
		if (!authoredBytes || !pathBytes || *authoredBytes > in.MaximumOperationBytes / 8 ||
			*pathBytes > in.MaximumOperationBytes / 8 - *authoredBytes)
			return fail("image array capture and path output exceed byte budget");
		uint64_t captureBytes = *authoredBytes + *pathBytes;
		for (const auto &input : in.Inputs) {
			const auto valueBytes = ValueClonePayloadBytes(input.Data);
			const uint64_t overhead = sizeof(AuthoredValue) + input.Port.size() + 1;
			if (!valueBytes || overhead > in.MaximumOperationBytes / 8 - captureBytes ||
				*valueBytes > in.MaximumOperationBytes / 8 - captureBytes - overhead)
				return fail("image array resolved capture exceeds byte budget");
			captureBytes += overhead + *valueBytes;
		}
		std::vector<Image> images;
		images.reserve(paths->Elements.size());
		uint32_t commonWidth = 0, commonHeight = 0;
		for (const auto &element : paths->Elements) {
			const auto *path = std::get_if<std::string>(&element);
			if (!path) return fail("image array path has incorrect leaf type");
			const GraphFileGrant *selected = nullptr;
			for (const auto &grant : grants)
				if (grant.NodeId == in.Authored.Id && grant.Resource == *path) {
					if (selected) return fail("image array resource grant is duplicated");
					selected = &grant;
				}
			if (!selected || selected->Write || selected->File.string() != *path)
				return fail("image array path requires an exact named read resource grant");
			Node authored;
			authored.Id = in.Authored.Id;
			authored.Type = "pc.image";
			std::array<AuthoredValue, 2> controls{{{"path", *path}, {"padding", Vector4{}}}};
			GraphFileGrant exact{authored.Id, selected->File, false};
			HostNodeCapture decoded;
			const uint64_t perImage = in.MaximumOperationBytes / (paths->Elements.size() + 1) / 4;
			if (!CaptureGraphRaster(
					{authored, in.Request, controls, {}, perImage, in.Timeline, in.OutputFormat},
					std::span<const GraphFileGrant>(&exact, 1),
					policy,
					decoded,
					failure
				))
				return false;
			auto image = std::move(decoded.Images.front().Data);
			if (images.empty()) {
				commonWidth = image.Width;
				commonHeight = image.Height;
			} else if (*canvas == 1) {
				commonWidth = std::min(commonWidth, image.Width);
				commonHeight = std::min(commonHeight, image.Height);
			} else if (*canvas == 2) {
				commonWidth = std::max(commonWidth, image.Width);
				commonHeight = std::max(commonHeight, image.Height);
			}
			images.push_back(std::move(image));
		}
		HostNodeCapture candidate;
		candidate.Authored = in.Authored;
		candidate.Tick = in.Request.Tick;
		candidate.Subframe = in.Request.Subframe;
		candidate.NegativeFrame = in.Request.NegativeFrame;
		candidate.Inputs.assign(in.Inputs.begin(), in.Inputs.end());
		ArrayValue dimensions;
		dimensions.ElementType = ValueType::Integer;
		uint64_t retained = 0;
		for (auto &original : images) {
			const double baseWidth = *canvas ? commonWidth : original.Width,
						 baseHeight = *canvas ? commonHeight : original.Height;
			const double width = baseWidth + padding->X + padding->Z,
						 height = baseHeight + padding->Y + padding->W;
			if (!std::isfinite(width) || !std::isfinite(height) || width != std::floor(width) ||
				height != std::floor(height) || width < 1 || height < 1 || width > Limits::MaximumDimension ||
				height > Limits::MaximumDimension)
				return fail("image array padded dimensions exceed bounded integer surface domain");
			const auto layout = CheckedSurfaceLayout(
				uint32_t(width), uint32_t(height), original.Format, in.MaximumOperationBytes / 4 - retained
			);
			if (!layout) return fail("image array output exceeds aggregate byte budget");
			const double scale =
				*canvas && *sizing ? std::min(baseWidth / original.Width, baseHeight / original.Height) : 1;
			const double left = *canvas ? (width - original.Width * scale) / 2 : padding->Z;
			const double top = *canvas ? (height - original.Height * scale) / 2 : padding->Y;
			// Plain source sprites leave device filtering disabled. Sample destination pixel centres.
			Image result;
			result.Width = uint32_t(width);
			result.Height = uint32_t(height);
			result.Format = original.Format;
			result.Pixels.resize(size_t(layout->Bytes));
			for (uint32_t y = 0; y < result.Height; ++y)
				for (uint32_t x = 0; x < result.Width; ++x) {
					const double sx = (x + .5 - left) / scale, sy = (y + .5 - top) / scale;
					SurfacePixel pixel{};
					if (sx >= 0 && sy >= 0 && sx < original.Width && sy < original.Height)
						if (!LoadSurfacePixel(
								original, uint32_t(std::floor(sx)), uint32_t(std::floor(sy)), pixel
							))
							return fail("image array source samples are invalid");
					if (!StoreSurfacePixel(result, x, y, pixel))
						return fail("image array output sample cannot be represented");
				}
			result.Hash = SurfaceHash(result);
			retained += result.Pixels.capacity();
			dimensions.Nested.push_back({int64_t(result.Width), int64_t(result.Height)});
			original = std::move(result);
		}
		candidate.Outputs = {{"paths", *paths}, {"dimensions", std::move(dimensions)}};
		candidate.ImageArrays.push_back({"surfaces_out", std::move(images)});
		out = std::move(candidate);
		return true;
	}

	static bool CaptureAnimated(
		const engine::imagegraph::HostNodeInvocation &in,
		std::span<const GraphFileGrant> grants,
		const engine::assets::ContentPolicy &policy,
		engine::imagegraph::HostNodeCapture &out,
		std::string &failure
	) {
		using namespace engine::imagegraph;
		const auto fail = [&](const char *message) {
			failure = message;
			return false;
		};
		const auto integer = [&](std::string_view port) -> std::optional<int64_t> {
			const auto *value = Input(in, port);
			if (!value) return {};
			if (const auto *v = std::get_if<int64_t>(value)) return *v;
			if (const auto *v = std::get_if<EnumValue>(value)) return v->Value;
			return {};
		};
		const auto boolean = [&](std::string_view port) -> const bool * {
			const auto *value = Input(in, port);
			return value ? std::get_if<bool>(value) : nullptr;
		};
		const auto canvas = integer("canvas_size"), loop = integer("loop_modes"),
				   start = integer("start_frame"), customFrame = integer("frame");
		const auto *stretch = boolean("stretch_frame"), *drawBefore = boolean("draw_before_start"),
				   *custom = boolean("custom_frame_order");
		const auto *rawSpeed = Input(in, "animation_speed"), *paths = Input(in, "path"),
				   *rawPadding = Input(in, "padding");
		const auto *paddingValue = rawPadding ? std::get_if<Vector4>(rawPadding) : nullptr;
		const auto padding = paddingValue ? RoundedPadding(*paddingValue) : std::nullopt;
		const auto *speedReal = rawSpeed ? std::get_if<double>(rawSpeed) : nullptr;
		const auto *speedInteger = rawSpeed ? std::get_if<int64_t>(rawSpeed) : nullptr;
		if (!paths || !padding || !canvas || *canvas < 0 || *canvas > 2 || !loop || *loop < 0 || *loop > 3 ||
			!start || !customFrame || !stretch || !drawBefore || !custom || (!speedReal && !speedInteger))
			return fail("animated image requires resolved source controls");
		const double raw = speedReal ? *speedReal : double(*speedInteger);
		if (!std::isfinite(raw)) return fail("animated image speed must be finite");
		if (!*stretch && raw == 0)
			return fail("zero animated image speed requires a source division-by-zero observation");
		if (std::abs(double(*start)) > (uint64_t{1} << 52) ||
			std::abs(double(*customFrame)) > (uint64_t{1} << 52))
			return fail("animated image frame controls exceed exact CPU frame domain");
		const auto authoredBytes = NodeClonePayloadBytes(in.Authored);
		if (!authoredBytes || *authoredBytes > in.MaximumOperationBytes / 8)
			return fail("animated image authored capture exceeds byte budget");
		uint64_t captured = *authoredBytes;
		for (const auto &input : in.Inputs) {
			const auto bytes = ValueClonePayloadBytes(input.Data);
			const uint64_t overhead = sizeof(AuthoredValue) + input.Port.size() + 1;
			if (!bytes || overhead > in.MaximumOperationBytes / 8 - captured ||
				*bytes > in.MaximumOperationBytes / 8 - captured - overhead)
				return fail("animated image resolved capture exceeds byte budget");
			captured += overhead + *bytes;
		}
		Node node;
		node.Id = in.Authored.Id;
		node.Type = "pc.image_sequence";
		std::array<AuthoredValue, 4> controls{
			{{"paths", *paths},
			 {"padding", Vector4{}},
			 {"canvas_size", EnumValue{0}},
			 {"sizing_method", EnumValue{0}}}
		};
		HostNodeCapture decoded;
		if (!CaptureSequence(
				{node, in.Request, controls, {}, in.MaximumOperationBytes / 2, in.Timeline, in.OutputFormat},
				grants,
				policy,
				decoded,
				failure
			))
			return false;
		const auto &frames = decoded.ImageArrays[0].Frames;
		if (frames.empty()) return fail("animated image has no admitted source frames");
		uint32_t baseWidth = frames[0].Width, baseHeight = frames[0].Height;
		for (const auto &frame : frames) {
			if (*canvas == 1) {
				baseWidth = std::min(baseWidth, frame.Width);
				baseHeight = std::min(baseHeight, frame.Height);
			}
			if (*canvas == 2) {
				baseWidth = std::max(baseWidth, frame.Width);
				baseHeight = std::max(baseHeight, frame.Height);
			}
		}
		const double width = baseWidth + padding->X + padding->Z,
					 height = baseHeight + padding->Y + padding->W;
		if (width < 1 || height < 1 || width > Limits::MaximumDimension || height > Limits::MaximumDimension)
			return fail("animated image padded dimensions exceed source surface bounds");
		const auto format = in.OutputFormat.value_or(SurfaceFormat::RGBA8Unorm);
		const auto layout =
			CheckedSurfaceLayout(uint32_t(width), uint32_t(height), format, in.MaximumOperationBytes / 4);
		if (!layout) return fail("animated image output exceeds byte budget");
		if (*stretch && (!in.Timeline || in.Timeline->Frames > Limits::MaximumTick))
			return fail("animated image stretch requires the authored timeline frame count");
		const double period = *stretch	 ? (double(in.Timeline->Frames) + 1) / frames.size()
							  : raw == 0 ? std::numeric_limits<double>::infinity()
										 : 1 / raw;
		const double clock =
			(double(in.Request.Tick) + in.Request.Subframe) * (in.Request.NegativeFrame ? -1 : 1);
		if (in.Request.Tick > (uint64_t{1} << 52) || !std::isfinite(in.Request.Subframe) ||
			in.Request.Subframe < 0 || in.Request.Subframe >= 1)
			return fail("animated image clock exceeds exact CPU frame domain");
		double selected = *custom ? double(*customFrame)
								  : std::floor(clock / (period == 0 ? 1 : period)) - (double(*start) - 1);
		if (!std::isfinite(selected)) return fail("animated image selected frame is not finite");
		bool draw = *drawBefore || selected >= 0;
		const double count = double(frames.size());
		// Source safe_mod preserves a negative remainder and returns zero for a zero divisor.
		if (*loop == 0) selected = std::fmod(selected, count);
		if (*loop == 1) {
			const double divisor = count * 2 - 2;
			selected = divisor == 0 ? 0 : std::fmod(selected, divisor);
			if (selected >= count) selected = divisor - selected;
		}
		if (*loop == 2) selected = std::min(selected, count - 1);
		if (selected < 0 || selected >= count) draw = false;
		Image image;
		image.Width = uint32_t(width);
		image.Height = uint32_t(height);
		image.Format = format;
		image.Pixels.resize(size_t(layout->Bytes));
		if (draw) {
			const auto &frame = frames[size_t(selected)];
			const double left = padding->Z + (double(baseWidth) - frame.Width) / 2,
						 top = padding->Y + (double(baseHeight) - frame.Height) / 2;
			for (uint32_t y = 0; y < image.Height; ++y)
				for (uint32_t x = 0; x < image.Width; ++x) {
					const double sx = x + .5 - left, sy = y + .5 - top;
					SurfacePixel pixel{};
					if (sx >= 0 && sy >= 0 && sx < frame.Width && sy < frame.Height)
						if (!LoadSurfacePixel(
								frame, uint32_t(std::floor(sx)), uint32_t(std::floor(sy)), pixel
							))
							return fail("animated image source sample is invalid");
					if (!StoreSurfacePixel(image, x, y, pixel))
						return fail("animated image output sample cannot be represented");
				}
		}
		image.Hash = SurfaceHash(image);
		HostNodeCapture candidate;
		candidate.Authored = in.Authored;
		candidate.Tick = in.Request.Tick;
		candidate.Subframe = in.Request.Subframe;
		candidate.NegativeFrame = in.Request.NegativeFrame;
		candidate.Inputs.assign(in.Inputs.begin(), in.Inputs.end());
		candidate.Outputs = {{"dimension", Vector2{width, height}}};
		candidate.Images.push_back({"surface_out", std::move(image)});
		out = std::move(candidate);
		return true;
	}

	bool CaptureGraphRaster(
		const engine::imagegraph::HostNodeInvocation &in,
		std::span<const GraphFileGrant> grants,
		const engine::assets::ContentPolicy &policy,
		engine::imagegraph::HostNodeCapture &out,
		std::string &failure,
		RasterFailure *classification
	) {
		using namespace engine::imagegraph;
		if (classification) *classification = RasterFailure::Refused;
		if (in.Authored.Type == "pc.image_animated") return CaptureAnimated(in, grants, policy, out, failure);
		if (in.Authored.Type == "pc.image_sequence") return CaptureSequence(in, grants, policy, out, failure);
		const auto fail = [&](const char *message, RasterFailure kind = RasterFailure::Refused) {
			if (classification) *classification = kind;
			failure = message;
			return false;
		};
		const auto *raw = Input(in, "path"), *padValue = Input(in, "padding");
		const auto *path = raw ? std::get_if<std::string>(raw) : nullptr;
		const auto *paddingValue = padValue ? std::get_if<Vector4>(padValue) : nullptr;
		const auto padding = paddingValue ? RoundedPadding(*paddingValue) : std::nullopt;
		if (!path || !padding) return fail("image requires resolved path and Vector4 source padding");
		const GraphFileGrant *grant = nullptr;
		for (const auto &g : grants)
			if (g.NodeId == in.Authored.Id && g.Resource.empty()) {
				if (grant) return fail("image primary file grant is duplicated");
				grant = &g;
			}
		if (!grant || grant->Write || path->empty() || *path != grant->File.string() ||
			!policy.AllowsName(*path))
			return fail("image read requires its exact node/path read grant");
		std::error_code error;
		if (!std::filesystem::is_regular_file(grant->File, error) || error)
			return fail("image source is not a regular file");
		const auto size = std::filesystem::file_size(grant->File, error);
		const auto authoredBytes = NodeClonePayloadBytes(in.Authored);
		if (!authoredBytes || *authoredBytes > in.MaximumOperationBytes / 4)
			return fail("image authored capture exceeds byte budget");
		uint64_t captureBytes = *authoredBytes;
		for (const auto &input : in.Inputs) {
			const auto valueBytes = ValueClonePayloadBytes(input.Data);
			const uint64_t overhead = sizeof(AuthoredValue) + input.Port.size() + 1;
			if (!valueBytes || overhead > in.MaximumOperationBytes / 4 - captureBytes ||
				*valueBytes > in.MaximumOperationBytes / 4 - captureBytes - overhead)
				return fail("image resolved capture exceeds byte budget");
			captureBytes += overhead + *valueBytes;
		}
		const uint64_t budget =
			std::min<uint64_t>(in.MaximumOperationBytes - captureBytes, 64ull * 1024 * 1024);
		if (!error && size == 0)
			return fail("image encoded source is empty", RasterFailure::SourceDecodeFailure);
		if (error || size > budget / 8) return fail("image encoded bytes exceed decoder budget");
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		std::ifstream stream(grant->File, std::ios::binary);
		stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		engine::core::Metrics::Count("image composer raster read operations", 1);
		engine::core::Metrics::Count("image composer raster encoded bytes read", double(stream.gcount()));
		if (!stream) return fail("cannot read complete granted image");
		const auto encodedFormat = engine::bake::ImageFormatOfBytes(bytes);
		using engine::assets::ContentForm;
		using engine::bake::ImageFormat;
		if (encodedFormat == ImageFormat::Unknown)
			return fail(
				"image encoded source has no recognized raster header", RasterFailure::SourceDecodeFailure
			);
		const auto actualForm = encodedFormat == ImageFormat::Png	 ? ContentForm::Png
								: encodedFormat == ImageFormat::Bmp	 ? ContentForm::Bmp
								: encodedFormat == ImageFormat::Jpeg ? ContentForm::Jpeg
								: encodedFormat == ImageFormat::Gif	 ? ContentForm::Gif
																	 : ContentForm::Unknown;
		if (!policy.Allows(actualForm))
			return fail("actual encoded image format is disabled by content policy");
		uint32_t width = 0, height = 0, frames = 0;
		bool frameLimit = false;
		if (!Dimensions(bytes, width, height, frames, frameLimit))
			return fail(
				"image header is malformed or outside native decoder coverage",
				!frameLimit && (encodedFormat == ImageFormat::Png || encodedFormat == ImageFormat::Gif)
					? RasterFailure::SourceDecodeFailure
					: RasterFailure::Refused
			);
		if (encodedFormat == ImageFormat::Png && bytes.size() >= 29) {
			const auto depth = std::to_integer<uint8_t>(bytes[24]);
			if ((depth != 8 && depth != 16) || bytes[28] != std::byte{0})
				return fail("PNG depth or interlacing is outside native still decoder coverage");
		}
		if (width > Limits::MaximumDimension || height > Limits::MaximumDimension ||
			!CheckedSurfaceLayout(width, height, SurfaceFormat::RGBA8Unorm, budget / 16))
			return fail("image header is unsupported or exceeds predecode dimensions/workspace budget");
		// Charge both frame copies, canvas/restore and bounded palette/LZW scratch before decoding.
		if (encodedFormat == ImageFormat::Gif &&
			(uint64_t(width) * height * 4 + 64) * (uint64_t(frames) + 2) * 2 + 64 * 1024 > budget / 2)
			return fail("GIF frame ledger exceeds predecode workspace budget");
		const std::array pad{padding->X, padding->Y, padding->Z, padding->W};
		const double w = width + pad[0] + pad[2], h = height + pad[1] + pad[3];
		const auto format = in.OutputFormat.value_or(SurfaceFormat::RGBA8Unorm);
		if (w < 1 || h < 1 || w > Limits::MaximumDimension || h > Limits::MaximumDimension)
			return fail("padded image dimensions exceed source surface bounds");
		const auto layout = CheckedSurfaceLayout(uint32_t(w), uint32_t(h), format, budget / 4);
		if (!layout) return fail("padded image exceeds output budget");
		engine::assets::TextureData decoded;
		engine::assets::TextureSequenceData sequence;
		std::span<const std::byte> pixels;
		engine::core::Metrics::Count("image composer raster decode operations", 1);
		if (encodedFormat == ImageFormat::Gif) {
			if (!engine::bake::ReadGifSequence(bytes, sequence, failure)) {
				if (classification) *classification = RasterFailure::SourceDecodeFailure;
				return false;
			}
			if (sequence.Width != width || sequence.Height != height ||
				sequence.FrameDurations.size() != frames)
				return fail("decoded GIF differs from its admitted frame ledger");
			pixels = sequence.FramePixels(0);
		} else {
			if (!engine::bake::ReadImage(bytes, decoded, failure)) {
				if (classification) *classification = RasterFailure::SourceDecodeFailure;
				return false;
			}
			if (decoded.Width != width || decoded.Height != height || decoded.FlipbookFrames)
				return fail("decoded image dimensions differ from admitted still header");
			pixels = decoded.Pixels;
		}
		Image image;
		image.Width = uint32_t(w);
		image.Height = uint32_t(h);
		image.Format = format;
		image.Pixels.resize(size_t(layout->Bytes));
		for (uint32_t y = 0; y < image.Height; ++y)
			for (uint32_t x = 0; x < image.Width; ++x) {
				const int64_t sx = int64_t(x) - int64_t(pad[2]), sy = int64_t(y) - int64_t(pad[1]);
				SurfacePixel pixel{};
				if (sx >= 0 && sy >= 0 && sx < width && sy < height) {
					const auto at = (uint64_t(sy) * width + uint64_t(sx)) * 4;
					for (size_t channel = 0; channel < 4; ++channel)
						pixel[channel] = std::to_integer<uint8_t>(pixels[size_t(at) + channel]) / 255.;
				}
				if (!StoreSurfacePixel(image, x, y, pixel)) return fail("image precision conversion failed");
			}
		image.Hash = SurfaceHash(image);
		HostNodeCapture candidate;
		candidate.Authored = in.Authored;
		candidate.Tick = in.Request.Tick;
		candidate.Subframe = in.Request.Subframe;
		candidate.NegativeFrame = in.Request.NegativeFrame;
		candidate.Inputs.assign(in.Inputs.begin(), in.Inputs.end());
		candidate.Outputs = {{"path", *path}, {"dimension", Vector2{w, h}}};
		candidate.Images.push_back({"surface_out", std::move(image)});
		out = std::move(candidate);
		return true;
	}
}
