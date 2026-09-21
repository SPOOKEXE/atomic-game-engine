#include <engine/render/PortalExchange.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

extern "C" int LLVMFuzzerInitialize(int *count, char ***arguments) {
	if (*count != 3 || std::string_view((*arguments)[1]) != "--write-seeds") return 0;
	const std::filesystem::path directory((*arguments)[2]);
	std::filesystem::create_directories(directory);
	engine::render::PortalImageLayerSet layers;
	auto &opaque = layers.Opaque;
	opaque.Key = {1, "door", 1, 1};
	opaque.Status = engine::render::PortalImageStatus::Ok;
	opaque.Scope = engine::render::PortalImageScope::OpaqueLighting;
	for (const uint32_t extent : {1u, 16u}) {
		opaque.Width = opaque.Height = extent;
		opaque.RowStride = extent * 8;
		opaque.Pixels.assign(size_t(extent) * extent * 8, std::byte{});
		opaque.Depth.assign(size_t(extent) * extent * 4, std::byte{});
		for (size_t pixel = 0; pixel < size_t(extent) * extent; ++pixel) {
			opaque.Pixels[pixel * 8 + 7] = std::byte{0x3c};
			opaque.Depth[pixel * 4 + 2] = std::byte{0x80};
			opaque.Depth[pixel * 4 + 3] = std::byte{0x3f};
		}
		opaque.PixelHash = engine::assets::Hasher::Of(opaque.Pixels);
		opaque.DepthHash = engine::assets::Hasher::Of(opaque.Depth);
		for (size_t members = 0; members <= engine::render::MAX_PORTAL_TRANSPARENT_LAYERS; ++members) {
			layers.Transparent.assign(members, opaque);
			std::vector<std::byte> wire;
			std::string error;
			if (!engine::render::EncodePortalImageLayerSet(layers, wire, error)) std::abort();
			std::ofstream file(
				directory / ("layers-" + std::to_string(extent) + "-" + std::to_string(members) + ".pimg"),
				std::ios::binary
			);
			file.write(
				reinterpret_cast<const char *>(wire.data()), static_cast<std::streamsize>(wire.size())
			);
			if (!file.good()) std::abort();
		}
	}
	for (const bool ordered : {false, true}) {
		for (const bool eye : {false, true}) {
			engine::render::PortalImageRequest request;
			request.Key = opaque.Key;
			request.Width = request.Height = 256;
			request.PixelBudget = engine::render::MAX_PORTAL_IMAGE_PIXELS;
			request.Scope = engine::render::PortalImageScope::OpaqueLighting;
			request.OrderedLayers = ordered;
			if (eye) {
				request.Projection = engine::render::PortalImageProjection::Eye;
				request.ClipPlane = {};
			}
			std::vector<std::byte> wire;
			std::string error;
			if (!engine::render::EncodePortalImageRequest(request, wire, error)) std::abort();
			std::ofstream file(
				directory / ("request-" + std::to_string(ordered) + "-" + std::to_string(eye) + ".pimg"),
				std::ios::binary
			);
			file.write(
				reinterpret_cast<const char *>(wire.data()), static_cast<std::streamsize>(wire.size())
			);
			if (!file.good()) std::abort();
		}
	}
	std::exit(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size) {
	using namespace engine::render;
	const auto prefix =
		MatchPortalImageLayerSet(std::as_bytes(std::span(input, size)), {1, "door", 1, 1}, 16, 16);
	if (prefix && (prefix->Status != PortalImageStatus::Ok || prefix->DepthBytes != 3 * 16 * 16 * 4))
		std::abort();
	PortalImageRequest request;
	request.Key.PortalKey = "untouched";
	const auto original = request;
	std::string requestError;
	if (DecodePortalImageRequest(std::as_bytes(std::span(input, size)), request, requestError)) {
		std::vector<std::byte> wire;
		PortalImageRequest roundtrip;
		if (!EncodePortalImageRequest(request, wire, requestError) ||
			!DecodePortalImageRequest(wire, roundtrip, requestError) || roundtrip != request)
			std::abort();
	} else if (request != original || requestError.empty())
		std::abort();
	PortalImageLayerSet layers;
	layers.Opaque.Key.PortalKey = "untouched";
	std::string error;
	if (!DecodePortalImageLayerSet(std::as_bytes(std::span(input, size)), layers, error)) {
		if (layers.Opaque.Key.PortalKey != "untouched" || !layers.Transparent.empty() || error.empty())
			std::abort();
		return 0;
	}
	const auto match = MatchPortalImageLayerSet(
		std::as_bytes(std::span(input, size)), layers.Opaque.Key, layers.Opaque.Width, layers.Opaque.Height
	);
	if (layers.Transparent.size() == MAX_PORTAL_TRANSPARENT_LAYERS) {
		if (!match || match->CaptureTick != layers.Opaque.CaptureTick ||
			match->DepthBytes != layers.Opaque.Depth.size() * (MAX_PORTAL_TRANSPARENT_LAYERS + 1))
			std::abort();
	} else if (match)
		std::abort();
	std::vector<std::byte> wire;
	PortalImageLayerSet roundtrip;
	if (!ValidPortalImageLayerSet(layers) || !EncodePortalImageLayerSet(layers, wire, error) ||
		!DecodePortalImageLayerSet(wire, roundtrip, error) || roundtrip != layers)
		std::abort();
	return 0;
}
