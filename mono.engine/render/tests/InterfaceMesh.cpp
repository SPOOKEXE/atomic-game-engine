// A compiled interface, as triangles.
//
// **The half of the interface pass a device is not needed for**, which is why
// it is a type of its own rather than a loop inside a recording function: quad
// generation, outline construction, batching by scissor and text layout are all
// arithmetic, and arithmetic found wrong here is arithmetic not found by looking
// at a screen.

#include <engine/core/Paths.hpp>
#include <engine/render/InterfaceMesh.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

TEST_SUITE_ID("engine.render.interfacemesh")
TEST_DEPENDS("engine.render.glyphatlas")

using Catch::Approx;
using engine::core::Rect;
using engine::core::Vector2;
using engine::render::GlyphAtlas;
using engine::render::InterfaceBatch;
using engine::render::InterfaceMesh;

namespace {
	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base());
		}
		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	engine::gui::DrawCommand Rectangle(float x, float y, float width, float height) {
		engine::gui::DrawCommand command;
		command.Kind = engine::gui::DrawKind::Rectangle;
		command.Bounds = Rect{Vector2{x, y}, Vector2{x + width, y + height}};
		command.Clip = Rect{Vector2{0.0f, 0.0f}, Vector2{800.0f, 600.0f}};
		return command;
	}
}

TEST_CASE("a rectangle becomes one quad", "[render][interfacemesh]") {
	GlyphAtlas atlas;
	engine::gui::DrawList list;
	list.Commands.push_back(Rectangle(10.0f, 20.0f, 100.0f, 50.0f));

	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	REQUIRE(mesh.Vertices().size() == 4);
	REQUIRE(mesh.Indices().size() == 6);
	REQUIRE(mesh.Batches().size() == 1);

	CHECK(mesh.Vertices()[0].X == Approx(10.0f));
	CHECK(mesh.Vertices()[0].Y == Approx(20.0f));
	CHECK(mesh.Vertices()[2].X == Approx(110.0f));
	CHECK(mesh.Vertices()[2].Y == Approx(70.0f));

	CHECK(mesh.Batches()[0].IndexCount == 6);
	CHECK_FALSE(mesh.Batches()[0].Image.IsValid());
}

TEST_CASE("transparency is inverted into alpha exactly once", "[render][interfacemesh]") {
	// **`gui` spells it Roblox's way round, where 0 is opaque.** Converting at
	// each use rather than once is how one of them ends up written backwards,
	// and a panel drawn at inverted alpha reads as a theme problem rather than
	// as a bug in a renderer.
	GlyphAtlas atlas;
	engine::gui::DrawList list;

	auto opaque = Rectangle(0.0f, 0.0f, 10.0f, 10.0f);
	opaque.Transparency = 0.0f;
	list.Commands.push_back(opaque);

	auto invisible = Rectangle(0.0f, 0.0f, 10.0f, 10.0f);
	invisible.Transparency = 1.0f;
	list.Commands.push_back(invisible);

	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	REQUIRE(mesh.Vertices().size() == 8);
	CHECK(mesh.Vertices()[0].A == 255);
	CHECK(mesh.Vertices()[4].A == 0);
}

TEST_CASE("an outline is four quads and never a line primitive", "[render][interfacemesh]") {
	// A line's width is a device setting with no portable guarantee, so an
	// outline that is one pixel on one driver and three on another is the kind
	// of difference nobody reproduces.
	GlyphAtlas atlas;
	engine::gui::DrawList list;

	auto outline = Rectangle(0.0f, 0.0f, 100.0f, 40.0f);
	outline.Kind = engine::gui::DrawKind::Outline;
	outline.Thickness = 2.0f;
	list.Commands.push_back(outline);

	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	CHECK(mesh.Vertices().size() == 16);
	CHECK(mesh.Indices().size() == 24);

	// **A zero thickness still draws**, at one pixel, because an outline
	// somebody asked for and cannot see is indistinguishable from one that
	// failed.
	engine::gui::DrawList thin;
	auto hairline = outline;
	hairline.Thickness = 0.0f;
	thin.Commands.push_back(hairline);

	mesh.Build(thin, atlas);
	CHECK(mesh.Vertices().size() == 16);
}

TEST_CASE("batches split on a scissor change and merge otherwise", "[render][interfacemesh]") {
	// **A clip is pipeline state rather than a vertex attribute**, so two
	// elements clipped differently cannot be one draw however alike their pixels
	// are - and merging the runs that *do* match is the whole reason this is
	// called batched.
	GlyphAtlas atlas;
	engine::gui::DrawList list;

	list.Commands.push_back(Rectangle(0.0f, 0.0f, 10.0f, 10.0f));
	list.Commands.push_back(Rectangle(20.0f, 0.0f, 10.0f, 10.0f));

	auto clipped = Rectangle(40.0f, 0.0f, 10.0f, 10.0f);
	clipped.Clip = Rect{Vector2{0.0f, 0.0f}, Vector2{100.0f, 100.0f}};
	list.Commands.push_back(clipped);

	list.Commands.push_back(Rectangle(60.0f, 0.0f, 10.0f, 10.0f));

	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	// Two merged, then the clipped one, then back to the original clip.
	REQUIRE(mesh.Batches().size() == 3);
	CHECK(mesh.Batches()[0].IndexCount == 12);
	CHECK(mesh.Batches()[1].IndexCount == 6);
	CHECK(mesh.Batches()[2].IndexCount == 6);

	// The ranges tile the index buffer exactly, which is what a submission loop
	// depends on and what an off-by-one here would break silently.
	uint32_t expected = 0;
	for (const InterfaceBatch &batch : mesh.Batches()) {
		CHECK(batch.FirstIndex == expected);
		expected += batch.IndexCount;
	}
	CHECK(expected == mesh.Indices().size());
}

TEST_CASE("an image starts its own batch and carries its name", "[render][interfacemesh]") {
	// **A `core::Name` rather than a texture handle**, because resolving one is
	// the caller's - `gui::DrawCommand::Image` is a content name for exactly
	// that reason and this type is no better placed to resolve it.
	GlyphAtlas atlas;
	engine::gui::DrawList list;

	list.Commands.push_back(Rectangle(0.0f, 0.0f, 10.0f, 10.0f));

	auto picture = Rectangle(0.0f, 0.0f, 10.0f, 10.0f);
	picture.Kind = engine::gui::DrawKind::Image;
	picture.Image = engine::core::Name("rbxasset://textures/wall");
	list.Commands.push_back(picture);

	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	REQUIRE(mesh.Batches().size() == 2);
	CHECK_FALSE(mesh.Batches()[0].Image.IsValid());
	CHECK(mesh.Batches()[1].Image == engine::core::Name("rbxasset://textures/wall"));
}

TEST_CASE("text without an atlas draws nothing and breaks nothing", "[render][interfacemesh]") {
	// **What a client whose fonts failed to stage should show.** The interface
	// still draws and the text is visibly absent, rather than the frame being
	// abandoned over a missing .ttf.
	GlyphAtlas atlas;
	engine::gui::DrawList list;

	auto label = Rectangle(0.0f, 0.0f, 100.0f, 20.0f);
	label.Kind = engine::gui::DrawKind::Text;
	label.Text = "hello";
	list.Commands.push_back(label);
	list.Commands.push_back(Rectangle(0.0f, 40.0f, 10.0f, 10.0f));

	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	// The rectangle survived; the text produced nothing, and the empty batch it
	// would have made was dropped rather than submitted as a scissor change and
	// a pipeline bind for no triangles.
	CHECK(mesh.Vertices().size() == 4);
	REQUIRE(mesh.Batches().size() == 1);
	CHECK(mesh.Batches()[0].IndexCount == 6);
}

TEST_CASE("text with an atlas advances the pen per glyph", "[render][interfacemesh]") {
	const StagedAssets assets;
	if (!std::filesystem::exists(engine::core::Paths::Assets() / "fonts" / "Inter.ttf")) {
		SUCCEED("no staged fonts");
		return;
	}

	GlyphAtlas atlas;
	REQUIRE(atlas.Build(16.0f));

	engine::gui::DrawList list;
	auto label = Rectangle(0.0f, 0.0f, 200.0f, 20.0f);
	label.Kind = engine::gui::DrawKind::Text;
	label.Text = "AB";
	list.Commands.push_back(label);

	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	// Two glyphs, one quad each.
	REQUIRE(mesh.Vertices().size() == 8);

	// **The second starts to the right of the first**, which is the whole of
	// what a pen advance is and the one thing a text renderer gets wrong in a
	// way that looks like a font problem.
	CHECK(mesh.Vertices()[4].X > mesh.Vertices()[0].X);

	// Both sample inside the sheet rather than at its edge, which is what a
	// glyph whose UVs were computed against the wrong dimension would do.
	for (const auto &vertex : mesh.Vertices()) {
		CHECK(vertex.U >= 0.0f);
		CHECK(vertex.U <= 1.0f);
		CHECK(vertex.V >= 0.0f);
		CHECK(vertex.V <= 1.0f);
	}
}

TEST_CASE("a solid quad samples the atlas's white texel", "[render][interfacemesh]") {
	// **One pipeline for a filled rectangle and a glyph.** Two - one textured,
	// one not - is two places for the blend state to be set differently, which
	// shows as panels being subtly the wrong opacity and nowhere else.
	const StagedAssets assets;
	if (!std::filesystem::exists(engine::core::Paths::Assets() / "fonts" / "Inter.ttf")) {
		SUCCEED("no staged fonts");
		return;
	}

	GlyphAtlas atlas;
	REQUIRE(atlas.Build(16.0f));

	engine::gui::DrawList list;
	list.Commands.push_back(Rectangle(0.0f, 0.0f, 10.0f, 10.0f));

	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	const Vector2 white = InterfaceMesh::WhiteUV(atlas);
	REQUIRE(mesh.Vertices().size() == 4);
	for (const auto &vertex : mesh.Vertices()) {
		CHECK(vertex.U == Approx(white.X));
		CHECK(vertex.V == Approx(white.Y));
	}
}

TEST_CASE("a rotated command rotates its geometry", "[render][interfacemesh]") {
	// **`D00023`, closed.** `Element::Rotation` and `Resolved::AbsoluteRotation`
	// were correct and carried on every command since the tree went in, and no
	// backend used them: imgui's `AddText` walks an atlas and appends
	// axis-aligned quads with no transform, so a rotated label drew its box
	// turned and its contents upright.
	//
	// The entry's own reopen trigger was "the quad pipeline - a pass that emits
	// its own vertices can apply the rotation to all four kinds in one place".
	// This is that place, and this is that assertion.
	GlyphAtlas atlas;
	engine::gui::DrawList list;

	// A wide, short rectangle centred at (50, 20), turned a quarter turn. After
	// rotation it must be tall and narrow - a quad that ignored the angle stays
	// wide, and one that rotated about the wrong pivot moves off centre.
	auto turned = Rectangle(0.0f, 10.0f, 100.0f, 20.0f);
	turned.Rotation = 90.0f;
	list.Commands.push_back(turned);

	InterfaceMesh mesh;
	mesh.Build(list, atlas);
	REQUIRE(mesh.Vertices().size() == 4);

	float minX = 1e9f;
	float maxX = -1e9f;
	float minY = 1e9f;
	float maxY = -1e9f;
	for (const auto &vertex : mesh.Vertices()) {
		minX = std::min(minX, vertex.X);
		maxX = std::max(maxX, vertex.X);
		minY = std::min(minY, vertex.Y);
		maxY = std::max(maxY, vertex.Y);
	}

	// Swapped: 100 wide by 20 tall becomes 20 by 100.
	CHECK(maxX - minX == Approx(20.0f).margin(0.01f));
	CHECK(maxY - minY == Approx(100.0f).margin(0.01f));

	// **About its own centre**, which is what keeps a rotated element where the
	// layout put it. A rotation about the origin would fling it across the
	// canvas, and about a corner would slide it by half its size.
	CHECK((minX + maxX) * 0.5f == Approx(50.0f).margin(0.01f));
	CHECK((minY + maxY) * 0.5f == Approx(20.0f).margin(0.01f));
}

TEST_CASE("a rotated label turns as a run rather than per glyph", "[render][interfacemesh]") {
	// **The pivot is the element's centre and not each quad's**, which is the
	// one thing about rotating text that is easy to get wrong and unmistakable
	// when it is: per-quad rotation spins every letter on the spot and leaves
	// the run in a straight line.
	const StagedAssets assets;
	if (!std::filesystem::exists(engine::core::Paths::Assets() / "fonts" / "Inter.ttf")) {
		SUCCEED("no staged fonts");
		return;
	}

	GlyphAtlas atlas;
	REQUIRE(atlas.Build(16.0f));

	engine::gui::DrawList list;
	auto label = Rectangle(0.0f, 0.0f, 200.0f, 20.0f);
	label.Kind = engine::gui::DrawKind::Text;
	label.Text = "AB";
	label.Rotation = 90.0f;
	list.Commands.push_back(label);

	InterfaceMesh mesh;
	mesh.Build(list, atlas);
	REQUIRE(mesh.Vertices().size() == 8);

	// Unrotated the second glyph is to the *right* of the first. Turned a
	// quarter turn clockwise it must be *below* it instead - which only holds if
	// both glyphs turned about one shared pivot.
	const float firstY = mesh.Vertices()[0].Y;
	const float secondY = mesh.Vertices()[4].Y;
	CHECK(secondY > firstY);

	// And barely apart on X, because the run now goes down rather than across.
	CHECK(std::abs(mesh.Vertices()[4].X - mesh.Vertices()[0].X) < 12.0f);
}
