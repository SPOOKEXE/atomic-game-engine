#include <engine/core/Bytes.hpp>
#include <engine/game/Play.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {
	using engine::game::DecodeTeleportRequest;
	using engine::game::DecodeTeleportResult;
	using engine::game::EncodeTeleportRequest;
	using engine::game::EncodeTeleportResult;
	using engine::game::PlayMessage;
	using engine::game::TeleportRequest;
	using engine::game::TeleportRequestDecision;
	using engine::game::TeleportRequestResult;

	TEST_CASE("teleport request and result preserve their typed contract", "[game][play]") {
		TeleportRequest request;
		request.Id = 42;
		request.Place = "Arena";
		request.Data = {std::byte{0x01}, std::byte{0x02}};

		TeleportRequest decodedRequest;
		REQUIRE(DecodeTeleportRequest(EncodeTeleportRequest(request), decodedRequest));
		CHECK(decodedRequest.Id == request.Id);
		CHECK(decodedRequest.Place == request.Place);
		CHECK(decodedRequest.Data == request.Data);

		TeleportRequestResult result;
		result.Id = request.Id;
		result.Decision = TeleportRequestDecision::Denied;
		result.Message = "The arena is full";

		TeleportRequestResult decodedResult;
		REQUIRE(DecodeTeleportResult(EncodeTeleportResult(result), decodedResult));
		CHECK(decodedResult.Id == result.Id);
		CHECK(decodedResult.Decision == TeleportRequestDecision::Denied);
		CHECK(decodedResult.Message == result.Message);
	}

	TEST_CASE("teleport decoders refuse foreign and incomplete messages", "[game][play]") {
		const std::vector<std::byte> foreign{std::byte{0xff}};
		TeleportRequest request;
		TeleportRequestResult result;
		CHECK_FALSE(DecodeTeleportRequest(foreign, request));
		CHECK_FALSE(DecodeTeleportResult(foreign, result));
	}

	TEST_CASE("teleport codecs keep their bounded wire contract", "[game][play]") {
		TeleportRequest request;
		request.Id = 1;
		request.Place = "Arena";
		request.Data.resize(64u * 1024u + 1);
		CHECK(EncodeTeleportRequest(request).empty());

		request.Data.clear();
		request.Id = 0;
		CHECK(EncodeTeleportRequest(request).empty());

		TeleportRequestResult result;
		result.Id = 1;
		result.Message.resize(1025, 'x');
		CHECK(EncodeTeleportResult(result).empty());
		result.Message.clear();
		result.Decision = static_cast<TeleportRequestDecision>(3);
		CHECK(EncodeTeleportResult(result).empty());

		engine::core::ByteWriter oversized;
		oversized.WriteUInt8(static_cast<uint8_t>(PlayMessage::TeleportRequest));
		oversized.WriteUInt64(1);
		oversized.WriteString("Arena");
		oversized.WriteUInt32(64u * 1024u + 1);
		CHECK_FALSE(DecodeTeleportRequest(oversized.Bytes(), request));

		engine::core::ByteWriter tooLong;
		tooLong.WriteUInt8(static_cast<uint8_t>(PlayMessage::TeleportResult));
		tooLong.WriteUInt64(1);
		tooLong.WriteUInt8(static_cast<uint8_t>(TeleportRequestDecision::Denied));
		tooLong.WriteString(std::string(1025, 'x'));
		CHECK_FALSE(DecodeTeleportResult(tooLong.Bytes(), result));
	}
}
