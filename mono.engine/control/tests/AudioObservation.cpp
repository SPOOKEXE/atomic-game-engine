#include "HookFixture.hpp"

#include <engine/control/Surface.hpp>
#include <engine/control/features/AudioObservation.hpp>
#include <engine/core/Name.hpp>
#include <engine/script/DataAudioObservationBridge.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.control.audioobservation")

using engine::control::Surface;
using engine::core::Name;
using engine::world::Universe;
using engine::world::WorldSettings;
using nlohmann::json;

namespace {
	constexpr const char *DIGEST = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

	json Call(Surface &surface, std::string_view name, const json &arguments, bool &failed) {
		const json reply = json::parse(surface.Answer(
			json{
				{"jsonrpc", "2.0"},
				{"id", 1},
				{"method", "tools/call"},
				{"params", {{"name", name}, {"arguments", arguments}}}
			}.dump()
		));
		const json &result = reply.at("result");
		failed = result.value("isError", false);
		return json::parse(result.at("content").at(0).at("text").get<std::string>());
	}

	class FakeBridge final : public engine::script::DataAudioObservationBridge {
	  public:
		std::string Status = "ok";
		std::string Detail;
		int CaptureCalls = 0;
		int ReadCalls = 0;
		bool OversizedObservation = false;
		bool ReturnRequestedWaveform = false;
		bool CaptureAvailable = true;
		std::string CaptureDetail = "fake bridge";
		uint64_t CaptureMaximumFrames = 256;

		engine::script::DataAudioObservationBridgeCapabilities Capabilities() const override {
			return {
				.Available = CaptureAvailable, .Detail = CaptureDetail, .MaximumFrames = CaptureMaximumFrames
			};
		}
		bool Capture(
			std::string_view instance,
			engine::script::DataAudioObservationBridgeResult &result,
			std::string &detail
		) override {
			CaptureCalls++;
			if (instance != "audio") {
				detail = "wrong instance";
				return false;
			}
			result.Status = Status;
			result.Detail = Detail;
			if (Status != "ok") return true;
			result.Observation.ObservationId = "audio/1";
			result.Observation.World = {
				.Tick = std::numeric_limits<uint64_t>::max(),
				.TimeNanoseconds = 116'666'666,
				.Epoch = 2,
				.Version = "world/v1",
				.TickInterval = {.NumeratorNanoseconds = 1'000'000'000, .Denominator = 60}
			};
			result.Observation.SampleBegin = 100;
			result.Observation.SampleEnd = 101;
			result.Observation.SampleRateHz = 48'000;
			result.Observation.Channels = 2;
			result.Observation.ChannelLayout = "stereo";
			result.Observation.Waveform = {
				.Available = true,
				.UnavailableReason = {},
				.ResourceId = "audio/1/waveform",
				.Sha256 = DIGEST,
				.ByteLength = 8,
				.Chunks = {{.Sha256 = DIGEST, .ByteBegin = 0, .ByteEnd = 8}}
			};
			result.Observation.Sources = {
				{.SourceId = "source/a", .Kind = "player", .Playing = true, .Present = true}
			};
			result.Observation.Events = {
				{.EventId = "event/a",
				 .Kind = "play",
				 .SourceId = "source/a",
				 .RelatedSourceId = {},
				 .RequestedSample = 100,
				 .AppliedSample = 100,
				 .Timing = "exact",
				 .FinishProvenance = {}}
			};
			if (OversizedObservation) {
				result.Observation.Events.clear();
				result.Observation.Sources.clear();
				for (size_t index = 0; index < 62; index++) {
					result.Observation.Sources.push_back(
						{.SourceId = "source/" + std::to_string(index),
						 .Kind = std::string(2'133, '\x01') + std::string(1'963, '"'),
						 .Present = true}
					);
				}
			}
			return true;
		}
		bool ReadWaveform(
			std::string_view,
			std::string_view resource,
			std::string_view digest,
			uint64_t begin,
			size_t maximum,
			std::vector<std::byte> &bytes,
			std::string &detail
		) override {
			ReadCalls++;
			if (resource != "audio/1/waveform" || digest != DIGEST || begin != 0 ||
				(!ReturnRequestedWaveform && maximum != 8)) {
				detail = "wrong chunk";
				return false;
			}
			bytes.assign(ReturnRequestedWaveform ? maximum : 8, std::byte{0});
			return true;
		}
	};
}

TEST_CASE(
	"audio observation MCP returns the exact record and external waveform handoff", "[control][audio]"
) {
	Universe universe;
	WorldSettings settings;
	settings.Name = Name("audio");
	universe.Create(settings);
	Surface surface("test", "test");
	const auto bridge = std::make_shared<FakeBridge>();
	engine::control::test::Install(
		surface, std::array{engine::control::test::DataAudioObservation(universe, bridge)}
	);

	bool failed = false;
	const json observation = Call(surface, "get_audio_observation", {{"instance_id", "audio"}}, failed);
	CHECK_FALSE(failed);
	CHECK(observation.dump(2).size() <= engine::script::MAX_AUDIO_OBSERVATION_JSON_BYTES);
	CHECK(observation.at("schema") == "audio_observation/v1");
	CHECK(observation.at("world").at("tick").is_number_unsigned());
	CHECK(observation.at("world").at("tick").get<uint64_t>() == std::numeric_limits<uint64_t>::max());
	CHECK(observation.at("world").at("tick_interval").at("numerator_ns") == 1'000'000'000);
	CHECK(observation.at("sample_type") == "float32");
	CHECK(observation.at("interleaved") == true);
	CHECK(observation.at("waveform").at("chunks").at(0).at("byte_end") == 8);
	CHECK(observation.at("events").at(0).at("related_source_id").is_null());

	const json bytes = Call(
		surface,
		"get_audio_waveform_chunk",
		{{"instance_id", "audio"},
		 {"resource_id", "audio/1/waveform"},
		 {"sha256", DIGEST},
		 {"byte_begin", 0},
		 {"byte_end", 8}},
		failed
	);
	CHECK_FALSE(failed);
	CHECK(bytes.dump(2).size() <= engine::script::MAX_AUDIO_OBSERVATION_JSON_BYTES);
	CHECK(bytes.at("base64") == "AAAAAAAAAAA=");
	const auto metadata =
		std::find_if(surface.Readable().begin(), surface.Readable().end(), [](const auto &resource) {
			return resource.Uri == "atomic://metadata/audio_observation/v1";
		});
	REQUIRE(metadata != surface.Readable().end());
	std::string resourceFailure;
	const json resource = json::parse(metadata->Read(resourceFailure));
	CHECK(resourceFailure.empty());
	CHECK(resource.at("contract") == "datafactories-docs/audio_observation.py");
	CHECK(resource.at("capture").at("available") == true);
	CHECK(resource.at("capture").at("detail") == "fake bridge");
	CHECK(resource.at("capture").at("maximum_frames") == 256);
	CHECK(resource.at("limits").at("events") == engine::script::MAX_AUDIO_OBSERVATION_EVENTS);
	CHECK(resource.at("waveform_handoff").at("lifetime") == "host_owned_immutable_until_expired");

	bridge->Status = "unavailable";
	bridge->Detail = "audio device is offline";
	bridge->CaptureAvailable = false;
	bridge->CaptureDetail = "audio device is offline";
	bridge->CaptureMaximumFrames = 0;
	const json unavailableResource = json::parse(metadata->Read(resourceFailure));
	CHECK(unavailableResource.at("capture").at("available") == false);
	CHECK(unavailableResource.at("capture").at("detail") == "audio device is offline");
	CHECK(unavailableResource.at("capture").at("maximum_frames") == 0);

	bridge->CaptureDetail = std::string(engine::script::MAX_AUDIO_OBSERVATION_STRING_BYTES + 1, 'x');
	const json boundedResource = json::parse(metadata->Read(resourceFailure));
	CHECK(boundedResource.at("capture").at("detail") == "invalid audio capture capability detail");
	CHECK(boundedResource.dump().size() <= engine::script::MAX_AUDIO_OBSERVATION_JSON_BYTES);

	const int captures = bridge->CaptureCalls;
	const json missing = Call(surface, "get_audio_observation", json::object(), failed);
	CHECK(failed);
	CHECK(missing.at("error").get<std::string>().find("instance_id") != std::string::npos);
	CHECK(bridge->CaptureCalls == captures);

	bridge->Status = "unknown";
	bridge->Detail = "not a valid status";
	const json badStatus = Call(surface, "get_audio_observation", {{"instance_id", "audio"}}, failed);
	CHECK(failed);
	CHECK(badStatus.at("error").get<std::string>().find("invalid_bridge_reply") != std::string::npos);

	bridge->Status = "unavailable";
	bridge->Detail = "audio device is offline";
	const json unavailable = Call(surface, "get_audio_observation", {{"instance_id", "audio"}}, failed);
	CHECK_FALSE(failed);
	CHECK(unavailable.at("status") == "unavailable");
	CHECK(unavailable.at("detail") == "audio device is offline");

	bridge->Status = "unavailable";
	bridge->Detail = std::string{"\xC0", 1};
	const json badDetail = Call(surface, "get_audio_observation", {{"instance_id", "audio"}}, failed);
	CHECK(failed);
	CHECK(badDetail.at("error").get<std::string>().find("invalid_bridge_reply") != std::string::npos);
	bridge->Status = "ok";
	bridge->Detail.clear();

	const int reads = bridge->ReadCalls;
	for (const std::string &digest : {std::string(64, 'A'), std::string(63, 'a'), std::string(64, 'g')}) {
		const json rejected = Call(
			surface,
			"get_audio_waveform_chunk",
			{{"instance_id", "audio"},
			 {"resource_id", "audio/1/waveform"},
			 {"sha256", digest},
			 {"byte_begin", 0},
			 {"byte_end", 8}},
			failed
		);
		CHECK(failed);
		CHECK(rejected.at("error").get<std::string>().find("sha256") != std::string::npos);
	}
	CHECK(bridge->ReadCalls == reads);

	bridge->OversizedObservation = true;
	engine::script::DataAudioObservationBridgeResult almostLimit;
	std::string almostLimitDetail;
	REQUIRE(bridge->Capture("audio", almostLimit, almostLimitDetail));
	const json compact = engine::control::audio_observation_detail::Record(almostLimit.Observation);
	CHECK(compact.dump().size() <= engine::script::MAX_AUDIO_OBSERVATION_JSON_BYTES);
	CHECK(compact.dump(2).size() > engine::script::MAX_AUDIO_OBSERVATION_JSON_BYTES);
	const json oversized = Call(surface, "get_audio_observation", {{"instance_id", "audio"}}, failed);
	CHECK(failed);
	CHECK(oversized.at("error").get<std::string>().find("resource_limit") != std::string::npos);
	bridge->OversizedObservation = false;

	bridge->ReturnRequestedWaveform = true;
	const json oversizedWaveform = Call(
		surface,
		"get_audio_waveform_chunk",
		{{"instance_id", "audio"},
		 {"resource_id", "audio/1/waveform"},
		 {"sha256", DIGEST},
		 {"byte_begin", 0},
		 {"byte_end", engine::script::MAX_AUDIO_OBSERVATION_JSON_BYTES}},
		failed
	);
	CHECK(failed);
	CHECK(oversizedWaveform.at("error").get<std::string>().find("resource_limit") != std::string::npos);
}
