#include "HookFixture.hpp"

#include <engine/assets/Animation.hpp>
#include <engine/control/Surface.hpp>
#include <engine/control/features/RigExport.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.control.rigexport")
TEST_DEPENDS("engine.script.rigexport")

TEST_CASE("rig export MCP tool validates selection and preserves data-rig shape", "[control][rigexport]") {
	engine::world::Universe worlds;
	engine::world::WorldSettings settings;
	settings.Name = engine::core::Name("rig-scene");
	const auto world = worlds.Create(settings);
	worlds.Enter(world, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneComponents();
		const auto rig = store.CreateInstance(engine::scene::PartClass(), "Rig");
		engine::ecs::AttributeValue id;
		id.Type = engine::ecs::PropertyType::String;
		id.String = "rig/one";
		REQUIRE(engine::ecs::SetAttribute(store, rig, engine::core::Name("DataFactoryId"), id));
		store.Set<engine::scene::Skeleton>(rig, {engine::core::Name("one"), 1});
		engine::scene::Visual visual;
		visual.Mesh = engine::core::Name("rig/one.amesh");
		store.Set(rig, visual);
		engine::scene::MeshSkinning skinning;
		skinning.JointCount = 1;
		skinning.VertexCount = 1;
		skinning.Vertices = {{{0, 0, 0, 0}, {65535, 0, 0, 0}}};
		REQUIRE(engine::scene::RecordMesh(store, visual.Mesh, 1, {}, skinning));
		const auto bone = store.CreateInstance(engine::scene::BoneClass(), "Bone");
		REQUIRE(store.SetParent(bone, rig));
		store.Set(bone, engine::scene::Bone{});
		const auto point = store.CreateInstance(engine::scene::RigKeypointClass(), "Point");
		REQUIRE(store.SetParent(point, rig));
		engine::scene::RigKeypoint keypoint;
		keypoint.Keypoint = engine::core::Name("tip");
		keypoint.Joint = 0;
		store.Set(point, keypoint);
		const auto buffer =
			store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("AnimationBuffer")), "Buffer");
		engine::assets::AnimationData animation;
		animation.Duration = store.Time().Delta;
		animation.Channels = {{{0, {{0.0f, {}}, {animation.Duration, {}}}}}};
		engine::core::ByteWriter writer;
		REQUIRE(engine::assets::Animation::Write(writer, animation));
		store.Set(
			buffer,
			engine::scene::AnimationBuffer{std::vector(writer.Bytes().begin(), writer.Bytes().end()), 1}
		);
		const auto clip =
			store.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Animation")), "Clip");
		id.String = "clip/one";
		REQUIRE(engine::ecs::SetAttribute(store, clip, engine::core::Name("DataFactoryId"), id));
		store.Set(clip, engine::scene::AnimationClip{{}, engine::core::Name("one"), buffer});
	});
	engine::control::Surface surface("test", "test");
	engine::control::test::Install(surface, std::array{engine::control::test::Discovery()});
	engine::control::test::Install(surface, std::array{engine::control::test::RigExport(worlds)});
	const nlohmann::json discoveryRequest{
		{"jsonrpc", "2.0"},
		{"id", 0},
		{"method", "tools/call"},
		{"params", {{"name", "negotiate"}, {"arguments", nlohmann::json::object()}}}
	};
	const nlohmann::json discoveryReply = nlohmann::json::parse(surface.Answer(discoveryRequest.dump()));
	const nlohmann::json discovery =
		nlohmann::json::parse(discoveryReply["result"]["content"][0]["text"].get<std::string>());
	CHECK(
		std::any_of(
			discovery["operations"].begin(),
			discovery["operations"].end(),
			[](const nlohmann::json &operation) { return operation["name"] == "get_rig_export"; }
		)
	);
	const nlohmann::json request{
		{"jsonrpc", "2.0"},
		{"id", 1},
		{"method", "tools/call"},
		{"params",
		 {{"name", "get_rig_export"},
		  {"arguments",
		   {{"instance_id", "rig-scene"},
			{"export_id", "export/one"},
			{"options", nlohmann::json::object()}}}}}
	};
	const nlohmann::json reply = nlohmann::json::parse(surface.Answer(request.dump()));
	const nlohmann::json payload =
		nlohmann::json::parse(reply["result"]["content"][0]["text"].get<std::string>());
	CHECK_FALSE(reply["result"].value("isError", true));
	CHECK(payload["schema"] == "data-rig/v1");
	CHECK(payload["tick_seconds_numerator"].is_number_unsigned());
	CHECK(payload["tick_seconds_denominator"].is_number_unsigned());
	CHECK(payload["entities"][0]["joints"][0]["slot"].is_number_unsigned());
	CHECK(payload["entities"][0]["joints"][0]["rest_frame"].contains("rotation_xyzw"));
	const nlohmann::json &keypoints = payload["entities"][0]["keypoints"];
	REQUIRE(keypoints.is_array());
	REQUIRE(keypoints.size() == 1);
	CHECK(keypoints[0].size() == 5);
	CHECK(keypoints[0].contains("keypoint_id"));
	CHECK(keypoints[0].contains("name"));
	CHECK(keypoints[0]["state"] == "present");
	CHECK(keypoints[0]["position"].is_array());
	CHECK(keypoints[0]["missing_reason"].is_null());
	const nlohmann::json &skinning = payload["entities"][0]["skinning"];
	CHECK(skinning["available"] == true);
	CHECK(skinning["unavailable_reason"].is_null());
	CHECK(skinning["mesh_id"] == "rig/one.amesh");
	CHECK(skinning["position_space"] == "mesh_object");
	CHECK(skinning["joint_index_space"] == "skeleton_palette_slot");
	CHECK(skinning["weight_encoding"] == "uint16_unorm");
	CHECK(skinning["weight_denominator"] == 65535);
	REQUIRE(skinning["vertices"].size() == 1);
	CHECK(skinning["vertices"][0]["influences"][0]["joint_id"] == "rig/one:joint:0");
	CHECK(skinning["vertices"][0]["influences"][0]["weight"] == 65535);
	const nlohmann::json &clips = payload["entities"][0]["clips"];
	REQUIRE(clips.is_array());
	REQUIRE(clips.size() == 1);
	CHECK(clips[0]["start_tick"].is_number_unsigned());
	CHECK(clips[0]["end_tick"].is_number_unsigned());
	const nlohmann::json &channels = clips[0]["channels"];
	REQUIRE(channels.is_array());
	REQUIRE(channels.size() == 2);
	CHECK(channels[0]["property"] == "rotation");
	CHECK(channels[0]["keys"].is_array());
	REQUIRE(channels[0]["keys"].size() == 2);
	CHECK(channels[0]["joint_slot"].is_number_unsigned());
	CHECK(channels[0]["keys"][1]["time"]["tick"].is_number_unsigned());
	CHECK(channels[0]["keys"][1]["time"]["seconds_numerator"].is_number_unsigned());
	CHECK(channels[0]["keys"][1]["time"]["seconds_denominator"].is_number_unsigned());
	CHECK(channels[0]["keys"][1]["value"].is_array());
	CHECK(channels[0]["keys"][1]["value"].size() == 4);
	const auto tool =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &item) {
			return item.Name == "get_rig_export";
		});
	REQUIRE(tool != surface.Registered().end());
	const nlohmann::json schema = tool->Schema();
	CHECK(schema["properties"]["options"]["properties"].contains("entity_ids"));
	CHECK(schema["properties"]["options"]["properties"]["limit"]["maximum"] == 256);

	engine::script::ScriptValue tooLarge(engine::script::ValueTag::Number);
	tooLarge.Number = 18'446'744'073'709'551'616.0;
	nlohmann::json converted;
	CHECK_FALSE(engine::control::rig_export_detail::Convert(tooLarge, converted, 0, "slot"));
}

TEST_CASE("rig export response cap becomes a compact Surface refusal", "[control][rigexport]") {
	engine::control::Surface surface("test", "test");
	engine::script::ScriptValue enormous(engine::script::ValueTag::String);
	enormous.Text.assign(engine::control::rig_export_detail::MAXIMUM_BYTES + 1, 'x');
	engine::control::test::Install(
		surface,
		std::array{engine::control::test::Custom(
			"rig-export-overflow", [&enormous](engine::control::Surface &owner) {
				owner.Add(
					engine::control::Tool{
						"get_rig_export",
						"test overflow",
						nullptr,
						[enormous](const nlohmann::json &, std::string &failure) mutable -> nlohmann::json {
							nlohmann::json output;
							if (!engine::control::rig_export_detail::Result(enormous, output)) {
								failure = "rig export exceeds the 4 MiB response limit";
								return nullptr;
							}
							return output;
						}
					}
				);
			}
		)}
	);
	const nlohmann::json request{
		{"jsonrpc", "2.0"},
		{"id", 1},
		{"method", "tools/call"},
		{"params", {{"name", "get_rig_export"}, {"arguments", nlohmann::json::object()}}}
	};
	const std::string wire = surface.Answer(request.dump());
	const nlohmann::json reply = nlohmann::json::parse(wire);
	CHECK(reply["result"].value("isError", false));
	CHECK(wire.size() < 1024);
}

TEST_CASE(
	"factory rig exports resolve through the owning session before world lookup",
	"[control][rigexport][data-factory]"
) {
	engine::world::Universe owned;
	engine::world::Universe decoy;
	engine::world::DataFactorySession session(owned);
	session.SetPauseParticipant(
		[](engine::world::WorldId, engine::world::DataFactoryPauseScope, bool, std::string &) { return true; }
	);
	const engine::world::DataFactoryWorldRequest create{
		.Operation = engine::world::DataFactoryWorldOperation::Create,
		.InstanceId = "rig-fence",
		.TickRate = 60.0,
		.OperationId = "rig-fence-create",
	};
	REQUIRE(session.CreateWorld(create).Status == engine::world::DataFactoryStatus::Ok);
	engine::world::WorldSettings decoySettings;
	decoySettings.Name = engine::core::Name("rig-fence");
	REQUIRE(decoy.Create(decoySettings).IsValid());
	const auto ownedWorld = owned.Find(engine::core::Name("rig-fence"));
	owned.Enter(ownedWorld, [](engine::ecs::Store &store) {
		engine::scene::RegisterSceneComponents();
		const auto rig = store.CreateInstance(engine::scene::PartClass(), "Rig");
		engine::ecs::AttributeValue id;
		id.Type = engine::ecs::PropertyType::String;
		id.String = "rig/fenced";
		REQUIRE(engine::ecs::SetAttribute(store, rig, engine::core::Name("DataFactoryId"), id));
		store.Set<engine::scene::Skeleton>(rig, {engine::core::Name("fenced"), 0});
	});
	engine::control::Surface surface("test", "test");
	engine::control::test::Install(surface, std::array{engine::control::test::RigExport(decoy, &session)});
	const auto current = session.Inspect("rig-fence");
	const auto tool =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &item) {
			return item.Name == "get_rig_export";
		});
	REQUIRE(tool != surface.Registered().end());
	std::string failure;
	const nlohmann::json result = tool->Call(
		{{"instance_id", "rig-fence"},
		 {"export_id", "fence"},
		 {"options",
		  {{"expected_tick", current.Clock.Tick},
		   {"expected_world_epoch", current.WorldEpoch},
		   {"expected_world_version", current.WorldVersion}}}},
		failure
	);
	INFO(result.dump());
	CHECK_FALSE(failure.empty());
	CHECK(failure.find("no scene called") == std::string::npos);
	CHECK_FALSE(result.is_null());

	const size_t namesBeforeHostileRead = engine::core::Name::Count();
	failure.clear();
	const nlohmann::json hostile = tool->Call(
		{{"instance_id", "hostile-rig-world"},
		 {"export_id", "fence"},
		 {"options",
		  {{"expected_tick", current.Clock.Tick},
		   {"expected_world_epoch", current.WorldEpoch},
		   {"expected_world_version", current.WorldVersion}}}},
		failure
	);
	CHECK(failure.find("not owned") != std::string::npos);
	CHECK(hostile.at("status") == "validation_failed");
	CHECK(engine::core::Name::Count() == namesBeforeHostileRead);
	CHECK_FALSE(engine::core::Name::Exists("hostile-rig-world"));
}

TEST_CASE("compatibility rig exports reject factory revision fields", "[control][rigexport]") {
	engine::world::Universe worlds;
	engine::world::WorldSettings settings;
	settings.Name = engine::core::Name("compat-rig");
	REQUIRE(worlds.Create(settings).IsValid());
	engine::control::Surface surface("test", "test");
	engine::control::test::Install(surface, std::array{engine::control::test::RigExport(worlds)});
	const auto tool =
		std::find_if(surface.Registered().begin(), surface.Registered().end(), [](const auto &item) {
			return item.Name == "get_rig_export";
		});
	REQUIRE(tool != surface.Registered().end());
	CHECK_FALSE(tool->Schema()["properties"]["options"]["properties"].contains("expected_tick"));
	std::string failure;
	const nlohmann::json result = tool->Call(
		{{"instance_id", "compat-rig"}, {"export_id", "compat"}, {"options", {{"expected_tick", 0}}}}, failure
	);
	CHECK(result.is_null());
	CHECK(failure.find("unknown option") != std::string::npos);
}
