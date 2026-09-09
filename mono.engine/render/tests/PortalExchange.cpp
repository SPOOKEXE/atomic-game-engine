#include <engine/core/Bytes.hpp>
#include <engine/render/PortalCaptureTree.hpp>
#include <engine/render/PortalExchange.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <limits>

TEST_SUITE_ID("engine.render.portalexchange")

using namespace engine::render;
using engine::assets::Hasher;

TEST_CASE(
	"resident portal receipts are bounded exact metadata without device handles", "[render][portal-exchange]"
) {
	PortalResidentReceipt receipt;
	receipt.Key = {13, "Workspace/Door", 19, 23};
	receipt.CaptureTick = 101;
	receipt.ContentRevision = 103;
	receipt.LightingRevision = 107;
	receipt.Width = 512;
	receipt.Height = 287;
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalResidentReceipt(receipt, wire, error));
	CHECK(wire.size() < 128);
	PortalResidentReceipt decoded;
	REQUIRE(DecodePortalResidentReceipt(wire, decoded, error));
	CHECK(decoded == receipt);
	PortalImageReply copied;
	CHECK_FALSE(DecodePortalImageReply(wire, copied, error));
	for (size_t size = 0; size < wire.size(); ++size) {
		CHECK_FALSE(DecodePortalResidentReceipt(std::span(wire).first(size), decoded, error));
		CHECK(decoded == receipt);
		CHECK_FALSE(error.empty());
	}
	auto extra = wire;
	extra.push_back(std::byte{});
	CHECK_FALSE(DecodePortalResidentReceipt(extra, decoded, error));
	CHECK(decoded == receipt);
	for (const uint32_t width : {0u, MAX_PORTAL_IMAGE_EXTENT + 1}) {
		auto invalid = receipt;
		invalid.Width = width;
		CHECK_FALSE(EncodePortalResidentReceipt(invalid, extra, error));
		CHECK(extra.size() == wire.size() + 1);
	}
	receipt.Key.PortalKey.assign(256, 'x');
	REQUIRE(EncodePortalResidentReceipt(receipt, wire, error));
	REQUIRE(DecodePortalResidentReceipt(wire, decoded, error));
	CHECK(decoded == receipt);
	receipt.Key.PortalKey.push_back('x');
	CHECK_FALSE(EncodePortalResidentReceipt(receipt, wire, error));
}

namespace {
	PortalImageRequest Request() {
		PortalImageRequest request;
		request.Key = {0x0807060504030201, "Workspace/Portal", 13, 27};
		request.Position = {2, 3, -4};
		request.Frustum = {-0.7f, 1.3f, -0.4f, 0.6f, 0.1f, 500};
		request.Width = 2;
		request.Height = 1;
		request.PixelBudget = 12;
		request.RecursionDepth = 2;
		return request;
	}
	PortalImageReply Reply() {
		PortalImageReply reply;
		reply.Key = Request().Key;
		reply.Status = PortalImageStatus::Ok;
		reply.CaptureTick = 17;
		reply.ContentRevision = 81;
		reply.LightingRevision = 98;
		reply.Width = 2;
		reply.Height = 1;
		reply.RowStride = 16;
		// Two different linear HDR pixels; 0x4000 is half-float 2, above display white.
		reply.Pixels = {
			std::byte{0},
			std::byte{0x40},
			std::byte{0},
			std::byte{0x38},
			std::byte{0},
			std::byte{0},
			std::byte{0},
			std::byte{0x3c},
			std::byte{0},
			std::byte{0},
			std::byte{0},
			std::byte{0x3c},
			std::byte{0},
			std::byte{0x38},
			std::byte{0},
			std::byte{0x3c}
		};
		reply.PixelHash = Hasher::Of(reply.Pixels);
		return reply;
	}
}

TEST_CASE("captured sunlight is bounded and paired across reply kinds", "[render][portal-exchange]") {
	const PortalCaptureLighting light{{0, 0, -1}, {.1f, .2f, .3f}, {.4f, .5f, .6f}, {2, 3, 4}};
	auto reply = Reply();
	reply.CaptureLighting = light;
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageReply(reply, wire, error));
	PortalImageReply decoded;
	REQUIRE(DecodePortalImageReply(wire, decoded, error));
	CHECK(decoded == reply);
	REQUIRE(MatchPortalImageReply(wire, reply.Key, reply.Width, reply.Height));
	const auto validWire = wire;
	for (int invalid = 0; invalid < 4; ++invalid) {
		auto bad = reply;
		if (invalid == 0) bad.CaptureLighting->Direction = {};
		if (invalid == 1) bad.CaptureLighting->Ambient[0] = -1;
		if (invalid == 2) bad.CaptureLighting->OutdoorAmbient[1] = std::numeric_limits<float>::infinity();
		if (invalid == 3) bad.CaptureLighting->Direct[2] = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(EncodePortalImageReply(bad, wire, error));
		CHECK(wire == validWire);
	}
	// The tiny uncompressed pair keeps the sunlight immediately before its hash and payload.
	const size_t lightOffset = wire.size() - reply.Pixels.size() - 36 - 69;
	for (size_t component = 0; component < 12; ++component) {
		auto malformed = wire;
		engine::core::ByteWriter nan;
		nan.WriteFloat(std::numeric_limits<float>::quiet_NaN());
		std::copy(nan.Bytes().begin(), nan.Bytes().end(), malformed.begin() + lightOffset + component * 4);
		CHECK_FALSE(DecodePortalImageReply(malformed, decoded, error));
		CHECK(decoded == reply);
	}
	PortalResidentReceipt receipt{reply.Key, reply.Scope, 1, 2, 3, reply.Width, reply.Height, light};
	for (const bool renewal : {false, true}) {
		const auto encode = renewal ? EncodePortalImageRenewal : EncodePortalResidentReceipt;
		const auto decode = renewal ? DecodePortalImageRenewal : DecodePortalResidentReceipt;
		REQUIRE(encode(receipt, wire, error));
		PortalResidentReceipt accepted;
		REQUIRE(decode(wire, accepted, error));
		CHECK(accepted == receipt);
		for (size_t length = 0; length < wire.size(); ++length) {
			CHECK_FALSE(decode(std::span(wire).first(length), accepted, error));
			CHECK(accepted == receipt);
		}
		auto bad = receipt;
		bad.CaptureLighting->Direction[1] = 1;
		const auto previous = wire;
		CHECK_FALSE(encode(bad, wire, error));
		CHECK(wire == previous);
		receipt.Key.PortalKey.assign(256, 'x');
		REQUIRE(encode(receipt, wire, error));
		REQUIRE(decode(wire, accepted, error));
		CHECK(accepted == receipt);
	}
}

TEST_CASE("captured local lights and fog retain bounded shading inputs", "[render][portal-exchange]") {
	auto reply = Reply();
	auto &lighting = reply.CaptureLighting.emplace();
	lighting.FogColour = {.2f, .3f, .4f};
	lighting.FogStart = 3;
	lighting.FogEnd = 19;
	lighting.LightCount = MAX_PORTAL_CAPTURE_LIGHTS;
	for (size_t index = 0; index < lighting.LightCount; ++index) {
		auto &light = lighting.Lights[index];
		light.Position = {float(index), 2, -3};
		light.Colour = {1, 2, 3};
		light.Range = float(index + 1);
		light.ConeCosine = index % 2 ? .5f : -1;
	}
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageReply(reply, wire, error));
	PortalImageReply decoded;
	REQUIRE(DecodePortalImageReply(wire, decoded, error));
	CHECK(decoded == reply);
	const auto original = wire;
	for (int invalid = 0; invalid < 9; ++invalid) {
		auto bad = reply;
		auto &light = *bad.CaptureLighting;
		if (invalid == 0) light.LightCount++;
		if (invalid == 1) light.FogEnd = light.FogStart;
		if (invalid == 2) light.FogColour[0] = -1;
		if (invalid == 3) light.Lights[0].Range = 0;
		if (invalid == 4) light.Lights[0].Position[1] = std::numeric_limits<float>::infinity();
		if (invalid == 5) light.Lights[0].Colour[2] = -1;
		if (invalid == 6) light.Lights[1].Direction = {};
		if (invalid == 7) light.Lights[0].ConeCosine = 2;
		if (invalid == 8) light.LightCount = 0;
		CHECK_FALSE(EncodePortalImageReply(bad, wire, error));
		CHECK(wire == original);
	}
	const size_t lightingOffset =
		wire.size() - reply.Pixels.size() - 36 - 69 - 44 * MAX_PORTAL_CAPTURE_LIGHTS;
	auto excessive = wire;
	excessive[lightingOffset + 68] = std::byte{MAX_PORTAL_CAPTURE_LIGHTS + 1};
	CHECK_FALSE(DecodePortalImageReply(excessive, decoded, error));
	CHECK(decoded == reply);
	PortalResidentReceipt receipt{reply.Key, reply.Scope, 1, 2, 3, reply.Width, reply.Height, lighting};
	receipt.Key.PortalKey.assign(256, 'x');
	for (const bool renewal : {false, true}) {
		const auto encode = renewal ? EncodePortalImageRenewal : EncodePortalResidentReceipt;
		const auto decode = renewal ? DecodePortalImageRenewal : DecodePortalResidentReceipt;
		REQUIRE(encode(receipt, wire, error));
		PortalResidentReceipt accepted;
		REQUIRE(decode(wire, accepted, error));
		CHECK(accepted == receipt);
	}
}

TEST_CASE("portal entrance metadata rejects invalid apertures", "[render][portal-exchange]") {
	const PortalImageEntrance entrance{"source", {2, 3, 4}, {1, 0, 0}, {0, 2, 0}};
	std::string error;
	std::vector<std::byte> output{std::byte{123}};
	for (int invalid = 0; invalid < 6; ++invalid) {
		auto request = Request();
		request.Entrance = entrance;
		auto &bad = *request.Entrance;
		if (invalid == 0) bad.SourceWorld.clear();
		if (invalid == 1) bad.SourceWorld.assign(257, 'x');
		if (invalid == 2) bad.SourceWorld.push_back('\0');
		if (invalid == 3) bad.Centre[0] = std::numeric_limits<float>::infinity();
		if (invalid == 4) bad.First = {};
		if (invalid == 5) bad.Second = bad.First;
		CHECK_FALSE(EncodePortalImageRequest(request, output, error));
		CHECK(output == std::vector<std::byte>{std::byte{123}});
	}
}

TEST_CASE("whole eye views have explicit bounded projection semantics", "[render][portal-exchange]") {
	auto request = Request();
	request.Projection = PortalImageProjection::Eye;
	request.ClipPlane = {};
	std::vector<std::byte> bytes;
	std::string error;
	REQUIRE(EncodePortalImageRequest(request, bytes, error));
	PortalImageRequest decoded;
	REQUIRE(DecodePortalImageRequest(bytes, decoded, error));
	CHECK(decoded == request);
	for (const auto invalid : {0, 1, 2}) {
		auto bad = request;
		if (invalid == 0) bad.Projection = static_cast<PortalImageProjection>(2);
		if (invalid == 1) bad.ClipPlane = {0, 0, 1, 0};
		if (invalid == 2) bad.Entrance = PortalImageEntrance{"Near", {}, {1, 0, 0}, {0, 1, 0}};
		auto output = bytes;
		CHECK_FALSE(EncodePortalImageRequest(bad, output, error));
		CHECK(output == bytes);
	}
	request.Projection = PortalImageProjection::Seam;
	CHECK_FALSE(EncodePortalImageRequest(request, bytes, error));
}

TEST_CASE(
	"portal image messages preserve projection identity and linear half pixels", "[render][portal-exchange]"
) {
	std::string error;
	std::vector<std::byte> bytes;
	auto request = Request();
	request.KnownImage = engine::render::PortalImageVersion{81, 98};
	request.Entrance = PortalImageEntrance{"source", {2, 3, 4}, {1, 0, 0}, {0, 2, 0}};
	REQUIRE(EncodePortalImageRequest(request, bytes, error));
	CHECK(error.empty());
	REQUIRE(bytes.size() > 16);
	CHECK(bytes[0] == std::byte{'P'});
	CHECK(bytes[1] == std::byte{'I'});
	CHECK(bytes[4] == std::byte{19});
	CHECK(bytes[6] == std::byte{1});
	for (size_t index = 0; index < 8; ++index) {
		CHECK(bytes[8 + index] == std::byte(index + 1));
	}
	PortalImageRequest decoded;
	REQUIRE(DecodePortalImageRequest(bytes, decoded, error));
	CHECK(decoded == request);
	std::vector<std::byte> repeated;
	REQUIRE(EncodePortalImageRequest(decoded, repeated, error));
	CHECK(bytes == repeated);
	const auto reply = Reply();
	REQUIRE(EncodePortalImageReply(reply, bytes, error));
	PortalImageReply image;
	REQUIRE(DecodePortalImageReply(bytes, image, error));
	CHECK(image == reply);
	REQUIRE(EncodePortalImageReply(image, repeated, error));
	CHECK(bytes == repeated);
	CHECK_FALSE(DecodePortalImageRequest(bytes, decoded, error));
	CHECK(decoded == request);
}

TEST_CASE(
	"portal requests reject malformed lenses poses budgets and framing transactionally",
	"[render][portal-exchange]"
) {
	std::string error;
	std::vector<std::byte> bytes;
	auto request = Request();
	request.Entrance = PortalImageEntrance{"source", {2, 3, 4}, {1, 0, 0}, {0, 2, 0}};
	REQUIRE(EncodePortalImageRequest(request, bytes, error));
	PortalImageRequest output = request;
	for (size_t length = 0; length < bytes.size(); ++length) {
		CHECK_FALSE(DecodePortalImageRequest(std::span(bytes).first(length), output, error));
		CHECK_FALSE(error.empty());
		CHECK(output == request);
	}
	for (size_t offset : {size_t(4), size_t(7), size_t(16), size_t(17), size_t(18), size_t(19)}) {
		auto corrupt = bytes;
		corrupt[offset] = std::byte{0xff};
		CHECK_FALSE(DecodePortalImageRequest(corrupt, output, error));
		CHECK(output == request);
	}
	auto trailing = bytes;
	trailing.push_back(std::byte{0});
	CHECK_FALSE(DecodePortalImageRequest(trailing, output, error));
	for (int invalid = 0; invalid < 17; ++invalid) {
		auto bad = request;
		switch (invalid) {
		case 0:
			bad.Position[1] = std::numeric_limits<float>::infinity();
			break;
		case 1:
			bad.Orientation = {};
			break;
		case 2:
			bad.Frustum[1] = bad.Frustum[0];
			break;
		case 3:
			bad.Frustum[3] = bad.Frustum[2];
			break;
		case 4:
			bad.Frustum[4] = 0;
			break;
		case 5:
			bad.Frustum[5] = bad.Frustum[4];
			break;
		case 6:
			bad.ClipPlane = {};
			break;
		case 7:
			bad.ClipPlane[3] = std::numeric_limits<float>::quiet_NaN();
			break;
		case 8:
			bad.Width = std::numeric_limits<uint32_t>::max();
			break;
		case 9:
			bad.PixelBudget = 1;
			break;
		case 10:
			bad.PixelBudget = MAX_PORTAL_IMAGE_PIXELS + 1;
			break;
		case 11:
			bad.RecursionDepth = MAX_PORTAL_IMAGE_RECURSION + 1;
			break;
		case 12:
			bad.Key.PortalKey = std::string(257, 'p');
			break;
		case 13:
			bad.Key.RequestId = 0;
			break;
		case 14:
			bad.Key.PortalKey.push_back('\0');
			break;
		case 15:
			bad.Orientation[3] = 1.01f;
			break;
		case 16:
			bad.Frustum[4] = std::numeric_limits<float>::quiet_NaN();
			break;
		}
		auto unchanged = bytes;
		CHECK_FALSE(EncodePortalImageRequest(bad, unchanged, error));
		CHECK(unchanged == bytes);
	}
}

TEST_CASE(
	"portal image replies reject corruption padding overflow and nonfinite half samples",
	"[render][portal-exchange]"
) {
	std::string error;
	std::vector<std::byte> bytes;
	const auto reply = Reply();
	REQUIRE(EncodePortalImageReply(reply, bytes, error));
	PortalImageReply output = reply;
	for (size_t length = 0; length < bytes.size(); ++length) {
		CHECK_FALSE(DecodePortalImageReply(std::span(bytes).first(length), output, error));
		CHECK(output == reply);
	}
	// Header, key, then status, scope and two reserved zero bytes.
	const size_t status = 8 + 8 + 4 + reply.Key.PortalKey.size() + 16;
	const size_t count = bytes.size() - reply.Pixels.size() - 4;
	for (size_t offset :
		 {size_t(7), status, status + 1, status + 2, status + 3, count, count + 3, bytes.size() - 1}) {
		auto corrupt = bytes;
		corrupt[offset] = std::byte{0xff};
		CHECK_FALSE(DecodePortalImageReply(corrupt, output, error));
		CHECK_FALSE(error.empty());
		CHECK(output == reply);
	}
	// A finite pixel bit flip must fail digest validation, independently of sample validation.
	auto mismatched = bytes;
	mismatched.back() = std::byte{0x38};
	CHECK_FALSE(DecodePortalImageReply(mismatched, output, error));
	CHECK(output == reply);
	// A correctly hashed infinity must still fail the linear pixel contract.
	auto nonfinite = reply.Pixels;
	nonfinite[1] = std::byte{0x7c};
	const auto nonfiniteHash = Hasher::Of(nonfinite);
	auto corruptSamples = bytes;
	std::copy(nonfinite.begin(), nonfinite.end(), corruptSamples.end() - nonfinite.size());
	for (size_t index = 0; index < nonfiniteHash.Digest.size(); ++index) {
		corruptSamples[count - nonfiniteHash.Digest.size() + index] = std::byte{nonfiniteHash.Digest[index]};
	}
	CHECK_FALSE(DecodePortalImageReply(corruptSamples, output, error));
	CHECK(output == reply);
	auto trailing = bytes;
	trailing.push_back(std::byte{0});
	CHECK_FALSE(DecodePortalImageReply(trailing, output, error));
	for (int invalid = 0; invalid < 5; ++invalid) {
		auto bad = reply;
		switch (invalid) {
		case 0:
			bad.RowStride += 8;
			break;
		case 1:
			bad.Height = std::numeric_limits<uint32_t>::max();
			break;
		case 2:
			bad.Pixels[1] = std::byte{0x7c};
			bad.PixelHash = Hasher::Of(bad.Pixels);
			break;
		case 3:
			bad.PixelHash = {};
			break;
		case 4:
			bad.Status = PortalImageStatus::Failed;
			break;
		}
		auto unchanged = bytes;
		CHECK_FALSE(EncodePortalImageReply(bad, unchanged, error));
		CHECK(unchanged == bytes);
	}
}

TEST_CASE(
	"portal exchange bounds maximum image and carries explicit no-image statuses", "[render][portal-exchange]"
) {
	std::string error;
	std::vector<std::byte> bytes;
	auto reply = Reply();
	reply.Width = MAX_PORTAL_IMAGE_EXTENT;
	reply.Height = MAX_PORTAL_IMAGE_EXTENT;
	reply.RowStride = reply.Width * 8;
	reply.Pixels.assign(size_t(reply.RowStride) * reply.Height, std::byte{0});
	reply.PixelHash = Hasher::Of(reply.Pixels);
	REQUIRE(EncodePortalImageReply(reply, bytes, error));
	CHECK(bytes.size() < MAX_PORTAL_EXCHANGE_BYTES);
	PortalImageReply decoded;
	REQUIRE(DecodePortalImageReply(bytes, decoded, error));
	CHECK(decoded == reply);
	for (auto status :
		 {PortalImageStatus::Unavailable,
		  PortalImageStatus::Stale,
		  PortalImageStatus::BudgetExceeded,
		  PortalImageStatus::Unsupported,
		  PortalImageStatus::Failed}) {
		PortalImageReply failed;
		failed.Key = Request().Key;
		failed.Status = status;
		failed.Diagnostic = "capture unavailable";
		REQUIRE(EncodePortalImageReply(failed, bytes, error));
		REQUIRE(DecodePortalImageReply(bytes, decoded, error));
		CHECK(decoded == failed);
	}
	const auto previous = decoded;
	bytes.assign(MAX_PORTAL_EXCHANGE_BYTES + 1, std::byte{0});
	CHECK_FALSE(DecodePortalImageReply(bytes, decoded, error));
	CHECK(decoded == previous);
}

TEST_CASE(
	"portal image compression preserves HDR bytes and rejects malformed frames", "[render][portal-exchange]"
) {
	auto reply = Reply();
	reply.Width = reply.Height = 128;
	reply.RowStride = reply.Width * 8;
	const auto pixel = reply.Pixels;
	reply.Pixels.resize(size_t(reply.RowStride) * reply.Height);
	for (size_t index = 0; index < reply.Pixels.size(); ++index)
		reply.Pixels[index] = pixel[index % pixel.size()];
	reply.PixelHash = Hasher::Of(reply.Pixels);
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageReply(reply, wire, error));
	CHECK(wire.size() < 1024);
	REQUIRE(MatchPortalImageReply(wire, reply.Key, reply.Width, reply.Height, reply.Scope));
	PortalImageReply decoded;
	REQUIRE(DecodePortalImageReply(wire, decoded, error));
	CHECK(decoded == reply);
	for (size_t size = 0; size < wire.size(); ++size) {
		CHECK_FALSE(DecodePortalImageReply(std::span(wire).first(size), decoded, error));
		CHECK(decoded == reply);
	}
	const size_t status = 8 + 8 + 4 + reply.Key.PortalKey.size() + 16;
	REQUIRE(wire[status + 2] == std::byte{1});
	// Status, codec, dimensions, stride, digest and the compressed frame are independent gates.
	for (size_t offset :
		 {status, status + 2, status + 32, status + 36, status + 40, status + 44, wire.size() - 1}) {
		auto corrupt = wire;
		corrupt[offset] ^= std::byte{0xff};
		CHECK_FALSE(DecodePortalImageReply(corrupt, decoded, error));
		CHECK(decoded == reply);
	}
	// High-entropy finite samples should retain raw storage when the frame would grow.
	reply.Width = 64;
	reply.Height = 1;
	reply.RowStride = reply.Width * 8;
	reply.Pixels.resize(reply.RowStride);
	uint32_t random = 0x6a09e667;
	for (size_t index = 0; index < reply.Pixels.size(); ++index) {
		random ^= random << 13;
		random ^= random >> 17;
		random ^= random << 5;
		uint8_t byte = static_cast<uint8_t>(random);
		if (index % 2 == 1 && (byte & 0x7c) == 0x7c) byte ^= 4;
		reply.Pixels[index] = std::byte{byte};
	}
	reply.PixelHash = Hasher::Of(reply.Pixels);
	REQUIRE(EncodePortalImageReply(reply, wire, error));
	CHECK(wire[status + 2] == std::byte{0});
	REQUIRE(DecodePortalImageReply(wire, decoded, error));
	CHECK(decoded == reply);
}

TEST_CASE("portal content scope is explicit and part of reply admission", "[render][portal-exchange]") {
	auto request = Request();
	CHECK(request.Scope == PortalImageScope::CompleteWorld);
	request.Scope = PortalImageScope::OpaqueLighting;
	std::vector<std::byte> bytes;
	std::string error;
	REQUIRE(EncodePortalImageRequest(request, bytes, error));
	PortalImageRequest decoded;
	REQUIRE(DecodePortalImageRequest(bytes, decoded, error));
	CHECK(decoded == request);
	const size_t scope = 8 + 8 + 4 + request.Key.PortalKey.size() + 16;
	for (size_t offset = scope; offset < scope + 4; ++offset) {
		auto malformed = bytes;
		malformed[offset] = std::byte{0xff};
		CHECK_FALSE(DecodePortalImageRequest(malformed, decoded, error));
		CHECK(decoded == request);
	}
	auto reply = Reply();
	reply.Scope = request.Scope;
	REQUIRE(EncodePortalImageReply(reply, bytes, error));
	CHECK_FALSE(MatchPortalImageReply(bytes, reply.Key, reply.Width, reply.Height));
	CHECK(MatchPortalImageReply(bytes, reply.Key, reply.Width, reply.Height, request.Scope));
	PortalImageReply image;
	REQUIRE(DecodePortalImageReply(bytes, image, error));
	CHECK(image == reply);
}

TEST_CASE("portal renewal metadata is distinct and bounded", "[render][portal-exchange]") {
	using namespace engine::render;
	PortalResidentReceipt receipt;
	receipt.Key = {1, "Door", 2, 3};
	receipt.Width = 32;
	receipt.Height = 16;
	receipt.ContentRevision = 4;
	receipt.LightingRevision = 5;
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageRenewal(receipt, wire, error));
	PortalResidentReceipt decoded;
	REQUIRE(DecodePortalImageRenewal(wire, decoded, error));
	CHECK(decoded == receipt);
	CHECK_FALSE(DecodePortalResidentReceipt(wire, decoded, error));
	for (size_t size = 0; size < wire.size(); ++size) {
		CHECK_FALSE(DecodePortalImageRenewal(std::span(wire).first(size), decoded, error));
		CHECK(decoded == receipt);
	}
}

TEST_CASE(
	"primary capture player identity is canonical bounded text", "[render][portal-exchange][eye-body]"
) {
	auto request = Request();
	request.Projection = GENERATE(PortalImageProjection::Eye, PortalImageProjection::Seam);
	if (request.Projection == PortalImageProjection::Eye) request.ClipPlane = {};
	std::vector<std::byte> bytes;
	std::string error;
	for (const std::string identity : {"", "0", "91", "-9223372036854775808", "9223372036854775807"}) {
		request.EyePlayer = identity;
		REQUIRE(EncodePortalImageRequest(request, bytes, error));
		PortalImageRequest decoded;
		REQUIRE(DecodePortalImageRequest(bytes, decoded, error));
		CHECK(decoded == request);
	}
	for (const std::string identity :
		 {"+1", "01", "-0", "1x", " 1", "9223372036854775808", "-9223372036854775809"}) {
		request.EyePlayer = identity;
		CHECK_FALSE(EncodePortalImageRequest(request, bytes, error));
	}
}

TEST_CASE("portal replies pair bounded depth with radiance", "[render][portal-exchange][portal-depth]") {
	auto reply = Reply();
	const uint32_t extent = GENERATE(2u, MAX_PORTAL_IMAGE_EXTENT);
	reply.Width = reply.Height = extent;
	reply.RowStride = extent * 8;
	reply.Pixels.assign(size_t(extent) * extent * 8, std::byte{});
	reply.PixelHash = Hasher::Of(reply.Pixels);
	engine::core::ByteWriter depths;
	for (size_t i = 0; i < size_t(extent) * extent; ++i)
		depths.WriteFloat(i % 2 ? 12.5f : 0.f);
	reply.Depth.assign(depths.Bytes().begin(), depths.Bytes().end());
	reply.DepthHash = Hasher::Of(reply.Depth);
	std::string error;
	std::vector<std::byte> wire;
	REQUIRE(EncodePortalImageReply(reply, wire, error));
	CHECK(wire.size() <= MAX_PORTAL_EXCHANGE_BYTES);
	const auto match = MatchPortalImageReply(wire, reply.Key, extent, extent, reply.Scope);
	REQUIRE(match);
	CHECK(match->DepthBytes == reply.Depth.size());
	PortalImageReply decoded;
	REQUIRE(DecodePortalImageReply(wire, decoded, error));
	CHECK(decoded == reply);
	for (size_t length = 0; length < wire.size(); ++length) {
		CHECK_FALSE(DecodePortalImageReply(std::span(wire).first(length), decoded, error));
		CHECK(decoded == reply);
	}
	for (int invalid = 0; invalid < 6; ++invalid) {
		auto bad = reply;
		switch (invalid) {
		case 0:
			bad.Depth.pop_back();
			break;
		case 1:
			bad.DepthHash = {};
			break;
		case 2:
			bad.Depth.clear();
			break;
		case 3:
			bad.Depth[3] = std::byte{0x80};
			break; // Negative zero is not the clear sample.
		case 4:
			bad.Depth[2] = std::byte{0x80};
			bad.Depth[3] = std::byte{0x7f};
			break;
		case 5:
			bad.Depth[2] = std::byte{0xc0};
			bad.Depth[3] = std::byte{0x7f};
			break;
		}
		if (invalid >= 3) bad.DepthHash = Hasher::Of(bad.Depth);
		auto preserved = wire;
		CHECK_FALSE(EncodePortalImageReply(bad, preserved, error));
		CHECK(preserved == wire);
	}
	if (extent == 2) {
		// A valid digest must not make a NaN depth acceptable on the raw wire.
		auto malformed = wire;
		auto depth = reply.Depth;
		depth[2] = std::byte{0xc0};
		depth[3] = std::byte{0x7f};
		const auto hash = Hasher::Of(depth);
		const auto dataOffset = malformed.size() - depth.size();
		std::copy(depth.begin(), depth.end(), malformed.begin() + dataOffset);
		for (size_t i = 0; i < hash.Digest.size(); ++i)
			malformed[dataOffset - 4 - hash.Digest.size() + i] = std::byte{hash.Digest[i]};
		CHECK_FALSE(DecodePortalImageReply(malformed, decoded, error));
		CHECK(decoded == reply);
	}
	auto corrupt = wire;
	corrupt.back() ^= std::byte{1};
	CHECK_FALSE(DecodePortalImageReply(corrupt, decoded, error));
	CHECK(decoded == reply);
	corrupt = wire;
	corrupt.push_back(std::byte{});
	CHECK_FALSE(DecodePortalImageReply(corrupt, decoded, error));
	CHECK(decoded == reply);
}

TEST_CASE(
	"portal layer sets preserve one bounded capture atomically", "[render][portal-exchange][portal-layers]"
) {
	PortalImageLayerSet layers;
	layers.Opaque = Reply();
	layers.Opaque.Scope = PortalImageScope::OpaqueLighting;
	engine::core::ByteWriter depth;
	depth.WriteFloat(3);
	depth.WriteFloat(4);
	layers.Opaque.Depth.assign(depth.Bytes().begin(), depth.Bytes().end());
	layers.Opaque.DepthHash = Hasher::Of(layers.Opaque.Depth);
	const auto count = GENERATE(0, 1, 2);
	layers.Transparent.assign(count, layers.Opaque);
	const bool overlay = GENERATE(false, true);
	if (overlay) {
		layers.SpatialOverlay = layers.Opaque;
		layers.SpatialOverlay->Depth.clear();
		layers.SpatialOverlay->DepthHash = {};
	}
	const auto lensMode = GENERATE(0, 1, 2);
	const bool lenses = lensMode != 0;
	const bool programs = lensMode == 2;
	if (lenses) {
		layers.Lenses.TimeSeconds = 12.5f;
		PortalCaptureLens first;
		first.Shader = "RoomLens";
		first.ProgramHash = Hasher::Of(layers.Opaque.Pixels);
		if (programs) {
			PortalCaptureLensProgram program;
			program.SpirV = {0x07230203, 0x00010000, 0, 1, 0};
			program.Hash = Hasher::Of(std::as_bytes(std::span(program.SpirV)));
			first.ProgramHash = program.Hash;
			layers.Lenses.Programs.push_back(std::move(program));
		}
		first.Position = {3, 2, 1};
		first.Spin = -2;
		first.Priority = -4;
		layers.Lenses.Entries.push_back(first);
		first.Shader = "EarlierName";
		first.Orientation = {0, 1, 0, 0};
		first.Strength = 0;
		layers.Lenses.Entries.push_back(first);
	}
	const auto skipLenses = [](engine::core::ByteReader &reader) {
		const auto count = reader.ReadUInt8();
		reader.ReadFloat();
		const auto programs = reader.ReadUInt8();
		for (size_t index = 0; index < programs; ++index) {
			reader.ReadRawView(32);
			const auto words = reader.ReadUInt32();
			reader.ReadRawView(size_t(words) * 4);
		}
		for (size_t index = 0; index < count; ++index) {
			reader.ReadRawView(7 * 4);
			reader.ReadString();
			reader.ReadRawView(32 + 6 * 4 + 1);
		}
		REQUIRE_FALSE(reader.Failed());
	};
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(ValidPortalImageLayerSet(layers));
	REQUIRE(EncodePortalImageLayerSet(layers, wire, error));
	PortalImageLayerSet decoded;
	REQUIRE(DecodePortalImageLayerSet(wire, decoded, error));
	CHECK(decoded == layers);
	CHECK(error.empty());
	PortalImageReply flat;
	CHECK_FALSE(DecodePortalImageReply(wire, flat, error));
	for (size_t size = 0; size < wire.size(); ++size) {
		CHECK_FALSE(DecodePortalImageLayerSet(std::span(wire).first(size), decoded, error));
		CHECK(decoded == layers);
	}
	auto extra = wire;
	extra.push_back(std::byte{});
	CHECK_FALSE(DecodePortalImageLayerSet(extra, decoded, error));
	CHECK(decoded == layers);
	for (const int invalid : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}) {
		auto bad = layers;
		bad.Transparent.assign(1, bad.Opaque);
		auto &layer = bad.Transparent.front();
		switch (invalid) {
		case 0:
			layer.Key.CameraRevision++;
			break;
		case 1:
			layer.CaptureTick++;
			break;
		case 2:
			layer.LightingRevision++;
			break;
		case 3:
			layer.ContentRevision++;
			break;
		case 4:
			layer.CaptureLighting.emplace();
			break;
		case 5:
			layer.Scope = PortalImageScope::CompleteWorld;
			break;
		case 6:
			bad.Transparent.resize(3, layer);
			break;
		case 7:
			layer.Depth.clear();
			break;
		case 8:
			layer.Pixels[7] = std::byte{0x40};
			layer.PixelHash = Hasher::Of(layer.Pixels);
			break;
		case 9:
			layer.Depth.assign(8, std::byte{});
			layer.DepthHash = Hasher::Of(layer.Depth);
			break;
		}
		auto preserved = wire;
		CHECK_FALSE(ValidPortalImageLayerSet(bad));
		CHECK_FALSE(EncodePortalImageLayerSet(bad, preserved, error));
		CHECK(preserved == wire);
	}
	if (count != 0 && !overlay) {
		// Re-encode an individually valid member with different capture metadata.
		// Its own digest is valid, so the group decoder must enforce the cohort.
		engine::core::ByteReader envelope(wire);
		envelope.ReadRawView(8);
		envelope.ReadUInt64();
		envelope.ReadString();
		envelope.ReadRawView(8 + 8 + 4 + 4 + 2);
		skipLenses(envelope);
		for (int member = 0; member < count; ++member) {
			const auto length = envelope.ReadUInt32();
			envelope.ReadRawView(length);
		}
		const auto prefix = std::span(wire).first(wire.size() - envelope.Remaining());
		for (const int changed : {0, 1, 2, 3, 4}) {
			auto foreign = layers.Transparent.back();
			if (changed == 0) foreign.CaptureTick++;
			if (changed == 1) foreign.ContentRevision++;
			if (changed == 2) foreign.LightingRevision++;
			if (changed == 3) foreign.CaptureLighting.emplace();
			if (changed == 4) foreign.Key.CameraRevision++;
			std::vector<std::byte> member;
			REQUIRE(EncodePortalImageReply(foreign, member, error));
			engine::core::ByteWriter malformed;
			malformed.WriteRaw(prefix.data(), prefix.size());
			malformed.WriteUInt32(static_cast<uint32_t>(member.size()));
			malformed.WriteRaw(member.data(), member.size());
			CHECK_FALSE(DecodePortalImageLayerSet(malformed.Bytes(), decoded, error));
			CHECK(decoded == layers);
		}
	}
	if (overlay) {
		engine::core::ByteReader envelope(wire);
		envelope.ReadRawView(8);
		envelope.ReadUInt64();
		envelope.ReadString();
		envelope.ReadRawView(8 + 8 + 4 + 4 + 2);
		skipLenses(envelope);
		for (int member = 0; member <= count; ++member) {
			const auto length = envelope.ReadUInt32();
			envelope.ReadRawView(length);
		}
		const auto prefix = std::span(wire).first(wire.size() - envelope.Remaining());
		for (int invalid = 0; invalid < 9; ++invalid) {
			auto bad = layers;
			auto &image = *bad.SpatialOverlay;
			if (invalid == 0) image.CaptureTick++;
			if (invalid == 1) image.Key.CameraRevision++;
			if (invalid == 2) image.ContentRevision++;
			if (invalid == 3) image.LightingRevision++;
			if (invalid == 4) image.CaptureLighting.emplace();
			if (invalid == 5) image.Depth = layers.Opaque.Depth;
			if (invalid == 6) image.Pixels[7] = std::byte{0x40};
			if (invalid == 7) {
				image.Pixels[6] = image.Pixels[7] = std::byte{};
				image.Pixels[0] = std::byte{1};
			}
			if (invalid == 8) image.Pixels.pop_back();
			image.PixelHash = Hasher::Of(image.Pixels);
			auto preserved = wire;
			CHECK_FALSE(ValidPortalImageLayerSet(bad));
			CHECK_FALSE(EncodePortalImageLayerSet(bad, preserved, error));
			CHECK(preserved == wire);
			if (invalid == 8) continue;
			if (!image.Depth.empty()) image.DepthHash = Hasher::Of(image.Depth);
			std::vector<std::byte> member;
			REQUIRE(EncodePortalImageReply(image, member, error));
			engine::core::ByteWriter malformed;
			malformed.WriteRaw(prefix.data(), prefix.size());
			malformed.WriteUInt32(static_cast<uint32_t>(member.size()));
			malformed.WriteRaw(member.data(), member.size());
			CHECK_FALSE(DecodePortalImageLayerSet(malformed.Bytes(), decoded, error));
			CHECK(decoded == layers);
		}
	}
	if (lenses) {
		for (int invalid = 0; invalid < 17; ++invalid) {
			auto bad = layers;
			auto &lens = bad.Lenses.Entries.front();
			if (invalid == 0) lens.Shader.clear();
			if (invalid == 1) lens.Shader.assign(257, 'x');
			if (invalid == 2) lens.Shader = std::string("bad\0name", 8);
			if (invalid == 3) lens.ProgramHash = {};
			if (invalid == 4) lens.Position[1] = std::numeric_limits<float>::infinity();
			if (invalid == 5) lens.Orientation = {};
			if (invalid == 6) lens.Radius = 0;
			if (invalid == 7) lens.InnerRadius = -1;
			if (invalid == 8) lens.InnerRadius = lens.Radius + 1;
			if (invalid == 9) lens.Falloff = 1.1f;
			if (invalid == 10) lens.Strength = -1;
			if (invalid == 11) lens.Spin = std::numeric_limits<float>::quiet_NaN();
			if (invalid == 12) lens.Shape = 1;
			if (invalid == 13) bad.Lenses.TimeSeconds = std::numeric_limits<float>::infinity();
			if (invalid == 14) bad.Lenses.Entries.resize(MAX_PORTAL_CAPTURE_LENSES + 1, lens);
			if (invalid == 15) bad.Lenses.Entries.clear();
			if (invalid == 16) lens.Falloff = -1;
			auto preserved = wire;
			CHECK_FALSE(ValidPortalCaptureLenses(bad.Lenses));
			CHECK_FALSE(ValidPortalImageLayerSet(bad));
			CHECK_FALSE(EncodePortalImageLayerSet(bad, preserved, error));
			CHECK(preserved == wire);
		}
		const size_t metadataOffset = 8 + 8 + 4 + layers.Opaque.Key.PortalKey.size() + 8 + 8 + 4 + 4 + 2;
		const size_t lensOffset = metadataOffset + 6 + (programs ? 32 + 4 + 5 * 4 : 0);
		const size_t hashOffset = lensOffset + 7 * 4 + 4 + layers.Lenses.Entries.front().Shader.size();
		for (int invalid = 0; invalid < 12; ++invalid) {
			auto malformed = wire;
			engine::core::ByteWriter replacement;
			size_t offset = 0;
			if (invalid == 0) {
				offset = metadataOffset;
				replacement.WriteUInt8(MAX_PORTAL_CAPTURE_LENSES + 1);
			}
			if (invalid == 1) {
				offset = metadataOffset + 1;
				replacement.WriteFloat(std::numeric_limits<float>::quiet_NaN());
			}
			if (invalid == 2) {
				offset = lensOffset;
				replacement.WriteFloat(std::numeric_limits<float>::infinity());
			}
			if (invalid == 3) {
				offset = lensOffset + 6 * 4;
				replacement.WriteFloat(0);
			}
			if (invalid == 4) {
				offset = lensOffset + 7 * 4;
				replacement.WriteUInt32(257);
			}
			if (invalid == 5) {
				offset = hashOffset;
				for (int index = 0; index < 32; ++index)
					replacement.WriteUInt8(0);
			}
			if (invalid >= 6 && invalid <= 10) {
				offset = hashOffset + 32 + (invalid - 6) * 4;
				const std::array<float, 5> values{0, 17, 2, -1, std::numeric_limits<float>::infinity()};
				replacement.WriteFloat(values[invalid - 6]);
			}
			if (invalid == 11) {
				offset = hashOffset + 32 + 6 * 4;
				replacement.WriteUInt8(1);
			}
			std::copy(replacement.Bytes().begin(), replacement.Bytes().end(), malformed.begin() + offset);
			CHECK_FALSE(DecodePortalImageLayerSet(malformed, decoded, error));
			CHECK(decoded == layers);
			if (count == 2)
				CHECK_FALSE(MatchPortalImageLayerSet(
					malformed, layers.Opaque.Key, layers.Opaque.Width, layers.Opaque.Height
				));
		}

		if (programs) {
			for (int invalid = 0; invalid < 10; ++invalid) {
				auto bad = layers;
				auto &program = bad.Lenses.Programs.front();
				if (invalid == 0) program.Hash = {};
				if (invalid == 1) {
					program.SpirV[0] = 0;
					program.Hash = Hasher::Of(std::as_bytes(std::span(program.SpirV)));
					for (auto &lens : bad.Lenses.Entries)
						lens.ProgramHash = program.Hash;
				}
				if (invalid == 2) {
					program.SpirV.resize(4);
					program.Hash = Hasher::Of(std::as_bytes(std::span(program.SpirV)));
					for (auto &lens : bad.Lenses.Entries)
						lens.ProgramHash = program.Hash;
				}
				if (invalid == 3) program.SpirV[2]++;
				if (invalid == 4) bad.Lenses.Programs.push_back(program);
				if (invalid == 5) {
					auto unused = program;
					unused.SpirV[2]++;
					unused.Hash = Hasher::Of(std::as_bytes(std::span(unused.SpirV)));
					bad.Lenses.Programs.push_back(std::move(unused));
				}
				if (invalid == 6) bad.Lenses.Entries.front().ProgramHash = layers.Opaque.PixelHash;
				if (invalid == 7) program.SpirV.resize(MAX_PORTAL_CAPTURE_LENS_PROGRAM_BYTES / 4 + 1);
				if (invalid == 8) bad.Lenses.Programs.resize(MAX_PORTAL_CAPTURE_LENSES + 1, program);
				if (invalid == 9) {
					bad.Lenses.Entries.clear();
					bad.Lenses.TimeSeconds = 0;
				}
				auto preserved = wire;
				CHECK_FALSE(ValidPortalCaptureLenses(bad.Lenses));
				CHECK_FALSE(EncodePortalImageLayerSet(bad, preserved, error));
				CHECK(preserved == wire);
			}
			const size_t programOffset = metadataOffset + 6;
			for (int invalid = 0; invalid < 8; ++invalid) {
				auto malformed = wire;
				if (invalid == 0) malformed[metadataOffset + 5] = std::byte{17};
				if (invalid == 1) std::fill_n(malformed.begin() + programOffset + 32, 4, std::byte{255});
				if (invalid == 2) malformed[programOffset + 32] = std::byte{4};
				if (invalid == 3) malformed[programOffset + 36] ^= std::byte{1};
				if (invalid == 4) malformed[programOffset] ^= std::byte{1};
				if (invalid == 5) malformed[hashOffset] ^= std::byte{1};
				if (invalid == 6) {
					malformed[metadataOffset + 5] = std::byte{2};
					malformed.insert(
						malformed.begin() + lensOffset,
						wire.begin() + programOffset,
						wire.begin() + lensOffset
					);
				}
				if (invalid == 7) {
					auto unused = layers.Lenses.Programs.front();
					unused.SpirV[2]++;
					unused.Hash = Hasher::Of(std::as_bytes(std::span(unused.SpirV)));
					engine::core::ByteWriter dictionary;
					dictionary.WriteRaw(unused.Hash.Digest.data(), unused.Hash.Digest.size());
					dictionary.WriteUInt32(static_cast<uint32_t>(unused.SpirV.size()));
					for (const auto word : unused.SpirV)
						dictionary.WriteUInt32(word);
					malformed[metadataOffset + 5] = std::byte{2};
					malformed.insert(
						malformed.begin() + lensOffset, dictionary.Bytes().begin(), dictionary.Bytes().end()
					);
				}
				CHECK_FALSE(DecodePortalImageLayerSet(malformed, decoded, error));
				CHECK(decoded == layers);
				if (count == 2)
					CHECK_FALSE(MatchPortalImageLayerSet(
						malformed, layers.Opaque.Key, layers.Opaque.Width, layers.Opaque.Height
					));
			}
			if (count == 2 && !overlay) {
				auto bounded = layers;
				auto &program = bounded.Lenses.Programs.front();
				program.SpirV.resize(MAX_PORTAL_CAPTURE_LENS_PROGRAM_BYTES / 4);
				program.Hash = Hasher::Of(std::as_bytes(std::span(program.SpirV)));
				for (auto &lens : bounded.Lenses.Entries)
					lens.ProgramHash = program.Hash;
				std::vector<std::byte> boundedWire;
				REQUIRE(EncodePortalImageLayerSet(bounded, boundedWire, error));
				PortalImageLayerSet boundedDecoded;
				REQUIRE(DecodePortalImageLayerSet(boundedWire, boundedDecoded, error));
				CHECK(boundedDecoded == bounded);
				const auto match = MatchPortalImageLayerSet(
					boundedWire, bounded.Opaque.Key, bounded.Opaque.Width, bounded.Opaque.Height
				);
				REQUIRE(match);
				CHECK(match->MetadataBytes == PortalCaptureLensBytes(bounded.Lenses));
				// Valid individual programs can still exceed the aggregate code allowance.
				auto overWire = boundedWire;
				overWire[metadataOffset + 5] = std::byte{2};
				overWire.insert(
					overWire.begin() + programOffset + 36 + MAX_PORTAL_CAPTURE_LENS_PROGRAM_BYTES,
					wire.begin() + programOffset,
					wire.begin() + lensOffset
				);
				CHECK_FALSE(MatchPortalImageLayerSet(
					overWire, bounded.Opaque.Key, bounded.Opaque.Width, bounded.Opaque.Height
				));
				CHECK_FALSE(DecodePortalImageLayerSet(overWire, boundedDecoded, error));
				CHECK(boundedDecoded == bounded);
				bounded.Lenses.Programs.push_back(layers.Lenses.Programs.front());
				bounded.Lenses.Entries.back().ProgramHash = bounded.Lenses.Programs.back().Hash;
				CHECK_FALSE(ValidPortalCaptureLenses(bounded.Lenses));
				CHECK_FALSE(EncodePortalImageLayerSet(bounded, boundedWire, error));
				auto dictionary = layers;
				dictionary.Lenses.Programs.clear();
				dictionary.Lenses.Entries.clear();
				for (size_t index = 0; index < MAX_PORTAL_CAPTURE_LENSES; ++index) {
					auto program = layers.Lenses.Programs.front();
					program.SpirV[2] = static_cast<uint32_t>(index);
					program.Hash = Hasher::Of(std::as_bytes(std::span(program.SpirV)));
					auto lens = layers.Lenses.Entries.front();
					lens.ProgramHash = program.Hash;
					dictionary.Lenses.Entries.push_back(std::move(lens));
					dictionary.Lenses.Programs.push_back(std::move(program));
				}
				REQUIRE(EncodePortalImageLayerSet(dictionary, boundedWire, error));
				REQUIRE(DecodePortalImageLayerSet(boundedWire, boundedDecoded, error));
				CHECK(boundedDecoded == dictionary);
			}
		}
		auto maximum = layers;
		maximum.Lenses.Entries.resize(MAX_PORTAL_CAPTURE_LENSES, layers.Lenses.Entries.front());
		std::vector<std::byte> maximumWire;
		REQUIRE(EncodePortalImageLayerSet(maximum, maximumWire, error));
		PortalImageLayerSet maximumDecoded;
		REQUIRE(DecodePortalImageLayerSet(maximumWire, maximumDecoded, error));
		CHECK(maximumDecoded == maximum);
		if (count == 2) {
			const auto match = MatchPortalImageLayerSet(
				maximumWire, layers.Opaque.Key, layers.Opaque.Width, layers.Opaque.Height
			);
			REQUIRE(match);
			CHECK(match->MetadataBytes == PortalCaptureLensBytes(maximum.Lenses));
		}
	}
	// Deterministic byte mutations also exercise malformed nested prefixes and digests.
	for (size_t index = 0; index < wire.size(); ++index) {
		auto mutated = wire;
		mutated[index] ^= std::byte{0x80};
		PortalImageLayerSet parsed = layers;
		if (!DecodePortalImageLayerSet(mutated, parsed, error)) {
			CHECK(parsed == layers);
		} else {
			std::vector<std::byte> again;
			REQUIRE(EncodePortalImageLayerSet(parsed, again, error));
			PortalImageLayerSet roundtrip;
			REQUIRE(DecodePortalImageLayerSet(again, roundtrip, error));
			CHECK(roundtrip == parsed);
		}
	}
}

TEST_CASE(
	"portal layer compression cannot bypass the expanded pixel budget",
	"[render][portal-exchange][portal-layers]"
) {
	const auto transparent = GENERATE(0u, 1u, 2u);
	const bool overlay = GENERATE(false, true);
	const std::array<uint32_t, 4> extents{512, 362, 295, 256};
	const uint32_t extent = extents[transparent + overlay];
	PortalImageLayerSet layers;
	layers.Opaque = Reply();
	layers.Opaque.Scope = PortalImageScope::OpaqueLighting;
	layers.Opaque.Width = layers.Opaque.Height = extent;
	layers.Opaque.RowStride = extent * 8;
	layers.Opaque.Pixels.assign(size_t(extent) * extent * 8, std::byte{});
	engine::core::ByteWriter depth;
	for (size_t pixel = 0; pixel < size_t(extent) * extent; ++pixel) {
		layers.Opaque.Pixels[pixel * 8 + 7] = std::byte{0x3c};
		depth.WriteFloat(1);
	}
	layers.Opaque.Depth.assign(depth.Bytes().begin(), depth.Bytes().end());
	layers.Opaque.PixelHash = Hasher::Of(layers.Opaque.Pixels);
	layers.Opaque.DepthHash = Hasher::Of(layers.Opaque.Depth);
	layers.Transparent.assign(transparent, layers.Opaque);
	if (overlay) {
		layers.SpatialOverlay = layers.Opaque;
		layers.SpatialOverlay->Depth.clear();
		layers.SpatialOverlay->DepthHash = {};
	}
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageLayerSet(layers, wire, error));
	CHECK(wire.size() < layers.Opaque.Pixels.size());
	PortalImageLayerSet decoded;
	REQUIRE(DecodePortalImageLayerSet(wire, decoded, error));
	CHECK(decoded == layers);
	const auto preserved = wire;
	const auto enlarge = [](PortalImageReply &image) {
		const bool paired = !image.Depth.empty();
		++image.Width;
		++image.Height;
		image.RowStride = image.Width * 8;
		image.Pixels.assign(size_t(image.Width) * image.Height * 8, std::byte{});
		image.PixelHash = Hasher::Of(image.Pixels);
		if (paired) {
			image.Depth.assign(size_t(image.Width) * image.Height * 4, std::byte{});
			image.DepthHash = Hasher::Of(image.Depth);
		}
	};
	enlarge(layers.Opaque);
	for (auto &layer : layers.Transparent)
		enlarge(layer);
	if (layers.SpatialOverlay) enlarge(*layers.SpatialOverlay);
	CHECK_FALSE(ValidPortalImageLayerSet(layers));
	CHECK_FALSE(EncodePortalImageLayerSet(layers, wire, error));
	CHECK(wire == preserved);
	// The outer dimensions gate total expansion before any nested decode.
	const size_t widthOffset = 8 + 8 + 4 + layers.Opaque.Key.PortalKey.size() + 8 + 8;
	wire[widthOffset] = std::byte{0xff};
	wire[widthOffset + 1] = std::byte{0xff};
	CHECK_FALSE(DecodePortalImageLayerSet(wire, decoded, error));
	CHECK(decoded.Opaque.Width == extent);
}

TEST_CASE("ordered portal requests charge the overflow capture", "[render][portal-exchange][portal-layers]") {
	auto request = Request();
	request.Scope = PortalImageScope::OpaqueLighting;
	request.OrderedLayers = true;
	request.RecursionDepth = 0;
	request.Width = request.Height = 256;
	request.PixelBudget = MAX_PORTAL_IMAGE_PIXELS;
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageRequest(request, wire, error));
	PortalImageRequest decoded;
	REQUIRE(DecodePortalImageRequest(wire, decoded, error));
	CHECK(decoded == request);
	for (int invalid = 0; invalid < 5; ++invalid) {
		auto bad = request;
		if (invalid == 0) --bad.PixelBudget;
		if (invalid == 1) ++bad.Width;
		if (invalid == 2) bad.Scope = PortalImageScope::CompleteWorld;
		if (invalid == 3) bad.RecursionDepth = MAX_PORTAL_CAPTURE_TREE_DEPTH + 1;
		if (invalid == 4) bad.KnownImage = PortalImageVersion{1, 2};
		auto output = wire;
		CHECK_FALSE(EncodePortalImageRequest(bad, output, error));
		CHECK(output == wire);
	}
	const size_t profileByte = 38 + request.Key.PortalKey.size();
	REQUIRE(wire[profileByte] == std::byte{1});
	for (const auto value : {2, 255}) {
		auto bad = wire;
		bad[profileByte] = std::byte(value);
		CHECK_FALSE(DecodePortalImageRequest(bad, decoded, error));
		CHECK(decoded == request);
	}
	for (size_t size = 0; size < wire.size(); ++size) {
		CHECK_FALSE(DecodePortalImageRequest(std::span(wire).first(size), decoded, error));
		CHECK(decoded == request);
	}
	auto old = wire;
	old[4] = std::byte{11};
	CHECK_FALSE(DecodePortalImageRequest(old, decoded, error));
	CHECK(decoded == request);
}

TEST_CASE(
	"ordered portal requests admit bounded recursive capture trees",
	"[render][portal-exchange][portal-layers]"
) {
	auto request = Request();
	request.Scope = PortalImageScope::OpaqueLighting;
	request.OrderedLayers = true;
	request.RecursionDepth = MAX_PORTAL_CAPTURE_TREE_DEPTH;
	request.PixelBudget = MAX_PORTAL_IMAGE_PIXELS;
	std::string error;
	std::vector<std::byte> wire;
	REQUIRE(EncodePortalImageRequest(request, wire, error));
	PortalImageRequest decoded;
	REQUIRE(DecodePortalImageRequest(wire, decoded, error));
	CHECK(decoded == request);
}

TEST_CASE(
	"portal ambient planes round trip and reject malformed samples transactionally",
	"[render][portal-exchange]"
) {
	auto reply = Reply();
	reply.Scope = PortalImageScope::OpaqueLighting;
	reply.CaptureLighting.emplace();
	const bool compressed = GENERATE(false, true);
	const bool directional = GENERATE(false, true);
	if (compressed) {
		reply.Width = reply.Height = 32;
		reply.RowStride = reply.Width * 8;
		reply.Pixels.assign(size_t(reply.RowStride) * reply.Height, std::byte{});
		reply.PixelHash = Hasher::Of(reply.Pixels);
	}
	const size_t pixels = size_t(reply.Width) * reply.Height;
	reply.Depth.assign(pixels * 4, std::byte{});
	reply.DepthHash = Hasher::Of(reply.Depth);
	reply.Normal.assign(pixels * 4, std::byte{255});
	reply.NormalHash = Hasher::Of(reply.Normal);
	engine::core::ByteWriter response;
	for (size_t i = 0; i < pixels; ++i) {
		response.WriteFloat(0.25f);
		response.WriteFloat(1);
		response.WriteFloat(2);
		response.WriteFloat(0.5f);
	}
	reply.AmbientResponse.assign(response.Bytes().begin(), response.Bytes().end());
	reply.AmbientResponseHash = Hasher::Of(reply.AmbientResponse);
	reply.LightingBaseline = reply.AmbientResponse;
	reply.LightingBaselineHash = Hasher::Of(reply.LightingBaseline);
	if (directional) {
		reply.DirectionalResponse = reply.AmbientResponse;
		reply.DirectionalResponseHash = Hasher::Of(reply.DirectionalResponse);
	}
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageReply(reply, wire, error));
	if (compressed) CHECK(wire.size() < reply.AmbientResponse.size());
	const auto measured = MatchPortalImageReply(wire, reply.Key, reply.Width, reply.Height, reply.Scope);
	REQUIRE(measured);
	CHECK(measured->AmbientBytes == pixels * (directional ? 52 : 36));
	PortalImageReply decoded;
	REQUIRE(DecodePortalImageReply(wire, decoded, error));
	CHECK(decoded == reply);
	for (size_t size = 0; size < wire.size(); ++size) {
		CHECK_FALSE(DecodePortalImageReply(std::span(wire).first(size), decoded, error));
		CHECK(decoded == reply);
	}
	auto corrupt = wire;
	corrupt.back() ^= std::byte{1};
	CHECK_FALSE(DecodePortalImageReply(corrupt, decoded, error));
	CHECK(decoded == reply);
	if (!compressed) {
		// Rehash invalid raw samples so content validation, not the digest, must refuse them.
		for (const auto &[component, value] : std::array<std::pair<size_t, float>, 3>{
				 {{0, std::numeric_limits<float>::quiet_NaN()}, {1, -1}, {3, 1.01f}}
			 }) {
			corrupt = wire;
			engine::core::ByteWriter sample;
			sample.WriteFloat(value);
			const auto start = corrupt.size() - (directional ? 36 + reply.DirectionalResponse.size() : 0) -
							   reply.LightingBaseline.size() - 36 - reply.AmbientResponse.size();
			std::copy(sample.Bytes().begin(), sample.Bytes().end(), corrupt.begin() + start + component * 4);
			const auto hash = Hasher::Of(std::span(corrupt).subspan(start, reply.AmbientResponse.size()));
			std::transform(
				hash.Digest.begin(), hash.Digest.end(), corrupt.begin() + start - 36, [](uint8_t value) {
					return std::byte{value};
				}
			);
			CHECK_FALSE(DecodePortalImageReply(corrupt, decoded, error));
			CHECK(decoded == reply);
		}
	}
	if (!compressed) {
		corrupt = wire;
		engine::core::ByteWriter sample;
		sample.WriteFloat(std::numeric_limits<float>::quiet_NaN());
		const auto start = corrupt.size() - (directional ? 36 + reply.DirectionalResponse.size() : 0) -
						   reply.LightingBaseline.size();
		std::copy(sample.Bytes().begin(), sample.Bytes().end(), corrupt.begin() + start);
		const auto hash = Hasher::Of(std::span(corrupt).subspan(start, reply.LightingBaseline.size()));
		std::transform(
			hash.Digest.begin(), hash.Digest.end(), corrupt.begin() + start - 36, [](uint8_t value) {
				return std::byte{value};
			}
		);
		CHECK_FALSE(DecodePortalImageReply(corrupt, decoded, error));
		CHECK(decoded == reply);
	}
	for (int fault = 0; fault < 9; ++fault) {
		auto invalid = reply;
		switch (fault) {
		case 0:
			invalid.Normal.clear();
			invalid.NormalHash = {};
			break;
		case 1:
			invalid.AmbientResponse.clear();
			invalid.AmbientResponseHash = {};
			break;
		case 2:
			invalid.Depth.clear();
			invalid.DepthHash = {};
			break;
		case 3:
			invalid.CaptureLighting.reset();
			break;
		case 4:
			invalid.Scope = PortalImageScope::CompleteWorld;
			break;
		case 5:
			invalid = {};
			invalid.Key = reply.Key;
			invalid.Status = PortalImageStatus::Failed;
			invalid.Diagnostic = "capture failed";
			invalid.Normal = reply.Normal;
			invalid.NormalHash = reply.NormalHash;
			invalid.AmbientResponse = reply.AmbientResponse;
			invalid.AmbientResponseHash = reply.AmbientResponseHash;
			break;
		case 6:
			invalid.Normal.pop_back();
			invalid.NormalHash = Hasher::Of(invalid.Normal);
			break;
		case 8:
			invalid.LightingBaseline.clear();
			invalid.LightingBaselineHash = {};
			break;
		case 7:
			invalid.LightingBaseline.clear();
			invalid.Normal.clear();
			invalid.AmbientResponse.clear();
			break;
		}
		auto unchanged = wire;
		CHECK_FALSE(EncodePortalImageReply(invalid, unchanged, error));
		CHECK(unchanged == wire);
	}
}

TEST_CASE(
	"directional response rejects malformed hash-valid samples and flags", "[render][portal-exchange]"
) {
	auto reply = Reply();
	reply.Scope = PortalImageScope::OpaqueLighting;
	reply.CaptureLighting.emplace();
	const size_t pixels = size_t(reply.Width) * reply.Height;
	reply.Depth.assign(pixels * 4, std::byte{});
	reply.DepthHash = Hasher::Of(reply.Depth);
	reply.Normal.assign(pixels * 4, std::byte{});
	reply.NormalHash = Hasher::Of(reply.Normal);
	reply.AmbientResponse.assign(pixels * 16, std::byte{});
	reply.AmbientResponseHash = Hasher::Of(reply.AmbientResponse);
	reply.LightingBaseline = reply.AmbientResponse;
	reply.LightingBaselineHash = Hasher::Of(reply.LightingBaseline);
	reply.DirectionalResponse = reply.AmbientResponse;
	engine::core::ByteWriter negativeZero;
	negativeZero.WriteFloat(-0.f);
	std::copy(negativeZero.Bytes().begin(), negativeZero.Bytes().end(), reply.DirectionalResponse.begin());
	reply.DirectionalResponseHash = Hasher::Of(reply.DirectionalResponse);
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageReply(reply, wire, error));
	PortalImageReply decoded;
	REQUIRE(DecodePortalImageReply(wire, decoded, error));
	CHECK(decoded == reply);
	for (const auto &[component, value] : std::array<std::pair<size_t, float>, 4>{
			 {{0, std::numeric_limits<float>::quiet_NaN()},
			  {1, std::numeric_limits<float>::infinity()},
			  {2, -1.f},
			  {3, 1.01f}}
		 }) {
		auto corrupt = wire;
		engine::core::ByteWriter sample;
		sample.WriteFloat(value);
		const auto start = corrupt.size() - reply.DirectionalResponse.size();
		std::copy(sample.Bytes().begin(), sample.Bytes().end(), corrupt.begin() + start + component * 4);
		const auto hash = Hasher::Of(std::span(corrupt).subspan(start));
		std::transform(
			hash.Digest.begin(), hash.Digest.end(), corrupt.begin() + start - 36, [](uint8_t byte) {
				return std::byte{byte};
			}
		);
		CHECK_FALSE(DecodePortalImageReply(corrupt, decoded, error));
		CHECK(decoded == reply);
	}
	PortalImageLayerSet layers;
	layers.Opaque = reply;
	auto transparent = reply;
	transparent.Pixels.assign(transparent.Pixels.size(), std::byte{});
	transparent.PixelHash = Hasher::Of(transparent.Pixels);
	transparent.DirectionalResponse.clear();
	transparent.DirectionalResponseHash = {};
	layers.Transparent.push_back(transparent);
	REQUIRE(ValidPortalImageLayerSet(layers));
	layers.Transparent[0].DirectionalResponse = reply.DirectionalResponse;
	layers.Transparent[0].DirectionalResponseHash = reply.DirectionalResponseHash;
	CHECK_FALSE(ValidPortalImageLayerSet(layers));
	std::vector<std::byte> layerWire;
	CHECK_FALSE(EncodePortalImageLayerSet(layers, layerWire, error));
	const size_t encodingOffset = 8 + 8 + 4 + reply.Key.PortalKey.size() + 8 + 8 + 2;
	for (const uint16_t flags : {uint16_t(512 | 26), uint16_t(256 | 10), uint16_t(1024 | 282)}) {
		auto corrupt = wire;
		corrupt[encodingOffset] = std::byte(flags & 255);
		corrupt[encodingOffset + 1] = std::byte(flags >> 8);
		CHECK_FALSE(MatchPortalImageReply(corrupt, reply.Key, reply.Width, reply.Height, reply.Scope));
		CHECK_FALSE(DecodePortalImageReply(corrupt, decoded, error));
		CHECK(decoded == reply);
	}
	for (int fault = 0; fault < 4; ++fault) {
		auto invalid = reply;
		if (fault == 0) invalid.DirectionalResponse.pop_back();
		if (fault == 1) invalid.DirectionalResponse.clear();
		if (fault == 2) invalid.DirectionalResponseHash = {};
		if (fault == 3) {
			invalid.Normal.clear();
			invalid.AmbientResponse.clear();
			invalid.LightingBaseline.clear();
		}
		auto unchanged = wire;
		CHECK_FALSE(EncodePortalImageReply(invalid, unchanged, error));
		CHECK(unchanged == wire);
	}
}

TEST_CASE("directional compressed payload preserves sample and wire limits", "[render][portal-exchange]") {
	auto reply = Reply();
	reply.Scope = PortalImageScope::OpaqueLighting;
	reply.CaptureLighting.emplace();
	reply.Width = reply.Height = 256;
	reply.RowStride = reply.Width * 8;
	const size_t pixels = size_t(reply.Width) * reply.Height;
	reply.Pixels.assign(pixels * 8, std::byte{});
	reply.Depth.assign(pixels * 4, std::byte{});
	reply.Normal.assign(pixels * 4, std::byte{});
	reply.AmbientResponse.assign(pixels * 16, std::byte{});
	reply.LightingBaseline.assign(pixels * 16, std::byte{});
	reply.DirectionalResponse.assign(pixels * 16, std::byte{});
	reply.PixelHash = Hasher::Of(reply.Pixels);
	reply.DepthHash = Hasher::Of(reply.Depth);
	reply.NormalHash = Hasher::Of(reply.Normal);
	reply.AmbientResponseHash = Hasher::Of(reply.AmbientResponse);
	reply.LightingBaselineHash = Hasher::Of(reply.LightingBaseline);
	reply.DirectionalResponseHash = Hasher::Of(reply.DirectionalResponse);
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageReply(reply, wire, error));
	CHECK(wire.size() < MAX_PORTAL_EXCHANGE_BYTES);
	PortalImageReply decoded;
	REQUIRE(DecodePortalImageReply(wire, decoded, error));
	CHECK(decoded == reply);
	const auto measured = MatchPortalImageReply(wire, reply.Key, reply.Width, reply.Height, reply.Scope);
	REQUIRE(measured);
	CHECK(measured->AmbientBytes == pixels * 52);

	// Default capture lighting has seventeen floats and an empty local-light list.
	const size_t encodingOffset = 8 + 8 + 4 + reply.Key.PortalKey.size() + 8 + 8 + 2;
	const size_t prefixBytes = encodingOffset + 2 + 4 + 3 * 8 + 3 * 4 + 17 * 4 + 1;
	engine::core::ByteWriter raw;
	raw.WriteRaw(wire.data(), prefixBytes);
	for (const auto &plane :
		 {reply.Pixels,
		  reply.Depth,
		  reply.Normal,
		  reply.AmbientResponse,
		  reply.LightingBaseline,
		  reply.DirectionalResponse}) {
		const auto hash = Hasher::Of(plane);
		raw.WriteRaw(hash.Digest.data(), hash.Digest.size());
		raw.WriteUInt32(static_cast<uint32_t>(plane.size()));
		raw.WriteRaw(plane.data(), plane.size());
	}
	std::vector<std::byte> rawWire(raw.Bytes().begin(), raw.Bytes().end());
	rawWire[encodingOffset] = std::byte{26};
	rawWire[encodingOffset + 1] = std::byte{1};
	REQUIRE(rawWire.size() > MAX_PORTAL_EXCHANGE_BYTES);
	CHECK_FALSE(DecodePortalImageReply(rawWire, decoded, error));
	CHECK_FALSE(MatchPortalImageReply(rawWire, reply.Key, reply.Width, reply.Height, reply.Scope));
	CHECK(decoded == reply);

	// Baseline permits negative finite values; response must refuse the same valid compressed frame.
	engine::core::ByteWriter negative;
	negative.WriteFloat(-1.f);
	std::copy(negative.Bytes().begin(), negative.Bytes().end(), reply.LightingBaseline.begin());
	reply.LightingBaselineHash = Hasher::Of(reply.LightingBaseline);
	REQUIRE(EncodePortalImageReply(reply, wire, error));
	engine::core::ByteReader payloads(wire);
	payloads.ReadRawView(prefixBytes);
	std::array<size_t, 7> offsets{};
	for (size_t plane = 0; plane < 6; ++plane) {
		offsets[plane] = payloads.Position();
		payloads.ReadRawView(32);
		const auto length = payloads.ReadUInt32();
		payloads.ReadRawView(length);
	}
	offsets[6] = payloads.Position();
	REQUIRE_FALSE(payloads.Failed());
	REQUIRE(payloads.AtEnd());
	REQUIRE((std::to_integer<unsigned>(wire[encodingOffset + 1]) & 2) != 0);
	std::vector<std::byte> corrupt(wire.begin(), wire.begin() + offsets[5]);
	corrupt.insert(corrupt.end(), wire.begin() + offsets[4], wire.begin() + offsets[5]);
	const auto unchanged = decoded;
	CHECK_FALSE(DecodePortalImageReply(corrupt, decoded, error));
	CHECK(decoded == unchanged);
}

TEST_CASE(
	"retained body exclusion is a distinct canonical ordered capture request",
	"[render][portal-exchange][retained-body]"
) {
	auto request = Request();
	request.Scope = PortalImageScope::OpaqueLighting;
	request.OrderedLayers = true;
	request.PixelBudget = request.Width * request.Height * 4;
	request.EyePlayer = "17";
	std::vector<std::byte> wire;
	std::string error;
	for (const std::string account : {"", "0", "91", "-9223372036854775808", "9223372036854775807"}) {
		request.RetainedBodyPlayer = account;
		REQUIRE(EncodePortalImageRequest(request, wire, error));
		PortalImageRequest decoded;
		REQUIRE(DecodePortalImageRequest(wire, decoded, error));
		CHECK(decoded == request);
		CHECK(decoded.EyePlayer == "17");
	}
	request.RetainedBodyPlayer = "91";
	REQUIRE(EncodePortalImageRequest(request, wire, error));
	for (size_t length = 0; length < wire.size(); ++length) {
		PortalImageRequest decoded = request;
		CHECK_FALSE(DecodePortalImageRequest(std::span(wire).first(length), decoded, error));
		CHECK(decoded == request);
	}
	for (const std::string account : {"01", "+1", "-0", "92x", "9223372036854775808"}) {
		request.RetainedBodyPlayer = account;
		CHECK_FALSE(EncodePortalImageRequest(request, wire, error));
	}
	request.RetainedBodyPlayer = "91";
	request.OrderedLayers = false;
	CHECK_FALSE(EncodePortalImageRequest(request, wire, error));
}
