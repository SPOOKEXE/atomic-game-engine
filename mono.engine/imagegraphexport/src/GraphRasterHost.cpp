#include "GraphRasterHost.hpp"

#include <engine/bake/Image.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		const Value *Input(const HostNodeInvocation &in, std::string_view port) {
			for (const auto &v : in.Inputs)
				if (v.Port == port) return &v.Data;
			return nullptr;
		}
		uint32_t Word(std::span<const std::byte> bytes, size_t at, size_t count, bool big) {
			uint32_t out = 0;
			for (size_t i = 0; i < count; ++i)
				out |= uint32_t(std::to_integer<uint8_t>(bytes[at + i])) << (8 * (big ? count - 1 - i : i));
			return out;
		}
		// Admit decoder workspace from the actual header before invoking the codec.
		bool Dimensions(std::span<const std::byte> bytes, uint32_t &width, uint32_t &height) {
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
		const auto *padding = padRaw ? std::get_if<Vector4>(padRaw) : nullptr;
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
			// Integer aligned copies are source exact. Scaled raster sampling needs the source filter
			// observation.
			if (scale != 1 || left != std::floor(left) || top != std::floor(top))
				return fail(
					"fractional or scaled image-array source sampling requires an explicit filter parity "
					"observation"
				);
			Image result;
			result.Width = uint32_t(width);
			result.Height = uint32_t(height);
			result.Format = original.Format;
			result.Pixels.resize(size_t(layout->Bytes));
			for (uint32_t y = 0; y < result.Height; ++y)
				for (uint32_t x = 0; x < result.Width; ++x) {
					const int64_t sx = int64_t(x) - int64_t(left), sy = int64_t(y) - int64_t(top);
					SurfacePixel pixel{};
					if (sx >= 0 && sy >= 0 && sx < original.Width && sy < original.Height)
						if (!LoadSurfacePixel(original, uint32_t(sx), uint32_t(sy), pixel))
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

	bool CaptureGraphRaster(
		const engine::imagegraph::HostNodeInvocation &in,
		std::span<const GraphFileGrant> grants,
		const engine::assets::ContentPolicy &policy,
		engine::imagegraph::HostNodeCapture &out,
		std::string &failure
	) {
		using namespace engine::imagegraph;
		if (in.Authored.Type == "pc.image_sequence") return CaptureSequence(in, grants, policy, out, failure);
		const auto fail = [&](const char *message) {
			failure = message;
			return false;
		};
		const auto *raw = Input(in, "path"), *padValue = Input(in, "padding");
		const auto *path = raw ? std::get_if<std::string>(raw) : nullptr;
		const auto *padding = padValue ? std::get_if<Vector4>(padValue) : nullptr;
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
		if (error || size == 0 || size > budget / 8) return fail("image encoded bytes exceed decoder budget");
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		std::ifstream stream(grant->File, std::ios::binary);
		stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (!stream) return fail("cannot read complete granted image");
		uint32_t width = 0, height = 0;
		if (!Dimensions(bytes, width, height) || width > Limits::MaximumDimension ||
			height > Limits::MaximumDimension ||
			!CheckedSurfaceLayout(width, height, SurfaceFormat::RGBA8Unorm, budget / 16))
			return fail("image header is unsupported or exceeds predecode dimensions/workspace budget");
		const std::array pad{padding->X, padding->Y, padding->Z, padding->W};
		for (double n : pad)
			if (!std::isfinite(n) || n != std::floor(n) || std::abs(n) > Limits::MaximumDimension)
				return fail("fractional image padding source rasterization is unverified");
		const double w = width + pad[0] + pad[2], h = height + pad[1] + pad[3];
		const auto format = in.OutputFormat.value_or(SurfaceFormat::RGBA8Unorm);
		if (w < 1 || h < 1 || w > Limits::MaximumDimension || h > Limits::MaximumDimension)
			return fail("padded image dimensions exceed source surface bounds");
		const auto layout = CheckedSurfaceLayout(uint32_t(w), uint32_t(h), format, budget / 4);
		if (!layout) return fail("padded image exceeds output budget");
		engine::assets::TextureData decoded;
		if (!engine::bake::ReadImage(bytes, decoded, failure)) return false;
		if (decoded.Width != width || decoded.Height != height || decoded.FlipbookFrames)
			return fail("decoded image dimensions differ from admitted still header");
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
						pixel[channel] =
							std::to_integer<uint8_t>(decoded.Pixels[size_t(at) + channel]) / 255.;
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
