#include <engine/audio/Device.hpp>
#include <engine/audio/Sample.hpp>
#include <engine/control/Surface.hpp>
#include <engine/control/features/AudioObservation.hpp>
#include <engine/control/features/DataFactory.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Audio.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <client/DataAudioObservation.hpp>
#include <client/Sounds.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("client.data-audio-observation")
TEST_DEPENDS("engine.audio.observation")
TEST_DEPENDS("engine.script.dataaudioobservationbridge")

namespace {
	engine::world::DataFactoryReply Clock(uint64_t epoch = 2, uint64_t version = 4) {
		engine::world::DataFactoryReply reply;
		reply.Status = engine::world::DataFactoryStatus::Ok;
		reply.InstanceId = "data-audio.world";
		reply.WorldEpoch = epoch;
		reply.WorldVersion = version;
		reply.Clock = {
			.Tick = 7,
			.TimeNanoseconds = 116'666'666,
			.RationalTimeAvailable = true,
			.Interval = {.NumeratorNanoseconds = 1'000'000'000, .Denominator = 60},
		};
		return reply;
	}

	engine::world::DataFactoryReply ClockAt(uint64_t tick, uint64_t epoch = 2, uint64_t version = 4) {
		auto clock = Clock(epoch, version);
		clock.Clock.Tick = tick;
		clock.Clock.TimeNanoseconds = tick * 16'666'666;
		return clock;
	}

	struct Rendered {
		std::unique_ptr<engine::audio::NullDevice> Device;
		engine::audio::NodeId Player;
	};

	Rendered RenderTone() {
		Rendered rendered;
		rendered.Device = engine::audio::OpenNullDevice({});
		auto &mixer = rendered.Device->Mixer();
		rendered.Player = mixer.Commands().Allocate();
		engine::audio::Command command;
		command.Kind = engine::audio::CommandKind::AddNode;
		command.Target = rendered.Player;
		command.Node = engine::audio::NodeKind::Player;
		REQUIRE(mixer.Commands().Post(command));
		command = {};
		command.Kind = engine::audio::CommandKind::Connect;
		command.Target = rendered.Player;
		command.Second = mixer.Graph().Output();
		REQUIRE(mixer.Commands().Post(command));
		const std::array<float, 8> samples{0.25f, -0.25f, 0.5f, -0.5f, 0.25f, -0.25f, 0.5f, -0.5f};
		command = {};
		command.Kind = engine::audio::CommandKind::SetSound;
		command.Target = rendered.Player;
		command.Sound = std::make_shared<const engine::audio::SampleBuffer>(mixer.Format(), samples);
		REQUIRE(mixer.Commands().Post(command));
		command = {};
		command.Kind = engine::audio::CommandKind::Play;
		command.Target = rendered.Player;
		REQUIRE(mixer.Commands().Post(command));
		REQUIRE(rendered.Device->Advance() > 0);
		return rendered;
	}

	engine::audio::MixReport Report(const Rendered &rendered) {
		const auto &mixer = rendered.Device->Mixer();
		const auto &waveform = rendered.Device->LastBlock();
		return {
			.Frames = waveform.Frames(),
			.BeginSample = mixer.Clock() - waveform.Frames(),
			.EndSample = mixer.Clock(),
			.ObservationSerial = mixer.ObservationSerial(),
		};
	}
}

TEST_CASE(
	"direct data-factory control boundaries publish each paused step and restore before the next step",
	"[client][audio][data-factory]"
) {
	engine::world::Universe worlds;
	engine::world::WorldSettings settings;
	settings.Name = engine::core::Name("client-control-audio");
	worlds.Create(settings);
	engine::world::DataFactorySession session(worlds);
	session.SetPauseParticipant(
		[](engine::world::WorldId, engine::world::DataFactoryPauseScope, bool, std::string &) { return true; }
	);
	session.SetRehydrate([](engine::world::Universe &, engine::world::WorldId, std::string &) {
		return true;
	});
	engine::control::Surface surface("client-control-audio", "client audio control boundary test");
	surface.Enable(std::array{engine::control::features::DataFactory(session)});

	std::vector<engine::world::DataFactoryReply> observed;
	const auto answer = [&](std::string_view tool, nlohmann::json arguments) {
		return client::AnswerDataFactoryControlLine(
			session,
			"client-control-audio",
			[&] {
				return surface.Answer(
					nlohmann::json{
						{"jsonrpc", "2.0"},
						{"id", 1},
						{"method", "tools/call"},
						{"params", {{"name", tool}, {"arguments", std::move(arguments)}}},
					}
						.dump()
				);
			},
			[&](const engine::world::DataFactoryReply &clock) { observed.push_back(clock); }
		);
	};
	const auto initial = session.Inspect("client-control-audio");
	REQUIRE(
		nlohmann::json::parse(
			answer(
				"pause",
				{{"instance_id", initial.InstanceId},
				 {"expected_tick", initial.Clock.Tick},
				 {"expected_world_epoch", initial.WorldEpoch},
				 {"expected_world_version", initial.WorldVersion}}
			)
		).contains("result")
	);
	const auto paused = session.Inspect(initial.InstanceId);
	const auto checkpointReply = nlohmann::json::parse(answer(
		"checkpoint",
		{{"instance_id", paused.InstanceId},
		 {"expected_tick", paused.Clock.Tick},
		 {"expected_world_epoch", paused.WorldEpoch},
		 {"expected_world_version", paused.WorldVersion},
		 {"operation_id", "client-audio-checkpoint"}}
	));
	const nlohmann::json checkpoint =
		nlohmann::json::parse(checkpointReply.at("result").at("content").at(0).at("text").get<std::string>());
	const auto step = [&](const engine::world::DataFactoryReply &clock, std::string_view id) {
		return nlohmann::json::parse(answer(
			"step",
			{{"instance_id", clock.InstanceId},
			 {"expected_tick", clock.Clock.Tick},
			 {"expected_world_epoch", clock.WorldEpoch},
			 {"expected_world_version", clock.WorldVersion},
			 {"operation_id", id},
			 {"dt_ns", {{"numerator", 1'000'000'000u}, {"denominator", 60u}}},
			 {"actions", nlohmann::json::array()}}
		));
	};
	REQUIRE(step(paused, "client-audio-step-1").contains("result"));
	const auto tickOne = session.Inspect(initial.InstanceId);
	REQUIRE(step(tickOne, "client-audio-step-2").contains("result"));
	const auto tickTwo = session.Inspect(initial.InstanceId);
	REQUIRE(
		nlohmann::json::parse(
			answer(
				"restore",
				{{"instance_id", tickTwo.InstanceId},
				 {"expected_tick", tickTwo.Clock.Tick},
				 {"expected_world_epoch", tickTwo.WorldEpoch},
				 {"expected_world_version", tickTwo.WorldVersion},
				 {"operation_id", "client-audio-restore"},
				 {"checkpoint_id", checkpoint.at("checkpoint_id")}}
			)
		).contains("result")
	);
	const auto restored = session.Inspect(initial.InstanceId);
	REQUIRE(step(restored, "client-audio-step-after-restore").contains("result"));

	REQUIRE(observed.size() == 4);
	CHECK(observed[0].Clock.Tick == 1);
	CHECK(observed[1].Clock.Tick == 2);
	CHECK(observed[2].Clock.Tick == 0);
	CHECK(observed[3].Clock.Tick == 1);
	CHECK(observed[0].WorldEpoch == 1);
	CHECK(observed[1].WorldEpoch == 1);
	CHECK(observed[2].WorldEpoch == 2);
	CHECK(observed[3].WorldEpoch == 2);
}

TEST_CASE(
	"data-factory audio host retains deterministic SHA-256 waveform records", "[client][audio][data-factory]"
) {
	client::DataAudioObservationHost host;
	Rendered rendered = RenderTone();
	std::string detail;
	host.ResetTickClock(ClockAt(0), 48'000);
	const std::vector<client::AudioObservationSourceBinding> sources{{"fixture/tone", rendered.Player}};
	REQUIRE(host.Publish(
		Clock(), rendered.Device->Mixer(), rendered.Device->LastBlock(), Report(rendered), sources, detail
	));

	engine::script::DataAudioObservationBridgeResult first;
	REQUIRE(host.Capture("data-audio.world", first, detail));
	REQUIRE(first.Status == "ok");
	CHECK(first.Observation.World.Tick == 7);
	CHECK(first.Observation.World.TimeNanoseconds == 116'666'666);
	CHECK(first.Observation.World.Epoch == 2);
	CHECK(first.Observation.World.Version == "4");
	CHECK(first.Observation.Sources.size() == 1);
	CHECK(first.Observation.Sources[0].SourceId == "fixture/tone");
	CHECK(first.Observation.Waveform.Sha256.size() == 64);
	CHECK(
		first.Observation.Waveform.ByteLength == rendered.Device->LastBlock().Data().size() * sizeof(float)
	);

	std::vector<std::byte> bytes;
	REQUIRE(host.ReadWaveform(
		"data-audio.world",
		first.Observation.Waveform.ResourceId,
		first.Observation.Waveform.Sha256,
		0,
		static_cast<size_t>(first.Observation.Waveform.ByteLength),
		bytes,
		detail
	));
	CHECK(bytes.size() == first.Observation.Waveform.ByteLength);
	engine::script::DataAudioObservationBridgeResult retry;
	REQUIRE(host.Capture("data-audio.world", retry, detail));
	CHECK(retry.Observation.ObservationId == first.Observation.ObservationId);
	CHECK(retry.Observation.Waveform.Sha256 == first.Observation.Waveform.Sha256);
}

TEST_CASE(
	"data-factory audio host expires waveform records on a lifecycle version change",
	"[client][audio][data-factory]"
) {
	client::DataAudioObservationHost host;
	Rendered rendered = RenderTone();
	std::string detail;
	const std::vector<client::AudioObservationSourceBinding> sources{{"fixture/tone", rendered.Player}};
	REQUIRE(host.Publish(
		Clock(), rendered.Device->Mixer(), rendered.Device->LastBlock(), Report(rendered), sources, detail
	));
	engine::script::DataAudioObservationBridgeResult first;
	REQUIRE(host.Capture("data-audio.world", first, detail));

	host.InvalidateUnless(Clock(2, 5));
	std::vector<std::byte> bytes;
	CHECK_FALSE(host.ReadWaveform(
		"data-audio.world",
		first.Observation.Waveform.ResourceId,
		first.Observation.Waveform.Sha256,
		0,
		1,
		bytes,
		detail
	));
	CHECK(detail.find("stale") != std::string::npos);
	engine::script::DataAudioObservationBridgeResult after;
	REQUIRE(host.Capture("data-audio.world", after, detail));
	CHECK(after.Status == "unavailable");
}

TEST_CASE(
	"sound observation bindings require authored unique DataFactoryIds", "[client][audio][data-factory]"
) {
	engine::ecs::Store store("data-audio-labels");
	engine::audio::AudioMixer mixer;
	client::SoundStage stage;
	const engine::ecs::Entity sound = store.CreateInstance(engine::scene::SoundClass());
	REQUIRE(sound != engine::ecs::NULL_ENTITY);
	auto *row = store.GetMutable<engine::scene::Sound>(sound);
	REQUIRE(row != nullptr);
	row->SoundId = engine::core::Name("tone.wav");
	row->Playing = true;
	client::SoundCatalogue catalogue;
	const std::array<float, 4> samples{0.25f, 0.25f, 0.25f, 0.25f};
	REQUIRE(catalogue.Add(
		engine::core::Name("tone.wav"),
		std::make_shared<const engine::audio::SampleBuffer>(mixer.Format(), samples)
	));
	stage.Sync(store, mixer, catalogue, {}, mixer.Format().SampleRate);
	std::vector<client::AudioObservationSourceBinding> sources;
	std::string detail;
	CHECK_FALSE(stage.CollectObservationSources(store, sources, detail));
	CHECK(detail.find("DataFactoryId") != std::string::npos);

	engine::ecs::AttributeValue id;
	id.Type = engine::ecs::PropertyType::String;
	id.String = "fixture/tone";
	REQUIRE(engine::ecs::SetAttribute(store, sound, engine::core::Name("DataFactoryId"), id));
	REQUIRE(stage.CollectObservationSources(store, sources, detail));
	REQUIRE(sources.size() == 2);
	CHECK(sources[0].SourceId == "fixture/tone/1/fader");
	CHECK(sources[1].SourceId == "fixture/tone/1/player");
}

TEST_CASE("data-factory listener reads the current world camera", "[client][audio][data-factory]") {
	engine::ecs::Store store("data-audio-listener");
	const engine::ecs::Entity camera = store.CreateInstance(engine::scene::PartClass());
	REQUIRE(camera != engine::ecs::NULL_ENTITY);
	auto *transform = store.GetMutable<engine::scene::Transform>(camera);
	REQUIRE(transform != nullptr);
	transform->Frame.Position = {3, 4, 5};
	store.SetResource(engine::scene::ActiveCamera{.Entity = camera});
	CHECK(client::DataFactoryListener(store, {9, 9, 9}) == engine::core::Vector3{3, 4, 5});
	transform->Frame.Position = {6, 7, 8};
	CHECK(client::DataFactoryListener(store, {9, 9, 9}) == engine::core::Vector3{6, 7, 8});
}

TEST_CASE(
	"one fixed tick joins bounded mix slices into one retrievable waveform", "[client][audio][data-factory]"
) {
	engine::world::Universe worlds;
	const engine::world::WorldId world = worlds.Create({.Name = engine::core::Name("data-audio.world")});
	REQUIRE(world.IsValid());
	const auto host = std::make_shared<client::DataAudioObservationHost>();
	engine::control::Surface surface("client-data-audio", "client data audio tests");
	surface.Enable(std::array{engine::control::features::DataAudioObservation(worlds, host)});
	const auto findTool = [&](std::string_view name) {
		return std::find_if(surface.Registered().begin(), surface.Registered().end(), [&](const auto &entry) {
			return entry.Name == name;
		});
	};
	const auto observationTool = findTool("get_audio_observation");
	const auto waveformTool = findTool("get_audio_waveform_chunk");
	REQUIRE(observationTool != surface.Registered().end());
	REQUIRE(waveformTool != surface.Registered().end());
	Rendered rendered = RenderTone();
	std::string detail;
	const std::vector<client::AudioObservationSourceBinding> sources{{"fixture/tone", rendered.Player}};
	REQUIRE(host->Publish(
		ClockAt(1),
		rendered.Device->Mixer(),
		rendered.Device->LastBlock(),
		Report(rendered),
		sources,
		detail,
		false
	));
	// The first call is accepted but intentionally has no externally complete
	// tick record until the 288-frame tail is rendered and joined below.
	CHECK(detail.empty());
	engine::audio::SampleBuffer tail(rendered.Device->Format(), 288);
	const auto report = rendered.Device->Mixer().Render(tail);
	REQUIRE(host->Publish(ClockAt(1), rendered.Device->Mixer(), tail, report, sources, detail));
	std::string failure;
	const nlohmann::json observation = observationTool->Call({{"instance_id", "data-audio.world"}}, failure);
	REQUIRE(failure.empty());
	CHECK(observation["sample_end"].get<uint64_t>() - observation["sample_begin"].get<uint64_t>() == 800);
	CHECK(observation["waveform"]["byte_length"].get<uint64_t>() == 800 * 2 * sizeof(float));
	const nlohmann::json waveform = waveformTool->Call(
		{{"instance_id", "data-audio.world"},
		 {"resource_id", observation["waveform"]["resource_id"]},
		 {"sha256", observation["waveform"]["sha256"]},
		 {"byte_begin", uint64_t{0}},
		 {"byte_end", observation["waveform"]["byte_length"]}},
		failure
	);
	REQUIRE(failure.empty());
	CHECK(waveform["byte_end"].get<uint64_t>() == 800 * 2 * sizeof(float));
	CHECK(waveform["base64"].get<std::string>().size() == ((800 * 2 * sizeof(float) + 2) / 3) * 4);
}

TEST_CASE(
	"data-factory audio frame schedule follows fixed ticks rather than presentation",
	"[client][audio][data-factory]"
) {
	client::DataAudioObservationHost everyFrame;
	client::DataAudioObservationHost batched;
	CHECK(everyFrame.AdvanceFrames(ClockAt(0), 48'000).empty());
	const auto first = everyFrame.AdvanceFrames(ClockAt(1), 48'000);
	const auto second = everyFrame.AdvanceFrames(ClockAt(2), 48'000);
	const auto third = everyFrame.AdvanceFrames(ClockAt(3), 48'000);
	CHECK(first == std::vector<size_t>({512, 288}));
	CHECK(second == std::vector<size_t>({512, 288}));
	CHECK(third == std::vector<size_t>({512, 288}));

	CHECK(batched.AdvanceFrames(ClockAt(0), 48'000).empty());
	CHECK(batched.AdvanceFrames(ClockAt(3), 48'000) == std::vector<size_t>({512, 288, 512, 288, 512, 288}));
	// A paused presentation frame changes no completed simulation tick and so
	// cannot advance the deterministic null mixer.
	CHECK(batched.AdvanceFrames(ClockAt(3), 48'000).empty());
}

TEST_CASE(
	"discarded unlabeled tick does not synthesize a later labeled capture", "[client][audio][data-factory]"
) {
	client::DataAudioObservationHost host;
	Rendered rendered = RenderTone();
	std::string detail;
	host.ResetTickClock(ClockAt(0), 48'000);
	// The client drains these two slices while labels are invalid, without publishing them.
	for (const size_t frames : host.AdvanceFrames(ClockAt(1), 48'000)) {
		engine::audio::SampleBuffer discarded(rendered.Device->Format(), frames);
		(void)rendered.Device->Mixer().Render(discarded);
	}
	const std::vector<client::AudioObservationSourceBinding> sources{{"fixture/repaired", rendered.Player}};
	const auto slices = host.AdvanceFrames(ClockAt(2), 48'000);
	REQUIRE(slices == std::vector<size_t>({512, 288}));
	for (size_t index = 0; index < slices.size(); ++index) {
		engine::audio::SampleBuffer waveform(rendered.Device->Format(), slices[index]);
		const auto report = rendered.Device->Mixer().Render(waveform);
		REQUIRE(host.Publish(
			ClockAt(2),
			rendered.Device->Mixer(),
			waveform,
			report,
			sources,
			detail,
			index + 1 == slices.size()
		));
	}
	engine::script::DataAudioObservationBridgeResult record;
	REQUIRE(host.Capture("data-audio.world", record, detail));
	CHECK(record.Observation.World.Tick == 2);
	CHECK(record.Observation.SampleBegin == 1312);
}

TEST_CASE(
	"audio reset starts at the restored fixed tick without replaying prior time",
	"[client][audio][data-factory]"
) {
	client::DataAudioObservationHost host;
	Rendered rendered = RenderTone();
	std::string detail;
	const std::vector<client::AudioObservationSourceBinding> sources{{"fixture/tone", rendered.Player}};
	REQUIRE(host.Publish(
		ClockAt(1), rendered.Device->Mixer(), rendered.Device->LastBlock(), Report(rendered), sources, detail
	));
	host.ResetTickClock(ClockAt(10'000, 3, 9), 48'000);
	engine::script::DataAudioObservationBridgeResult unavailable;
	REQUIRE(host.Capture("data-audio.world", unavailable, detail));
	CHECK(unavailable.Status == "unavailable");
	CHECK(host.AdvanceFrames(ClockAt(10'000, 3, 9), 48'000).empty());
	CHECK(host.AdvanceFrames(ClockAt(10'001, 3, 9), 48'000) == std::vector<size_t>({512, 288}));
}

TEST_CASE(
	"sound replacement and reparenting keep distinct observation voice incarnations",
	"[client][audio][data-factory]"
) {
	engine::ecs::Store store("data-audio-incarnation");
	engine::audio::AudioMixer mixer;
	client::SoundStage stage;
	client::SoundCatalogue catalogue;
	const std::array<float, 4> samples{0.25f, 0.25f, 0.25f, 0.25f};
	REQUIRE(catalogue.Add(
		engine::core::Name("first.wav"),
		std::make_shared<const engine::audio::SampleBuffer>(mixer.Format(), samples)
	));
	REQUIRE(catalogue.Add(
		engine::core::Name("second.wav"),
		std::make_shared<const engine::audio::SampleBuffer>(mixer.Format(), samples)
	));
	const engine::ecs::Entity sound = store.CreateInstance(engine::scene::SoundClass());
	REQUIRE(sound != engine::ecs::NULL_ENTITY);
	auto *row = store.GetMutable<engine::scene::Sound>(sound);
	REQUIRE(row != nullptr);
	row->SoundId = engine::core::Name("first.wav");
	row->Playing = true;
	engine::ecs::AttributeValue id;
	id.Type = engine::ecs::PropertyType::String;
	id.String = "fixture/replaced";
	REQUIRE(engine::ecs::SetAttribute(store, sound, engine::core::Name("DataFactoryId"), id));
	stage.Sync(store, mixer, catalogue, {}, mixer.Format().SampleRate);
	const engine::audio::NodeId firstPlayer = stage.Find(sound)->Player;
	std::vector<client::AudioObservationSourceBinding> sources;
	std::string detail;
	REQUIRE(stage.CollectObservationSources(store, sources, detail));
	const engine::ecs::Entity part = store.CreateInstance(engine::scene::PartClass());
	REQUIRE(part != engine::ecs::NULL_ENTITY);
	REQUIRE(store.SetParent(sound, part));
	row->SoundId = engine::core::Name("second.wav");
	stage.Sync(store, mixer, catalogue, {}, mixer.Format().SampleRate);
	REQUIRE(stage.Find(sound) != nullptr);
	CHECK_FALSE(stage.Find(sound)->Player == firstPlayer);

	REQUIRE(stage.CollectObservationSources(store, sources, detail));
	CHECK(sources.size() == 5);
	CHECK(sources[0].SourceId.find("fixture/replaced/1/") == 0);
	CHECK(sources.back().SourceId.find("fixture/replaced/2/") == 0);
	CHECK(std::adjacent_find(sources.begin(), sources.end(), [](const auto &left, const auto &right) {
			  return left.SourceId == right.SourceId;
		  }) == sources.end());

	engine::audio::SampleBuffer waveform(mixer.Format(), engine::audio::DEFAULT_BLOCK_FRAMES);
	const engine::audio::MixReport report = mixer.Render(waveform);
	client::DataAudioObservationHost host;
	REQUIRE(host.Publish(ClockAt(1), mixer, waveform, report, sources, detail));
	engine::script::DataAudioObservationBridgeResult record;
	REQUIRE(host.Capture("data-audio.world", record, detail));
	CHECK(record.Status == "ok");
	stage.ConsumeObservationSources();
}

TEST_CASE(
	"data-factory audio keeps bounded immutable artifacts until deterministic expiry",
	"[client][audio][data-factory]"
) {
	client::DataAudioObservationHost host;
	Rendered rendered = RenderTone();
	std::string detail;
	const std::vector<client::AudioObservationSourceBinding> sources{{"fixture/tone", rendered.Player}};
	REQUIRE(host.Publish(
		Clock(), rendered.Device->Mixer(), rendered.Device->LastBlock(), Report(rendered), sources, detail
	));
	engine::script::DataAudioObservationBridgeResult first;
	REQUIRE(host.Capture("data-audio.world", first, detail));
	REQUIRE(rendered.Device->Advance() > 0);
	REQUIRE(host.Publish(
		Clock(), rendered.Device->Mixer(), rendered.Device->LastBlock(), Report(rendered), sources, detail
	));

	std::vector<std::byte> firstBytes;
	REQUIRE(host.ReadWaveform(
		"data-audio.world",
		first.Observation.Waveform.ResourceId,
		first.Observation.Waveform.Sha256,
		0,
		static_cast<size_t>(first.Observation.Waveform.ByteLength),
		firstBytes,
		detail
	));
	CHECK_FALSE(firstBytes.empty());
	host.InvalidateUnless(Clock(2, 5));
	CHECK_FALSE(host.ReadWaveform(
		"data-audio.world",
		first.Observation.Waveform.ResourceId,
		first.Observation.Waveform.Sha256,
		0,
		1,
		firstBytes,
		detail
	));
}

TEST_CASE(
	"capture and resource reads inspect the current lifecycle before returning bytes",
	"[client][audio][data-factory]"
) {
	engine::world::DataFactoryReply current = Clock();
	client::DataAudioObservationHost host([&current](std::string_view) { return current; });
	Rendered rendered = RenderTone();
	std::string detail;
	const std::vector<client::AudioObservationSourceBinding> sources{{"fixture/tone", rendered.Player}};
	REQUIRE(host.Publish(
		current, rendered.Device->Mixer(), rendered.Device->LastBlock(), Report(rendered), sources, detail
	));
	engine::script::DataAudioObservationBridgeResult record;
	REQUIRE(host.Capture("data-audio.world", record, detail));
	current = Clock(3, 9);
	std::vector<std::byte> bytes;
	CHECK_FALSE(host.ReadWaveform(
		"data-audio.world",
		record.Observation.Waveform.ResourceId,
		record.Observation.Waveform.Sha256,
		0,
		1,
		bytes,
		detail
	));
	engine::script::DataAudioObservationBridgeResult stale;
	REQUIRE(host.Capture("data-audio.world", stale, detail));
	CHECK(stale.Status == "unavailable");
}

TEST_CASE(
	"audio observation MCP feature registers both bounded data-factory tools", "[client][audio][data-factory]"
) {
	engine::world::Universe worlds;
	const auto host = std::make_shared<client::DataAudioObservationHost>();
	engine::control::Surface surface("client-data-audio", "client data audio tests");
	surface.Enable(std::array{engine::control::features::DataAudioObservation(worlds, host)});
	bool observation = false;
	bool waveform = false;
	for (const auto &tool : surface.Registered()) {
		observation = observation || tool.Name == "get_audio_observation";
		waveform = waveform || tool.Name == "get_audio_waveform_chunk";
	}
	CHECK(observation);
	CHECK(waveform);
}

TEST_CASE(
	"audio observation MCP refuses stale factory lifecycle revisions", "[client][audio][data-factory]"
) {
	engine::world::Universe worlds;
	engine::world::Universe decoy;
	engine::world::DataFactorySession session(worlds);
	session.SetPauseParticipant(
		[](engine::world::WorldId, engine::world::DataFactoryPauseScope, bool, std::string &) { return true; }
	);
	const engine::world::DataFactoryWorldRequest create{
		.Operation = engine::world::DataFactoryWorldOperation::Create,
		.InstanceId = "audio-fence",
		.TickRate = 60.0,
		.OperationId = "audio-fence-create",
	};
	REQUIRE(session.CreateWorld(create).Status == engine::world::DataFactoryStatus::Ok);
	engine::world::WorldSettings decoySettings;
	decoySettings.Name = engine::core::Name("audio-fence");
	REQUIRE(decoy.Create(decoySettings).IsValid());
	const auto host = std::make_shared<client::DataAudioObservationHost>();
	engine::control::Surface surface("client-data-audio", "client data audio tests");
	surface.Enable(std::array{engine::control::features::DataAudioObservation(decoy, host, &session)});
	const auto observation =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
			return tool.Name == "get_audio_observation";
		});
	REQUIRE(observation != surface.Registered().end());
	const auto current = session.Inspect("audio-fence");
	REQUIRE(current.Status == engine::world::DataFactoryStatus::Ok);
	const nlohmann::json revision{
		{"expected_tick", current.Clock.Tick},
		{"expected_world_epoch", current.WorldEpoch},
		{"expected_world_version", current.WorldVersion},
	};

	std::string failure;
	const nlohmann::json currentRead = observation->Call(
		{{"instance_id", "audio-fence"},
		 {"expected_tick", revision["expected_tick"]},
		 {"expected_world_epoch", revision["expected_world_epoch"]},
		 {"expected_world_version", revision["expected_world_version"]}},
		failure
	);
	INFO(currentRead.dump());
	CHECK(failure.find("no scene") == std::string::npos);

	const auto waveform =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &tool) {
			return tool.Name == "get_audio_waveform_chunk";
		});
	REQUIRE(waveform != surface.Registered().end());
	failure.clear();
	const nlohmann::json waveformRead = waveform->Call(
		{{"instance_id", "audio-fence"},
		 {"resource_id", "audio/fence"},
		 {"sha256", std::string(64, 'a')},
		 {"byte_begin", 0},
		 {"byte_end", 1},
		 {"expected_tick", revision["expected_tick"]},
		 {"expected_world_epoch", revision["expected_world_epoch"]},
		 {"expected_world_version", revision["expected_world_version"]}},
		failure
	);
	INFO(waveformRead.dump());
	CHECK(failure.find("instance_id is invalid") == std::string::npos);

	failure.clear();
	const nlohmann::json missing = observation->Call({{"instance_id", "audio-fence"}}, failure);
	CHECK(missing.is_null());
	CHECK(failure.find("expected lifecycle revision") != std::string::npos);
	REQUIRE(
		session.CommitExternalMutation("audio-fence", current.Clock.Tick, current.WorldVersion).Status ==
		engine::world::DataFactoryStatus::Ok
	);
	failure.clear();
	const nlohmann::json stale = observation->Call(
		{{"instance_id", "audio-fence"},
		 {"expected_tick", revision["expected_tick"]},
		 {"expected_world_epoch", revision["expected_world_epoch"]},
		 {"expected_world_version", revision["expected_world_version"]}},
		failure
	);
	CHECK(failure.find("version_conflict") != std::string::npos);
	CHECK(stale.at("status") == "version_conflict");
	CHECK(stale.at("current_world_version") == current.WorldVersion + 1);
}
