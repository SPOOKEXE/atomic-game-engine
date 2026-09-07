#include <engine/core/Bytes.hpp>
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
	CHECK(bytes[4] == std::byte{12});
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
	if (count != 0) {
		// Re-encode an individually valid member with different capture metadata.
		// Its own digest is valid, so the group decoder must enforce the cohort.
		engine::core::ByteReader envelope(wire);
		envelope.ReadRawView(8);
		envelope.ReadUInt64();
		envelope.ReadString();
		envelope.ReadRawView(8 + 8 + 4 + 4 + 1);
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
	const uint32_t extent = transparent == 0 ? 512 : transparent == 1 ? 362 : 295;
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
	std::vector<std::byte> wire;
	std::string error;
	REQUIRE(EncodePortalImageLayerSet(layers, wire, error));
	CHECK(wire.size() < layers.Opaque.Pixels.size());
	PortalImageLayerSet decoded;
	REQUIRE(DecodePortalImageLayerSet(wire, decoded, error));
	CHECK(decoded == layers);
	const auto preserved = wire;
	layers.Opaque.Width++;
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
		if (invalid == 3) bad.RecursionDepth = 1;
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
