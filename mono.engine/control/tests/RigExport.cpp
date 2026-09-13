#include <engine/control/Surface.hpp>
#include <engine/control/features/RigExport.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
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
		const auto bone = store.CreateInstance(engine::scene::BoneClass(), "Bone");
		REQUIRE(store.SetParent(bone, rig));
		store.Set(bone, engine::scene::Bone{});
	});
	engine::control::Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::RigExport(worlds)});
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
	CHECK(payload["entities"][0]["skinning"]["unavailable_reason"].is_string());
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
	surface.Add(
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
