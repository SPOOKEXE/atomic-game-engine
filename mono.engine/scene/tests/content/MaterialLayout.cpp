#include <engine/scene/Components.hpp>
#include <engine/scene/content/MaterialLayout.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.materiallayout")

using namespace engine::scene;

TEST_CASE("a layout packs parameters without crossing a group of four", "[scene][material]") {
	const auto layout = ParseMaterialLayout(
		"#pragma parameter vec3 a = 1, 2, 3\n#pragma parameter float b\n#pragma parameter vec2 c\n"
		"#pragma parameter vec3 d\n#pragma texture t\n"
	);
	REQUIRE(layout.Error.empty());
	CHECK(layout.Find("a")->Offset == 0);
	CHECK(layout.Find("b")->Offset == 3);
	CHECK(layout.Find("c")->Offset == 4);
	CHECK(layout.Find("d")->Offset == 8);
	CHECK(layout.Find("a")->Default[2] == 3.0f);
	CHECK(layout.FindTexture("t") == 0);
	CHECK(layout.FindTexture("a") == -1);
	CHECK_FALSE(ParseMaterialLayout(
					"#pragma parameter vec4 a\n#pragma parameter vec4 b\n"
					"#pragma parameter vec4 c\n#pragma parameter vec4 d\n"
					"#pragma parameter float e\n"
	)
					.Error.empty());
	CHECK_FALSE(ParseMaterialLayout("#pragma parameter float a\n#pragma texture a\n").Error.empty());
}

TEST_CASE("material values survive their text and malformed text changes nothing", "[scene][material]") {
	const auto layout = ParseMaterialLayout("#pragma parameter vec2 a\n#pragma parameter float b\n");
	SurfaceAppearance appearance;
	const float pair[]{0.1f, -2.5e-7f};
	REQUIRE(SetMaterialParameter(appearance, *layout.Find("a"), pair, 2));
	CHECK_FALSE(SetMaterialParameter(appearance, *layout.Find("a"), pair, 1));
	const std::string text = FormatMaterialParameters(appearance);
	SurfaceAppearance loaded;
	REQUIRE(ParseMaterialParameters(text, loaded));
	CHECK(loaded.MaterialParameterMask == 3);
	CHECK(loaded.MaterialParameters[0] == pair[0]);
	CHECK(loaded.MaterialParameters[1] == pair[1]);
	for (const char *bad : {"16=1", "0=nan", "0=inf", "a=1", "0", "0=1;;x"}) {
		INFO(bad);
		CHECK_FALSE(ParseMaterialParameters(bad, loaded));
		CHECK(loaded.MaterialParameterMask == 3);
	}
	REQUIRE(SetMaterialParameter(loaded, *layout.Find("a"), nullptr, 0));
	CHECK(FormatMaterialParameters(loaded).empty());
	REQUIRE(ParseMaterialParameters("", appearance));
	CHECK(appearance.MaterialParameterMask == 0);
}
