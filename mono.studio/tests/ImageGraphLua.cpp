#include <engine/scripthost/ComposerLua.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/ImageGraph.hpp>
TEST_SUITE_ID("studio.imagegraph.lua")
TEST_DEPENDS("studio.imagegraph")
using namespace engine::imagegraph;
TEST_CASE(
	"Studio live Lua preview preserves frame scheduling and resets document lifetime", "[studio][lua]"
) {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"lua",
		 "pc.lua_compute",
		 "",
		 {},
		 {{"lua_code", std::string{"counter=(counter or 0)+1 return counter"}},
		  {"function_name", std::string{"renderFixture"}},
		  {"execute_on_frame", true}}}
	};
	document.Outputs = {{"result", "lua", "return_value"}};
	auto host = engine::script::MakeComposerLuaHost();
	REQUIRE(host);
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	studio::ImageGraphPreviewValue preview;
	EvaluationRequest request;
	request.HostProvider = host.get();
	for (const auto &[tick, expected] : {std::pair{0u, 1.0}, std::pair{0u, 1.0}, std::pair{1u, 2.0}}) {
		request.Tick = tick;
		const auto status =
			studio::EvaluateImageGraphPreview(document, plan, "result", request, preview, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		CHECK(std::get<double>(std::get<EvaluatedValue>(preview).Data) == expected);
	}
	host->Reset();
	request.Tick = 2;
	REQUIRE(
		studio::EvaluateImageGraphPreview(document, plan, "result", request, preview, error) == Status::Ok
	);
	CHECK(std::get<double>(std::get<EvaluatedValue>(preview).Data) == 1.0);
}
TEST_CASE("Studio Lua surface preview executes through the live host", "[studio][lua]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {
		{"lua",
		 "pc.lua_surface",
		 "",
		 {},
		 {{"lua_code", std::string{"clear() setColor(colorCreateRGB(255,0,0)) drawPixel(0,0)"}},
		  {"function_name", std::string{"renderFixture"}},
		  {"output_dimension", Vector2{2, 2}},
		  {"execute_on_frame", true}}}
	};
	document.Outputs = {{"result", "lua", "surface_out"}};
	auto host = engine::script::MakeComposerLuaHost();
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	EvaluationRequest request;
	request.HostProvider = host.get();
	studio::ImageGraphPreviewValue preview;
	const auto status = studio::EvaluateImageGraphPreview(document, plan, "result", request, preview, error);
	INFO(error.Message);
	REQUIRE(status == Status::Ok);
	const auto &image = std::get<Image>(preview);
	CHECK(image.Width == 2);
	CHECK(image.Height == 2);
	REQUIRE(image.Pixels.size() == 16);
	CHECK(image.Pixels[0] == uint8_t{255});
	CHECK(image.Pixels[3] == uint8_t{255});
	CHECK(image.Pixels[7] == uint8_t{0});
}
