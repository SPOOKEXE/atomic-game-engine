#include <engine/imagegraph/ComposerLuaHost.hpp>
#include <engine/scriptluau/ComposerLua.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scriptluau.composer_lua")
TEST_DEPENDS("engine.imagegraph.document")

using namespace engine::imagegraph;
namespace {
	struct Invocation {
		Node Authored;
		EvaluationRequest Request;
		HostNodeCapture Output;
		std::string Failure;
		bool Run(ComposerLuaHost &host) {
			return host.Capture(
				{Authored, Request, Authored.Values, {}, Limits::MaximumEvaluationBytes}, Output, Failure
			);
		}
		const Value &ValueAt(std::string_view port) const {
			for (const auto &value : Output.Outputs)
				if (value.Port == port) return value.Data;
			FAIL("missing captured output");
			return Output.Outputs.front().Data;
		}
	};
	Invocation Compute(std::string code) {
		Invocation call;
		call.Authored = {"compute", "pc.lua_compute", "", {}, {{"lua_code", std::move(code)}}};
		return call;
	}
}

TEST_CASE(
	"Composer Lua executes source functions with typed arguments and nested results", "[composer_lua]"
) {
	auto host = engine::script::MakeLuauComposerHost();
	auto call = Compute("return {label=stringUpper(word), values={amount*2, Project.frame}, enabled=true}");
	call.Authored.DynamicInputs = {
		{"argument_name_0", ValueType::Text, {}},
		{"argument_type_0", ValueType::Enum, {}},
		{"argument_value_0", ValueType::Scalar, {}},
		{"argument_name_1", ValueType::Text, {}},
		{"argument_type_1", ValueType::Enum, {}},
		{"argument_value_1", ValueType::Text, {}}
	};
	call.Authored.Values.insert(
		call.Authored.Values.end(),
		{{"argument_name_0", std::string{"amount"}},
		 {"argument_value_0", 7.0},
		 {"argument_name_1", std::string{"word"}},
		 {"argument_value_1", std::string{"hello"}}}
	);
	call.Request.Tick = 4;
	call.Request.Subframe = .5;
	INFO(call.Failure);
	REQUIRE(call.Run(*host));
	const auto &result = std::get<StructValue>(call.ValueAt("return_value"));
	REQUIRE(result.Data);
	REQUIRE(result.Data->Fields.size() == 3);
	CHECK(result.Data->Fields[0].first == "enabled");
	CHECK(std::get<bool>(result.Data->Fields[0].second));
	CHECK(std::get<std::string>(result.Data->Fields[1].second) == "HELLO");
	const auto &values = std::get<ArrayValue>(result.Data->Fields[2].second);
	REQUIRE(values.Items.size() == 2);
	CHECK(std::get<double>(std::get<ElementValue>(values.Items[0].Data)) == 14);
	CHECK(std::get<double>(std::get<ElementValue>(values.Items[1].Data)) == 4.5);
}

TEST_CASE("Composer Lua global sessions and frame scheduling retain explicit host state", "[composer_lua]") {
	auto host = engine::script::MakeLuauComposerHost();
	Invocation global;
	global.Authored = {
		"globals",
		"pc.lua_global",
		"",
		{},
		{{"lua_code", std::string{"counter=10"}}, {"run_order", EnumValue{0}}}
	};
	REQUIRE(global.Run(*host));
	auto call = Compute("counter=counter+1 return counter");
	call.Authored.Values.push_back({"execution_thread", global.ValueAt("execution_thread")});
	call.Authored.Values.push_back({"execute_on_frame", false});
	REQUIRE(call.Run(*host));
	CHECK(std::get<double>(call.ValueAt("return_value")) == 11);
	call.Request.Tick = 1;
	REQUIRE(call.Run(*host));
	CHECK(std::get<double>(call.ValueAt("return_value")) == 11);
	call.Authored.Values.back().Data = true;
	REQUIRE(call.Run(*host));
	CHECK(std::get<double>(call.ValueAt("return_value")) == 12);
	call.Request.Tick = 2;
	REQUIRE(call.Run(*host));
	CHECK(std::get<double>(call.ValueAt("return_value")) == 13);
	host->Reset();
	CHECK_FALSE(call.Run(*host));
	CHECK(call.Failure.find("stale") != std::string::npos);
}

TEST_CASE("Composer Lua renders actual target pixels and returns supplied resources", "[composer_lua]") {
	auto host = engine::script::MakeLuauComposerHost();
	Invocation surface;
	surface.Authored = {
		"pixels",
		"pc.lua_surface",
		"",
		{},
		{{"output_dimension", Vector2{3, 2}},
		 {"lua_code",
		  std::string{"clear() setColor(colorCreateRGB(255,0,0)) drawPixel(1,0) "
					  "setColor(colorCreateRGB(0,255,0)) drawRect(0,1,3,2)"}}}
	};
	INFO(surface.Failure);
	REQUIRE(surface.Run(*host));
	REQUIRE(surface.Output.Images.size() == 1);
	const auto &image = surface.Output.Images[0].Data;
	SurfacePixel sample{};
	REQUIRE(LoadSurfacePixel(image, 1, 0, sample));
	CHECK(sample[0] == 1);
	CHECK(sample[1] == 0);
	CHECK(sample[3] == 1);
	REQUIRE(LoadSurfacePixel(image, 2, 1, sample));
	CHECK(sample[1] == 1);
	REQUIRE(LoadSurfacePixel(image, 0, 0, sample));
	CHECK(sample[3] == 0);
	auto call = Compute("return {surface=resource, bytes=data}");
	call.Authored.DynamicInputs = {
		{"argument_name_0", ValueType::Text, {}},
		{"argument_value_0", ValueType::Any, {}},
		{"argument_name_1", ValueType::Text, {}},
		{"argument_value_1", ValueType::Any, {}}
	};
	call.Authored.Values.insert(
		call.Authored.Values.end(),
		{{"argument_name_0", std::string{"resource"}},
		 {"argument_value_0", SurfaceValue{image}},
		 {"argument_name_1", std::string{"data"}},
		 {"argument_value_1", BufferValue{{1, 2, 3}}}}
	);
	INFO(call.Failure);
	REQUIRE(call.Run(*host));
	const auto &fields = std::get<StructValue>(call.ValueAt("return_value")).Data->Fields;
	REQUIRE(fields.size() == 2);
	CHECK(std::get<BufferValue>(fields[0].second).Bytes == std::vector<uint8_t>{1, 2, 3});
	CHECK(std::get<SurfaceValue>(fields[1].second).Data == image);
}

TEST_CASE("Composer Lua rejects excessive execution memory and cyclic return tables", "[composer_lua]") {
	SECTION("instruction budget") {
		ComposerLuaLimits limits;
		limits.MaximumSteps = 100;
		auto host = engine::script::MakeLuauComposerHost(limits);
		auto call = Compute("while true do end");
		CHECK_FALSE(call.Run(*host));
		CHECK(call.Failure.find("instruction") != std::string::npos);
	}
	SECTION("memory budget") {
		ComposerLuaLimits limits;
		limits.MaximumMemoryBytes = 8 * 1024 * 1024;
		auto host = engine::script::MakeLuauComposerHost(limits);
		auto call = Compute("return string.rep('x',32*1024*1024)");
		CHECK_FALSE(call.Run(*host));
		CHECK_FALSE(call.Failure.empty());
	}
	SECTION("cyclic return") {
		auto host = engine::script::MakeLuauComposerHost();
		auto call = Compute("local a={} a.self=a return a");
		CHECK_FALSE(call.Run(*host));
		CHECK(call.Failure.find("cyclic") != std::string::npos);
	}
	SECTION("native drawing budget") {
		ComposerLuaLimits limits;
		limits.MaximumPixelVisits = 3;
		auto host = engine::script::MakeLuauComposerHost(limits);
		Invocation call;
		call.Authored = {
			"surface",
			"pc.lua_surface",
			"",
			{},
			{{"output_dimension", Vector2{2, 2}}, {"lua_code", std::string{"clear()"}}}
		};
		CHECK_FALSE(call.Run(*host));
		CHECK(call.Failure.find("pixel budget") != std::string::npos);
	}
}

TEST_CASE("Composer Lua print messages retain bounded emission order", "[composer_lua]") {
	auto host = engine::script::MakeLuauComposerHost();
	auto call = Compute("print('first') print('second') return 0");
	REQUIRE(call.Run(*host));
	auto messages = host->TakeMessages();
	REQUIRE(messages.size() == 2);
	CHECK(messages[0].NodeId == "compute");
	CHECK(messages[0].Text == "first");
	CHECK(messages[1].Text == "second");
	CHECK(host->TakeMessages().empty());
}

TEST_CASE("Composer Lua host runs through graph evaluation without ambient capabilities", "[composer_lua]") {
	auto host = engine::script::MakeLuauComposerHost();
	Document document;
	document.Nodes.push_back(
		{"compute",
		 "pc.lua_compute",
		 "",
		 {},
		 {{"lua_code",
		   std::string{"return os==nil and io==nil and require==nil and game==nil and power(3,2)==9"}}}}
	);
	document.Outputs.push_back({"answer", "compute", "return_value"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	request.HostProvider = host.get();
	EvaluatedValue result;
	INFO(diagnostic.Message);
	REQUIRE(EvaluateValue(document, plan, "answer", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<bool>(result.Data));
}
