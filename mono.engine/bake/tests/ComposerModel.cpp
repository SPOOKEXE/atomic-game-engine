#include <engine/bake/ComposerModel.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
TEST_SUITE_ID("engine.bake.composer_model")
namespace {
	auto Bytes(std::string_view text) {
		return std::as_bytes(std::span(text.data(), text.size()));
	}
}
TEST_CASE(
	"Composer OBJ preserves dummy-origin centering, source winding and axis mapping", "[bake][composer]"
) {
	engine::bake::ComposerModel model;
	std::string failure;
	const std::string text = "v 1 0 0\nv 3 0 0\nv 1 2 0\nvt 0.2 0.3\nvt 0.8 0.3\nvt 0.2 0.9\nvn 0 0 "
							 "1\nusemtl named\nf 1/1/1 2/2/1 3/3/1\n";
	REQUIRE(engine::bake::ReadComposerObj(Bytes(text), 1, 2, model, failure));
	REQUIRE(model.Parts.size() == 1);
	REQUIRE(model.Parts[0].Vertices.size() == 3);
	CHECK(model.Parts[0].Material == "named");
	const auto &v = model.Parts[0].Vertices;
	CHECK(v[0].Position[0] == Catch::Approx(-0.25));
	CHECK(v[0].Position[2] == Catch::Approx(-0.5));
	CHECK(v[1].Position[2] == Catch::Approx(1.5));
	CHECK(v[2].Position[0] == Catch::Approx(1.75));
	CHECK(v[0].Normal[1] == Catch::Approx(-4));
	CHECK(v[0].TexCoord[1] == Catch::Approx(0.3));
	REQUIRE(model.Edges.size() == 3);
	CHECK_FALSE(engine::bake::ReadComposerObj(Bytes("v 0 0 0\nf 1 2 3\n"), 1, 0, model, failure));
	CHECK_FALSE(engine::bake::ReadComposerObj(Bytes(text), 1, 3, model, failure));
	CHECK_FALSE(engine::bake::ReadComposerObj(Bytes(text), 1, 0, model, failure, 1));
}
TEST_CASE("Composer element JSON preserves face material, UV and nested transforms", "[bake][composer]") {
	engine::bake::ComposerModel model;
	std::string failure;
	const std::string text =
		R"({"textureWidth":16,"textureHeight":16,"elements":[{"from":[1,2,3],"to":[3,4,5],"faces":{"up":{"texture":"#wood","uv":[0,0,16,16]}},"children":[{"from":[0,0,0],"to":[1,1,1],"faces":{"south":{"texture":"#glass","uv":[0,0,8,8]}}}]}]})";
	REQUIRE(engine::bake::ReadComposerElementJson(Bytes(text), 1, 0, model, failure));
	REQUIRE(model.Parts.size() == 2);
	CHECK(model.Parts[0].Material == "wood");
	CHECK(model.Parts[1].Material == "glass");
	const auto &up = model.Parts[0].Vertices;
	REQUIRE(up.size() == 6);
	CHECK(up[0].Position[0] == 0);
	CHECK(up[0].Position[1] == 2);
	CHECK(up[0].Position[2] == 0);
	CHECK(up[1].TexCoord[0] == 1);
	CHECK(up[1].TexCoord[1] == 1);
	REQUIRE(model.Parts[0].LocalMatrix);
	CHECK((*model.Parts[0].LocalMatrix)[12] == 1);
	CHECK((*model.Parts[0].LocalMatrix)[13] == 2);
	CHECK((*model.Parts[0].LocalMatrix)[14] == 3);
	REQUIRE(model.Parts[1].LocalMatrix);
	CHECK((*model.Parts[1].LocalMatrix)[12] == 1);
	CHECK((*model.Parts[1].LocalMatrix)[13] == 2);
	CHECK((*model.Parts[1].LocalMatrix)[14] == 3);
	const auto &child = model.Parts[1].Vertices;
	CHECK(child[0].Position[0] == 0);
	CHECK(child[0].Position[1] == 0);
	CHECK(child[0].Position[2] == 1);
	CHECK_FALSE(
		engine::bake::ReadComposerElementJson(
			Bytes(
				R"({"elements":[{"from":[0,0,0],"to":[1,1,1],"faces":{"up":{"texture":"#x","uv":[0,0]}}}]})"
			),
			1,
			0,
			model,
			failure
		)
	);
	CHECK_FALSE(engine::bake::ReadComposerElementJson(Bytes(R"({"elements":[]})"), 1, 0, model, failure));
}
