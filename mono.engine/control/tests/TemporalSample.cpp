#include "HookFixture.hpp"

#include <engine/control/Surface.hpp>
#include <engine/control/features/TemporalSample.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

TEST_SUITE_ID("engine.control.temporalsample")
TEST_DEPENDS("engine.control.datascene")

using engine::control::Surface;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::AttributeValue;
using engine::ecs::Entity;
using engine::ecs::PropertyType;
using engine::scene::Camera;
using engine::scene::Transform;
using engine::world::DataFactoryPauseScope;
using engine::world::DataFactorySession;
using engine::world::DataFactoryStatus;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;
using nlohmann::json;

namespace {
	json Call(Surface &surface, const json &arguments, bool &failed) {
		const json request{
			{"jsonrpc", "2.0"},
			{"id", 1},
			{"method", "tools/call"},
			{"params", {{"name", "get_temporal_sample"}, {"arguments", arguments}}},
		};
		const json reply = json::parse(surface.Answer(request.dump()));
		const json &result = reply.at("result");
		failed = result.value("isError", false);
		return json::parse(result.at("content").at(0).at("text").get<std::string>());
	}

	WorldId World(Universe &universe, std::string_view name) {
		WorldSettings settings;
		settings.Name = Name(name);
		return universe.Create(settings);
	}

	void Identify(engine::ecs::Store &store, Entity entity, std::string_view id) {
		AttributeValue value;
		value.Type = PropertyType::String;
		value.String = id;
		REQUIRE(engine::ecs::SetAttribute(store, entity, Name("DataFactoryId"), value));
	}

	struct Fixture {
		Universe Worlds;
		WorldId Id = World(Worlds, "temporal-sample");
		DataFactorySession Session{Worlds};
		Surface Control{"test", "test"};

		Fixture() {
			Session.SetPauseParticipant([](WorldId, DataFactoryPauseScope, bool, std::string &) {
				return true;
			});
			engine::control::test::Install(
				Control, std::array{engine::control::test::TemporalSample(Worlds, Session)}
			);
			Worlds.Enter(Id, [](engine::ecs::Store &) {
				if (!engine::ecs::Components::Find(Name("scene.Camera")).IsValid())
					engine::scene::RegisterSceneComponents();
				if (!engine::ecs::Classes::Find(Name("Camera")).IsValid())
					engine::scene::RegisterSceneClasses();
			});
		}

		Entity Add(std::string_view className, std::string_view name, std::string_view id, CFrame frame) {
			Entity entity;
			Worlds.Enter(Id, [&](engine::ecs::Store &store) {
				entity = store.CreateInstance(engine::ecs::Classes::Find(Name(className)), name);
				store.Set(entity, Transform{frame});
				Identify(store, entity, id);
			});
			return entity;
		}

		json SnapshotRequest(std::initializer_list<const char *> objects) {
			const auto before = Session.Inspect("temporal-sample");
			REQUIRE(
				Session.Pause("temporal-sample", DataFactoryPauseScope::AllSystems, before.Clock.Tick)
					.Status == DataFactoryStatus::Ok
			);
			std::string snapshot;
			REQUIRE(Session.Snapshot("temporal-sample", snapshot).Status == DataFactoryStatus::Ok);
			const auto after = Session.Inspect("temporal-sample");
			return {
				{"instance_id", "temporal-sample"},
				{"snapshot_id", snapshot},
				{"expected_world_epoch", after.WorldEpoch},
				{"expected_world_version", after.WorldVersion},
				{"expected_tick", after.Clock.Tick},
				{"camera_id", "camera/main"},
				{"object_ids", objects},
			};
		}
	};
}

TEST_CASE(
	"temporal sample copies one retained camera and sorted requested object poses",
	"[control][temporal-sample]"
) {
	Fixture fixture;
	fixture.Add("Camera", "Camera", "camera/main", CFrame{Vector3{1.0f, 2.0f, 3.0f}});
	fixture.Add("Part", "Alpha", "object/alpha", CFrame{Vector3{4.0f, 5.0f, 6.0f}});
	fixture.Add("Part", "Beta", "object/beta", CFrame{Vector3{7.0f, 8.0f, 9.0f}});

	bool failed = false;
	const json sample = fixture.SnapshotRequest({"object/beta", "object/missing", "object/alpha"});
	const json result = Call(fixture.Control, sample, failed);
	INFO(result.dump());
	CHECK_FALSE(failed);
	CHECK(result.at("schema_version") == "temporal-sample/v1");
	CHECK(result.at("camera").at("id") == "camera/main");
	CHECK(result.at("camera").at("world_from_camera").at("position") == json::array({1.0, 2.0, 3.0}));
	REQUIRE(result.at("objects").size() == 3);
	CHECK(result.at("objects").at(0).at("id") == "object/alpha");
	CHECK(result.at("objects").at(0).at("pose").at("available") == true);
	CHECK(result.at("objects").at(1).at("id") == "object/beta");
	CHECK(result.at("objects").at(2).at("id") == "object/missing");
	CHECK(result.at("objects").at(2).at("pose").at("available") == false);
	CHECK(result.at("objects").at(2).at("pose").at("reason") == "stable_object_not_found");
	CHECK(result.at("objects").at(2).at("pose").at("world_from_object").is_null());
	CHECK(result.at("dt_ns").at("denominator") == 60);
}

TEST_CASE(
	"temporal sample refuses stale revisions and duplicate requested ids", "[control][temporal-sample]"
) {
	Fixture fixture;
	fixture.Add("Camera", "Camera", "camera/main", CFrame{});
	fixture.Add("Part", "Part", "object/one", CFrame{});
	json request = fixture.SnapshotRequest({"object/one"});

	bool failed = false;
	json stale = request;
	stale["expected_world_version"] = stale["expected_world_version"].get<uint64_t>() + 1;
	const json staleReply = Call(fixture.Control, stale, failed);
	CHECK(failed);
	CHECK(staleReply.dump().find("version_conflict") != std::string::npos);

	request["object_ids"] = json::array({"object/one", "object/one"});
	const json duplicateReply = Call(fixture.Control, request, failed);
	CHECK(failed);
	CHECK(duplicateReply.dump().find("validation_failed") != std::string::npos);
}

TEST_CASE("temporal sample rejects a duplicated selected stable identity", "[control][temporal-sample]") {
	Fixture fixture;
	fixture.Add("Camera", "Camera", "camera/main", CFrame{});
	fixture.Add("Part", "First", "object/duplicate", CFrame{});
	fixture.Add("Part", "Second", "object/duplicate", CFrame{});
	const json request = fixture.SnapshotRequest({"object/duplicate"});

	bool failed = false;
	const json reply = Call(fixture.Control, request, failed);
	CHECK(failed);
	CHECK(reply.dump().find("identity_conflict") != std::string::npos);
}

TEST_CASE("temporal sample rejects zero and nonunit camera rotations", "[control][temporal-sample]") {
	for (const float rotationW : {0.0f, 1.01f}) {
		Fixture fixture;
		CFrame invalid;
		invalid.QuaternionW = rotationW;
		fixture.Add("Camera", "Camera", "camera/main", invalid);
		const json request = fixture.SnapshotRequest({});

		bool failed = false;
		const json reply = Call(fixture.Control, request, failed);
		CHECK(failed);
		CHECK(
			reply.dump().find("camera_id does not name a camera with a valid rigid world pose") !=
			std::string::npos
		);
	}
}

TEST_CASE(
	"temporal sample normalizes tolerated drift and marks invalid object rotations unavailable",
	"[control][temporal-sample]"
) {
	Fixture fixture;
	CFrame camera;
	camera.QuaternionW = 1.0005f;
	fixture.Add("Camera", "Camera", "camera/main", camera);
	CFrame invalidObject;
	invalidObject.QuaternionW = 0.0f;
	fixture.Add("Part", "Invalid", "object/invalid", invalidObject);
	const json request = fixture.SnapshotRequest({"object/invalid"});

	bool failed = false;
	const json reply = Call(fixture.Control, request, failed);
	INFO(reply.dump());
	CHECK_FALSE(failed);
	CHECK(reply.at("camera").at("world_from_camera").at("rotation").at(3) == Catch::Approx(1.0));
	const json &pose = reply.at("objects").at(0).at("pose");
	CHECK(pose.at("available") == false);
	CHECK(pose.at("reason") == "object_transform_unavailable");
	CHECK(pose.at("world_from_object").is_null());
}
