#include "FrameObservationChannel.hpp"

#include "FrameObservation.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <client/Client.hpp>
#include <cstring>
#include <stdexcept>
#include <thread>

TEST_SUITE_ID("client.frame-observation-channel")
TEST_DEPENDS("client.frame-observation")

namespace {
	std::shared_ptr<client::FrameObservation> Sample(uint64_t number, bool retained = false) {
		auto frame = std::make_shared<client::FrameObservation>();
		frame->Generation = 41;
		frame->ClientFrame = number;
		frame->Pipeline = "native-channel-pipeline";
		auto &image = frame->Image;
		image.Token = 1000 + number;
		image.Frame = 100 + number;
		image.World = 71;
		image.WorldName = "native-world";
		image.SnapshotId = "native-snapshot";
		image.ViewSlot = 3;
		image.Pipeline = engine::core::Name(frame->Pipeline);
		image.PipelineRevision = 7;
		image.Status = engine::render::SceneCaptureStatus::Ready;
		image.Format = engine::render::ResourceImageFormat::RGBA8_UNorm;
		image.Width = image.Height = 64;
		image.RowStride = image.Width * 4;
		image.Pixels.resize(image.RowStride * image.Height, uint8_t(number));
		image.CameraFrame.Position = {1, 2, 3};
		image.Camera.NearPlane = .01f;
		image.Camera.RenderFeatures = {.Enable = 4, .Disable = 8};
		image.Projection[0][0] = 2;
		frame->Metadata = {
			{"frame", static_cast<int64_t>(number)},
			{"observation_generation", frame->Generation},
			{"view_world", image.WorldName},
			{"snapshot_id", image.SnapshotId},
			{"view_slot", image.ViewSlot},
			{"pipeline", {{"name", frame->Pipeline}, {"revision", image.PipelineRevision}}},
			{"delivered_textures", int64_t{2}},
		};
		if (retained) frame->Metadata["retained_world"] = "client.replica";
		// A deliberately conservative charge, as the native producer supplies.
		frame->OwnedBytes = sizeof(client::FrameObservation) + image.Pixels.size() + 16 * 1024;
		return frame;
	}

	client::FrameObservationChannelSummary Summary() {
		client::FrameObservationChannelSummary summary;
		summary.Run = 77;
		summary.Generation = 41;
		summary.CapturedFrames = 10;
		summary.Complete = true;
		summary.Latest = Sample(10);
		summary.Retained = Sample(7, true);
		return summary;
	}

	std::vector<std::byte>
	ReplaceMetadata(std::vector<std::byte> bytes, std::string_view from, std::string_view to) {
		REQUIRE(from.size() == to.size());
		std::string encoded(reinterpret_cast<const char *>(bytes.data()), bytes.size());
		const auto position = encoded.find(from);
		REQUIRE(position != std::string::npos);
		encoded.replace(position, from.size(), to);
		std::memcpy(bytes.data(), encoded.data(), bytes.size());
		return bytes;
	}
}

TEST_CASE(
	"native frame channel requests and acknowledgements bind the run and schema", "[client][observation]"
) {
	client::FrameObservationChannelRequest request;
	request.Run = 77;
	const auto bytes = client::EncodeFrameObservationChannelRequest(request);
	client::FrameObservationChannelRequest decoded;
	std::string failure;
	REQUIRE(client::DecodeFrameObservationChannelRequest(bytes, 77, decoded, failure));
	CHECK(decoded.MaxOwnedBytes == request.MaxOwnedBytes);
	CHECK(decoded.RetainedPresenceField == request.RetainedPresenceField);
	CHECK(decoded.RetainedIntegerField == request.RetainedIntegerField);
	CHECK(decoded.RetainedIntegerValue == 2);
	CHECK_FALSE(client::DecodeFrameObservationChannelRequest(bytes, 78, decoded, failure));
	CHECK_FALSE(
		client::DecodeFrameObservationChannelRequest(
			std::span(bytes).first(bytes.size() - 1), 77, decoded, failure
		)
	);
	auto trailing = bytes;
	trailing.push_back(std::byte{0});
	CHECK_FALSE(client::DecodeFrameObservationChannelRequest(trailing, 77, decoded, failure));
	CHECK_FALSE(
		client::DecodeFrameObservationChannelRequest(std::vector<std::byte>(4097), 77, decoded, failure)
	);
	request.MaxOwnedBytes = 8u * 1024u * 1024u + 1;
	CHECK_THROWS_AS(client::EncodeFrameObservationChannelRequest(request), std::invalid_argument);

	const auto ack = client::EncodeFrameObservationChannelAcknowledgement(77, 41, true);
	CHECK(client::FrameObservationChannelAcknowledged(ack, 77, 41));
	CHECK_FALSE(client::FrameObservationChannelAcknowledged(ack, 78, 41));
	CHECK_FALSE(client::FrameObservationChannelAcknowledged(ack, 77, 42));
	CHECK_FALSE(
		client::FrameObservationChannelAcknowledged(
			client::EncodeFrameObservationChannelAcknowledgement(77, 41, false), 77, 41
		)
	);
	CHECK_FALSE(client::FrameObservationChannelAcknowledged(std::span(ack).first(ack.size() - 1), 77, 41));
}

TEST_CASE(
	"native channel records own complete pixels and matching source provenance", "[client][observation]"
) {
	client::FrameObservationChannelRequest request;
	request.Run = 77;
	auto summary = Summary();
	const auto bytes = client::EncodeFrameObservationChannelSummary(summary);
	client::FrameObservationChannelSummary decoded;
	std::string failure;
	REQUIRE(client::DecodeFrameObservationChannelSummary(bytes, request, decoded, failure));
	REQUIRE(decoded.Latest);
	REQUIRE(decoded.Retained);
	CHECK(decoded.Complete);
	CHECK(decoded.CapturedFrames == 10);
	CHECK(decoded.Latest->Image.Pixels == summary.Latest->Image.Pixels);
	CHECK(decoded.Latest->Metadata == summary.Latest->Metadata);
	CHECK(decoded.Retained->Metadata == summary.Retained->Metadata);
	CHECK(decoded.Latest->Image.Frame == summary.Latest->Image.Frame);
	CHECK(decoded.Latest->Image.Token == summary.Latest->Image.Token);
	CHECK(decoded.Latest->Image.CameraFrame.FuzzyEq(summary.Latest->Image.CameraFrame, 0));
	CHECK(decoded.Latest->Image.Camera.NearPlane == .01f);
	CHECK(decoded.Latest->Image.Camera.RenderFeatures.Enable == 4);
	CHECK(decoded.Latest->Image.Camera.RenderFeatures.Disable == 8);
	CHECK(decoded.Latest->Image.Projection == summary.Latest->Image.Projection);
	CHECK(decoded.Latest->OwnedBytes >= decoded.Latest->Image.Pixels.size());
	CHECK(decoded.Retained->OwnedBytes > 0);
	CHECK(decoded.Latest->Objects.empty());
	summary.Latest.reset();
	summary.Retained.reset();
	CHECK(decoded.Latest->Image.Pixels.front() == 10);

	request.MaxOwnedBytes = decoded.Latest->OwnedBytes + decoded.Retained->OwnedBytes - 1;
	CHECK_FALSE(client::DecodeFrameObservationChannelSummary(bytes, request, decoded, failure));
	request.MaxOwnedBytes = 1;
	CHECK_FALSE(client::DecodeFrameObservationChannelSummary(bytes, request, decoded, failure));
}

TEST_CASE("native channel codec rejects stale and contradictory source records", "[client][observation]") {
	client::FrameObservationChannelRequest request;
	request.Run = 77;
	const auto summary = Summary();
	const auto bytes = client::EncodeFrameObservationChannelSummary(summary);
	client::FrameObservationChannelSummary decoded;
	std::string failure;
	const std::array replacements{
		std::pair{"\"snapshot_id\":\"native-snapshot\"", "\"snapshot_id\":\"stale--snapshot\""},
		std::pair{"\"view_slot\":3", "\"view_slot\":4"},
		std::pair{"\"name\":\"native-channel-pipeline\"", "\"name\":\"stale--channel-pipeline\""},
		std::pair{"\"revision\":7", "\"revision\":8"},
	};
	for (const auto &[from, to] : replacements) {
		CAPTURE(from);
		CHECK_FALSE(
			client::DecodeFrameObservationChannelSummary(
				ReplaceMetadata(bytes, from, to), request, decoded, failure
			)
		);
	}
	for (const auto field : {"snapshot_id", "view_slot", "pipeline"}) {
		auto inconsistent = Sample(10);
		inconsistent->Metadata.erase(field);
		auto invalid = Summary();
		invalid.Latest = std::move(inconsistent);
		CHECK_THROWS_AS(client::EncodeFrameObservationChannelSummary(invalid), std::invalid_argument);
	}
	request.Run = 78;
	CHECK_FALSE(client::DecodeFrameObservationChannelSummary(bytes, request, decoded, failure));
	request.Run = 77;
	CHECK_FALSE(
		client::DecodeFrameObservationChannelSummary(
			std::span(bytes).first(bytes.size() - 1), request, decoded, failure
		)
	);
	auto trailing = bytes;
	trailing.push_back(std::byte{0});
	CHECK_FALSE(client::DecodeFrameObservationChannelSummary(trailing, request, decoded, failure));
	CHECK_FALSE(
		client::DecodeFrameObservationChannelSummary(
			std::vector<std::byte>(client::FRAME_OBSERVATION_CHANNEL_MAXIMUM_WIRE + 1),
			request,
			decoded,
			failure
		)
	);

	auto invalid = Summary();
	auto badPixels = Sample(10);
	badPixels->Image.Pixels.pop_back();
	invalid.Latest = badPixels;
	CHECK_THROWS_AS(client::EncodeFrameObservationChannelSummary(invalid), std::invalid_argument);
	auto tooMuchMetadata = Sample(10);
	tooMuchMetadata->Metadata["oversized"] =
		std::string(client::FRAME_OBSERVATION_CHANNEL_MAXIMUM_METADATA, 'x');
	invalid.Latest = tooMuchMetadata;
	CHECK_THROWS_AS(client::EncodeFrameObservationChannelSummary(invalid), std::length_error);
}

TEST_CASE(
	"native channel selections retain only the latest and latest eligible frame", "[client][observation]"
) {
	client::FrameObservationChannelRequest request;
	request.Run = 77;
	client::FrameObservationChannelSummary history;
	std::string failure;
	auto eligible = Sample(1, true);
	std::weak_ptr<const client::FrameObservation> oldEligible = eligible;
	REQUIRE(client::RetainFrameObservationChannel(history, request, eligible, failure));
	eligible.reset();
	for (uint64_t number = 2; number <= 100; ++number) {
		std::weak_ptr<const client::FrameObservation> previousLatest = history.Latest;
		REQUIRE(client::RetainFrameObservationChannel(history, request, Sample(number), failure));
		if (number > 2) CHECK(previousLatest.expired());
		CHECK(history.Retained->ClientFrame == 1);
	}
	CHECK(history.CapturedFrames == 100);
	CHECK(history.Latest->ClientFrame == 100);
	CHECK_FALSE(oldEligible.expired());
	REQUIRE(client::RetainFrameObservationChannel(history, request, Sample(101, true), failure));
	CHECK(oldEligible.expired());
	CHECK(history.Latest == history.Retained);
	CHECK_FALSE(client::RetainFrameObservationChannel(history, request, Sample(100), failure));
	auto staleGeneration = Sample(102);
	staleGeneration->Generation = 42;
	staleGeneration->Metadata["observation_generation"] = uint64_t{42};
	CHECK_FALSE(client::RetainFrameObservationChannel(history, request, staleGeneration, failure));
	request.MaxOwnedBytes = history.Retained->OwnedBytes;
	CHECK_FALSE(client::RetainFrameObservationChannel(history, request, Sample(102), failure));
	history.Latest.reset();
	history.Retained.reset();
	CHECK(oldEligible.expired());
}

TEST_CASE(
	"native channel adapter installs before client startup and reports missing observations",
	"[client][observation]"
) {
	using engine::parallel::ChannelStatus;
	client::Client owner;
	client::FrameObservationChannelRequest request;
	request.Run = 77;
	auto endpoints = engine::parallel::MakeLocalChannel(client::FrameObservationChannelSettings());
	REQUIRE(
		endpoints.first->Send(client::EncodeFrameObservationChannelRequest(request)) == ChannelStatus::Ok
	);
	client::FrameObservationChannel adapter(owner, std::move(endpoints.second), 77);
	REQUIRE(adapter.Ready());
	REQUIRE(client::HasClientFrameSubscription(owner));
	REQUIRE(client::BeginClientFrameObservation(owner));
	bool decodedReport = false;
	client::FrameObservationChannelSummary report;
	std::string failure;
	std::jthread parent([&] {
		std::vector<std::byte> bytes;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (std::chrono::steady_clock::now() < deadline) {
			const auto status = endpoints.first->Receive(bytes);
			if (status == ChannelStatus::Ok) {
				decodedReport = client::DecodeFrameObservationChannelSummary(bytes, request, report, failure);
				const auto ack =
					client::EncodeFrameObservationChannelAcknowledgement(77, report.Generation, false);
				(void)endpoints.first->Send(ack);
				return;
			}
			if (status != ChannelStatus::Empty) return;
			std::this_thread::yield();
		}
	});
	CHECK_FALSE(adapter.Finish(0));
	parent.join();
	REQUIRE(decodedReport);
	CHECK_FALSE(report.Complete);
	CHECK(report.CapturedFrames == 0);
	CHECK_FALSE(report.Failure.empty());
	CHECK_FALSE(client::HasClientFrameSubscription(owner));
	std::vector<std::byte> closed;
	CHECK(endpoints.first->Receive(closed) == ChannelStatus::Closed);
	engine::render::Renderer renderer;
	client::CloseClientFrameObservation(owner, renderer);

	client::FrameObservationChannel absent(owner, {}, 78);
	CHECK_FALSE(absent.Ready());
	CHECK_FALSE(absent.Failure().empty());
}
