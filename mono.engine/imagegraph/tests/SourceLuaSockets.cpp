#include "../src/SourceLuaSockets.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_lua_sockets")
using namespace engine::imagegraph;
TEST_CASE(
	"Lua dynamic argument declarations persist all four source socket types", "[imagegraph][lua_sockets]"
) {
	const std::array types{ValueType::Scalar, ValueType::Text, ValueType::Image, ValueType::Struct};
	for (const auto &type : {std::string{"pc.lua_compute"}, std::string{"pc.lua_surface"}})
		for (size_t mode = 0; mode < types.size(); ++mode) {
			Node node{
				"lua",
				type,
				"",
				{},
				{},
				{{"argument_name_0", ValueType::Text, Value{std::string{"arg"}}},
				 {"argument_type_0", ValueType::Enum, Value{EnumValue{int64_t(mode)}}},
				 {"argument_value_0", types[mode], std::nullopt}}
			};
			if (mode == 0) node.DynamicInputs[2].Default = Value{3.0};
			if (mode == 1) node.DynamicInputs[2].Default = Value{std::string{"value"}};
			CHECK(engine::imagegraph::detail::SourceLuaArgumentType(node, "argument_value_0") == types[mode]);
			Document doc;
			doc.FormatVersion = 9;
			doc.Nodes = {node};
			doc.Outputs = {{"out", "lua", type == "pc.lua_compute" ? "return_value" : "surface_out"}};
			Plan plan;
			Diagnostic diagnostic;
			const auto status = Compile(doc, plan, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(status == Status::Ok);
			Document parsed;
			REQUIRE(Read(Write(doc), parsed, diagnostic) == Status::Ok);
			CHECK(parsed == doc);
			doc.Nodes[0].DynamicInputs[2].Type = types[(mode + 1) % 4];
			CHECK(Compile(doc, plan, diagnostic) == Status::TypeMismatch);
		}
}
TEST_CASE(
	"Lua argument selector uses authored value before constructor default", "[imagegraph][lua_sockets]"
) {
	Node node{
		"lua",
		"pc.lua_compute",
		"",
		{},
		{{"argument_type_0", EnumValue{1}}},
		{{"argument_type_0", ValueType::Enum, Value{EnumValue{0}}},
		 {"argument_value_0", ValueType::Text, Value{std::string{"text"}}}}
	};
	CHECK(engine::imagegraph::detail::SourceLuaArgumentType(node, "argument_value_0") == ValueType::Text);
	node.Values[0].Data = EnumValue{99};
	CHECK(engine::imagegraph::detail::SourceLuaArgumentType(node, "argument_value_0") == ValueType(255));
	CHECK_FALSE(engine::imagegraph::detail::SourceLuaArgumentType(node, "argument_name_0"));
}
TEST_CASE(
	"Lua type changes retain prior numeric raw defaults and animated values", "[imagegraph][lua_sockets]"
) {
	const std::array types{ValueType::Scalar, ValueType::Text, ValueType::Image, ValueType::Struct};
	for (size_t mode = 0; mode < types.size(); ++mode) {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"lua",
			 "pc.lua_compute",
			 "",
			 {},
			 {},
			 {{"argument_name_0", ValueType::Text, Value{std::string{"arg"}}},
			  {"argument_type_0", ValueType::Enum, Value{EnumValue{int64_t(mode)}}},
			  {"argument_value_0", types[mode], Value{9.5}}}}
		};
		doc.Outputs = {{"out", "lua", "return_value"}};
		Plan plan;
		Diagnostic diagnostic;
		const auto code = Compile(doc, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(code == Status::Ok);
		EvaluationSnapshot snapshot;
		const auto status = EvaluateNodeInputs(doc, plan, "lua", {}, snapshot, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		auto value = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
			return v.Port == "argument_value_0";
		});
		REQUIRE(value != snapshot.Values().end());
		CHECK(std::get<double>(value->Data) == 9.5);
		doc.Keyframes = {{"lua", "argument_value_0", 0, 6.5, "source", KeyframeEase{}}};
		doc.Tracks = {{"lua", "argument_value_0", "hold", -1}};
		const auto keyed = Compile(doc, plan, diagnostic);
		INFO(mode);
		INFO(diagnostic.Message);
		REQUIRE(keyed == Status::Ok);
		REQUIRE(EvaluateNodeInputs(doc, plan, "lua", {}, snapshot, diagnostic) == Status::Ok);
		value = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
			return v.Port == "argument_value_0";
		});
		REQUIRE(value != snapshot.Values().end());
		CHECK(std::get<double>(value->Data) == 6.5);
		Document parsed;
		REQUIRE(Read(Write(doc), parsed, diagnostic) == Status::Ok);
		CHECK(parsed == doc);
	}
}
