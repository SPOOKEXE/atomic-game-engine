#include "../src/nodes/SourceShape3D.hpp"

#include "NodeHarness.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_shape_3d")
using namespace engine::imagegraph;
using namespace engine::imagegraph::detail;

namespace {
	struct Prepared {
		Node Authored{"shape", "pc.shape_3_d", "", {}, {}};
		EvaluationRequest Request;
		const CatalogueEntry *Entry = FindCatalogueEntry("pc.shape_3_d");
		NodeContext Context{Authored, *Entry, Request};
		SourceShape3DRecipe Recipe;
		Prepared() {
			for (const auto &input : Entry->Inputs)
				if (auto value = CatalogueDefault(input)) Context.Values.emplace_back(input.Id, *value);
		}
		void Set(std::string_view port, Value value) {
			for (auto &[existingPort, existingValue] : Context.Values)
				if (existingPort == port) {
					existingValue = std::move(value);
					return;
				}
			Context.Values.emplace_back(port, std::move(value));
		}
	};
}

TEST_CASE("Draw Shape 3D prepares pinned transform stack and camera", "[imagegraph][shape3d]") {
	Prepared prepared;
	prepared.Context.Project.SurfaceWidth = 100;
	prepared.Context.Project.SurfaceHeight = 80;
	prepared.Set("anchor", Vector3{1, 0, .5});
	prepared.Set("position", Vector2{.25, .75});
	REQUIRE(BuildSourceShape3DRecipe(prepared.Context, prepared.Recipe));
	const auto &recipe = prepared.Recipe;
	CHECK(recipe.Width == 100);
	CHECK(recipe.Height == 80);
	CHECK(recipe.Shape == SourceShape3DKind::Cube);
	CHECK((recipe.WorldStack[0].Value == Vector3{.25, 0, -.25}));
	CHECK((recipe.WorldStack[1].Value == Vector3{.25, -.25, 0}));
	CHECK(recipe.WorldStack[2].Kind == SourceShape3DTransformKind::RotateX);
	CHECK((recipe.WorldStack[2].Value == Vector3{30, 0, 0}));
	CHECK((recipe.WorldStack[4].Value == Vector3{0, 0, -45}));
	CHECK((recipe.WorldStack[5].Value == Vector3{-.25, .25, 0}));
	CHECK((recipe.WorldStack[6].Value == Vector3{.5, .5, .5}));
	CHECK((recipe.CameraEye == Vector3{0, 1, 0}));
	CHECK((recipe.CameraUp == Vector3{0, 0, -1}));
	CHECK(recipe.Near == 0);
	CHECK(recipe.Far == 2);
	REQUIRE(recipe.Colours.size() == 1);
	CHECK((std::get<Colour>(recipe.Colours.front()) == Colour{255, 255, 255, 255}));
	CHECK(prepared.Context.OutputValues.empty());
	CHECK(prepared.Context.OutputImages.empty());
}

TEST_CASE(
	"Draw Shape 3D resolves all pinned shape names and refuses malformed recipes", "[imagegraph][shape3d]"
) {
	for (const auto name :
		 {"Plane", "Cube", "Octahedron", "Cylinder", "Cone", "Capsule", "Sphere", "Cut Sphere", "Torus"}) {
		Prepared prepared;
		prepared.Set("shape", std::string(name));
		REQUIRE(BuildSourceShape3DRecipe(prepared.Context, prepared.Recipe));
	}
	for (const auto &[port, value] : std::initializer_list<std::pair<std::string_view, Value>>{
			 {"shape", std::string("Unknown")},
			 {"anchor", std::string("bad")},
			 {"view_range", Vector2{1, 1}},
			 {"side_2", Vector2{4097, 8}},
			 {"colors", ArrayValue{ValueType::Colour, {}}},
			 {"radius", Vector2{std::numeric_limits<double>::infinity(), 1}}
		 }) {
		Prepared prepared;
		prepared.Set(port, value);
		prepared.Recipe.Width = 99;
		CHECK_FALSE(BuildSourceShape3DRecipe(prepared.Context, prepared.Recipe));
		CHECK(prepared.Recipe.Width == 99);
		CHECK(prepared.Context.FailureCode != Status::Ok);
	}
}

TEST_CASE("Draw Shape 3D shader outputs retain distinct alpha and unclamped depth", "[imagegraph][shape3d]") {
	SourceShape3DRecipe recipe;
	SourceShape3DFragment fragment;
	REQUIRE(ShadeSourceShape3DFragment(
		recipe, {.8, .6, .4, .5}, {.5, .5, .5, .25}, {.5, 1, .5, .5}, .5, -.75, fragment
	));
	CHECK((fragment.Surface == std::array<double, 4>{.2, .3, .1, .0625}));
	CHECK((fragment.Depth == std::array<double, 4>{2, 2, 2, .5}));
	CHECK((fragment.RimNormal == std::array<double, 4>{.75, .75, .75, .5}));
	const auto before = fragment.Surface;
	recipe.ViewRange = {0, 0};
	CHECK_FALSE(ShadeSourceShape3DFragment(recipe, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, 1, 1, fragment));
	CHECK(fragment.Surface == before);
}

TEST_CASE("Draw Shape 3D authored recipe controls survive native saves", "[imagegraph][shape3d]") {
	Document document;
	document.FormatVersion = 7;
	document.Nodes = {
		{"shape",
		 "pc.shape_3_d",
		 "",
		 {},
		 {{"shape", std::string("Torus")},
		  {"side_2", Vector2{12, 6}},
		  {"radius", Vector2{.8, .2}},
		  {"rotation", Vector3{15, 25, 35}},
		  {"uv_position", Vector2{.125, -.25}},
		  {"uv_scale", Vector2{2, -1}},
		  {"view_range", Vector2{-.5, .5}},
		  {"array_texture", true},
		  {"colors", ArrayValue{ValueType::Colour, {Colour{1, 2, 3, 4}, Colour{5, 6, 7, 8}}}}}}
	};
	document.Outputs = {{"out", "shape", "surface_out"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	Prepared prepared;
	for (const auto &value : restored.Nodes.front().Values)
		prepared.Set(value.Port, value.Data);
	REQUIRE(BuildSourceShape3DRecipe(prepared.Context, prepared.Recipe));
	CHECK(prepared.Recipe.Shape == SourceShape3DKind::Torus);
	CHECK((prepared.Recipe.Sides == Vector2{12, 6}));
	CHECK((prepared.Recipe.WorldStack[4].Value == Vector3{0, 0, -35}));
	CHECK((prepared.Recipe.UVPosition == Vector2{.125, -.25}));
	CHECK((prepared.Recipe.UVScale == Vector2{2, -1}));
	CHECK(prepared.Recipe.ArrayTexture);
	CHECK(prepared.Recipe.Colours.size() == 2);
}
