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

#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>

TEST_SUITE_ID("engine.render.interfacemesh")
TEST_DEPENDS("engine.render.glyphatlas")

using Catch::Approx;
using engine::core::Rect;
using engine::core::Vector2;
using engine::render::GlyphAtlas;
using engine::render::InterfaceBatch;
using engine::render::InterfaceMesh;
using engine::render::InterfaceScissor;
using engine::render::ScissorFor;

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

TEST_CASE("a join moves an outline's corners without changing its cost", "[render][interfacemesh]") {
	// **The three modes are one ring with its corner points moved**, so the
	// vertex and index counts must not move with the mode. That is the property
	// the implementation is built on, and a join that quietly grew the mesh
	// would make a stroke a performance decision.
	GlyphAtlas atlas;

	const auto ringFor = [&](engine::gui::LineJoin join) {
		engine::gui::DrawList list;
		auto outline = Rectangle(0.0f, 0.0f, 100.0f, 60.0f);
		outline.Kind = engine::gui::DrawKind::Outline;
		outline.Thickness = 4.0f;
		outline.CornerRadius = 12.0f;
		outline.Join = join;
		list.Commands.push_back(outline);

		InterfaceMesh mesh;
		mesh.Build(list, atlas);
		return mesh.Vertices();
	};

	const auto round = ringFor(engine::gui::LineJoin::Round);
	const auto bevel = ringFor(engine::gui::LineJoin::Bevel);
	const auto miter = ringFor(engine::gui::LineJoin::Miter);

	REQUIRE(round.size() == bevel.size());
	REQUIRE(round.size() == miter.size());
	CHECK(!round.empty());

	// How near the top-left corner of the box each mode gets. The radius is 12,
	// so a round corner stays a radius away along the diagonal and a mitred one
	// reaches the corner exactly.
	const auto nearestToOrigin = [](const std::vector<engine::render::InterfaceVertex> &vertices) {
		float best = std::numeric_limits<float>::max();
		for (const engine::render::InterfaceVertex &vertex : vertices) {
			best = std::min(best, std::hypot(vertex.X, vertex.Y));
		}
		return best;
	};

	// **Miter reaches the corner exactly**, which is the whole point of it.
	CHECK(nearestToOrigin(miter) == Approx(0.0f).margin(0.001f));

	// **Round stops a radius short along the diagonal**: the arc is centred at
	// (12, 12) with radius 12, so its closest approach to the origin is
	// `12 * sqrt(2) - 12`.
	CHECK(nearestToOrigin(round) == Approx(12.0f * std::sqrt(2.0f) - 12.0f).margin(0.01f));

	// **Bevel stops furthest short, not nearest**, and the arithmetic is the
	// reason: the chord runs from (0, 12) to (12, 0) and its midpoint is
	// (6, 6), at `6 * sqrt(2)`. An arc *bulges toward* the corner it rounds, so
	// a straight cut across it takes more off than the round does - which is
	// the opposite of what "bevel is a cheaper round" suggests, and is why the
	// three are asserted against arithmetic rather than against each other.
	CHECK(nearestToOrigin(bevel) == Approx(6.0f * std::sqrt(2.0f)).margin(0.01f));
	CHECK(nearestToOrigin(miter) < nearestToOrigin(round));
	CHECK(nearestToOrigin(round) < nearestToOrigin(bevel));
}

TEST_CASE("every join is the same shape at a square corner", "[render][interfacemesh]") {
	// A zero radius is already a sharp corner, so there is nothing for a join
	// to decide and all three must agree. Stated as a test because the
	// implementation reaches that case through an entirely different branch -
	// four rectangles rather than a ring - and a mode leaking into it would be
	// invisible until somebody set one.
	GlyphAtlas atlas;

	const auto squareFor = [&](engine::gui::LineJoin join) {
		engine::gui::DrawList list;
		auto outline = Rectangle(0.0f, 0.0f, 100.0f, 40.0f);
		outline.Kind = engine::gui::DrawKind::Outline;
		outline.Thickness = 2.0f;
		outline.Join = join;
		list.Commands.push_back(outline);

		InterfaceMesh mesh;
		mesh.Build(list, atlas);
		return mesh.Vertices();
	};

	const auto round = squareFor(engine::gui::LineJoin::Round);
	const auto miter = squareFor(engine::gui::LineJoin::Miter);
	REQUIRE(round.size() == miter.size());
	for (size_t index = 0; index < round.size(); index++) {
		CHECK(round[index].X == Approx(miter[index].X));
		CHECK(round[index].Y == Approx(miter[index].Y));
	}
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

TEST_CASE(
	"a shader change starts its own batch, the same rule a texture change follows", "[render][interfacemesh]"
) {
	GlyphAtlas atlas;
	engine::gui::DrawList list;

	auto plain = Rectangle(0.0f, 0.0f, 10.0f, 10.0f);
	plain.Kind = engine::gui::DrawKind::Image;
	plain.Image = engine::core::Name("rbxasset://textures/wall");
	list.Commands.push_back(plain);

	// Same image, same clip, same collector - only the shader differs, and
	// that alone has to be enough to end the run: a pipeline is bound per
	// batch, and these two want different ones.
	auto shaded = plain;
	shaded.Shader = engine::core::Name("toon");
	list.Commands.push_back(shaded);

	// A second command naming the same shader stays in the batch the first
	// one opened, rather than starting a third.
	list.Commands.push_back(shaded);

	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	REQUIRE(mesh.Batches().size() == 2);
	CHECK_FALSE(mesh.Batches()[0].Shader.IsValid());
	CHECK(mesh.Batches()[1].Shader == engine::core::Name("toon"));
	CHECK(mesh.Batches()[1].IndexCount == 12);
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

TEST_CASE("rounded fills and outlines are tessellated around their corners", "[render][interfacemesh]") {
	GlyphAtlas atlas;
	engine::gui::DrawList list;
	auto rounded = Rectangle(10.0f, 20.0f, 100.0f, 50.0f);
	rounded.CornerRadius = 12.0f;
	list.Commands.push_back(rounded);

	auto outline = rounded;
	outline.Kind = engine::gui::DrawKind::Outline;
	outline.Thickness = 3.0f;
	list.Commands.push_back(outline);

	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	CHECK(mesh.Vertices().size() > 20);
	CHECK(mesh.Indices().size() > 30);
	for (const auto &vertex : mesh.Vertices()) {
		CHECK(vertex.X >= 7.0f);
		CHECK(vertex.X <= 113.0f);
		CHECK(vertex.Y >= 17.0f);
		CHECK(vertex.Y <= 73.0f);
	}
}

TEST_CASE("spatial collectors never merge into one batch", "[render][interfacemesh]") {
	GlyphAtlas atlas;
	engine::gui::DrawList list;
	auto first = Rectangle(0.0f, 0.0f, 10.0f, 10.0f);
	first.Collector = engine::ecs::Entity{1};
	auto second = first;
	second.Collector = engine::ecs::Entity{2};
	list.Commands.push_back(first);
	list.Commands.push_back(second);

	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	REQUIRE(mesh.Batches().size() == 2);
	CHECK(mesh.Batches()[0].Collector == first.Collector);
	CHECK(mesh.Batches()[1].Collector == second.Collector);
}

TEST_CASE("an image bakes its flipbook cell into final UVs", "[render][interfacemesh]") {
	GlyphAtlas atlas;
	engine::gui::DrawList list;
	auto picture = Rectangle(0.0f, 0.0f, 100.0f, 50.0f);
	picture.Kind = engine::gui::DrawKind::Image;
	picture.Image = engine::core::Name("animated");
	list.Commands.push_back(picture);

	InterfaceMesh mesh;
	mesh.Build(list, atlas, [](const engine::core::Name &) {
		engine::render::InterfaceImageInfo info;
		info.Size = Vector2{100.0f, 50.0f};
		info.Cell.Scale = 0.25f;
		info.Cell.OffsetU = 0.5f;
		info.Cell.OffsetV = 0.25f;
		return info;
	});

	REQUIRE(mesh.Vertices().size() == 4);
	CHECK(mesh.Vertices()[0].U == Approx(0.5f));
	CHECK(mesh.Vertices()[0].V == Approx(0.25f));
	CHECK(mesh.Vertices()[2].U == Approx(0.75f));
	CHECK(mesh.Vertices()[2].V == Approx(0.5f));
}

TEST_CASE("viewport frames resolve by element and keep target extents", "[render][interfacemesh]") {
	GlyphAtlas atlas;
	engine::gui::DrawList list;
	auto viewport = Rectangle(0.0f, 0.0f, 160.0f, 90.0f);
	viewport.Kind = engine::gui::DrawKind::Viewport;
	viewport.Source = engine::ecs::Entity{77};
	list.Commands.push_back(viewport);

	InterfaceMesh mesh;
	mesh.Build(list, atlas, {}, [](engine::ecs::Entity source) {
		CHECK(source == engine::ecs::Entity{77});
		engine::render::InterfaceImageInfo info;
		info.Size = Vector2{160.0f, 90.0f};
		info.UVMax = Vector2{0.75f, 0.5f};
		return info;
	});

	REQUIRE(mesh.Batches().size() == 1);
	CHECK(mesh.Batches()[0].Viewport == viewport.Source);
	CHECK(mesh.Vertices()[0].U == Approx(0.0f));
	CHECK(mesh.Vertices()[0].V == Approx(0.0f));
	CHECK(mesh.Vertices()[2].U == Approx(0.75f));
	CHECK(mesh.Vertices()[2].V == Approx(0.5f));
}

TEST_CASE("text size alignment wrapping truncation and stroke affect geometry", "[render][interfacemesh]") {
	const StagedAssets assets;
	if (!std::filesystem::exists(engine::core::Paths::Assets() / "fonts" / "Inter.ttf")) {
		SUCCEED("no staged fonts");
		return;
	}

	GlyphAtlas atlas;
	REQUIRE(atlas.Build(16.0f));
	InterfaceMesh mesh;

	auto text = Rectangle(0.0f, 0.0f, 120.0f, 80.0f);
	text.Kind = engine::gui::DrawKind::Text;
	text.Text = "AB";
	text.TextSize = 16;
	text.XAlignment = engine::gui::TextXAlignment::Left;
	text.YAlignment = engine::gui::TextYAlignment::Top;
	engine::gui::DrawList list;
	list.Commands.push_back(text);
	mesh.Build(list, atlas);
	REQUIRE(mesh.Vertices().size() == 8);
	const float smallHeight = mesh.Vertices()[2].Y - mesh.Vertices()[0].Y;
	const float left = mesh.Vertices()[0].X;

	list.Commands[0].TextSize = 32;
	list.Commands[0].XAlignment = engine::gui::TextXAlignment::Right;
	mesh.Build(list, atlas);
	REQUIRE(mesh.Vertices().size() == 8);
	CHECK(mesh.Vertices()[2].Y - mesh.Vertices()[0].Y == Approx(smallHeight * 2.0f));
	CHECK(mesh.Vertices()[0].X > left);

	list.Commands[0] = text;
	list.Commands[0].Text = "A A A A";
	list.Commands[0].Bounds.Max.X = 24.0f;
	list.Commands[0].Wrapped = true;
	mesh.Build(list, atlas);
	REQUIRE(mesh.Vertices().size() == 16);
	CHECK(mesh.Vertices()[12].Y > mesh.Vertices()[0].Y);

	list.Commands[0] = text;
	list.Commands[0].Text = "A";
	list.Commands[0].StrokeTransparency = 0.0f;
	mesh.Build(list, atlas);
	CHECK(mesh.Vertices().size() == 36);

	list.Commands[0] = text;
	list.Commands[0].Text = "ABCDEFGHIJK";
	list.Commands[0].Bounds.Max.X = 30.0f;
	list.Commands[0].Truncate = engine::gui::TextTruncate::AtEnd;
	mesh.Build(list, atlas);
	CHECK(mesh.Vertices().size() < 44);
}

TEST_CASE("a two-stop gradient ramps a quad's own vertices", "[render][interfacemesh]") {
	// **Four vertices and no split, which is the case that has to stay cheap.**
	// A ramp with a stop at each end is one linear segment, and a linear
	// function interpolated across a triangle from exact vertex values is exact
	// - so subdividing would be work that changed no pixel.
	engine::gui::DrawList list;
	engine::gui::DrawCommand fill = Rectangle(0.0f, 0.0f, 100.0f, 40.0f);
	fill.Gradient = 0;
	list.Commands.push_back(fill);

	engine::gui::DrawGradient ramp;
	ramp.Color = engine::core::ColorSequence{
		engine::core::Color3{1.0f, 1.0f, 1.0f}, engine::core::Color3{0.0f, 0.0f, 0.0f}
	};
	ramp.Origin = Vector2{0.0f, 0.0f};
	ramp.Axis = Vector2{100.0f, 0.0f};
	list.Gradients.push_back(ramp);

	GlyphAtlas atlas;
	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	REQUIRE(mesh.Vertices().size() == 4);

	// The clipper winds a rectangle top-left, top-right, bottom-right,
	// bottom-left, so the two left-hand corners are the white end.
	CHECK(mesh.Vertices()[0].R == 255);
	CHECK(mesh.Vertices()[3].R == 255);
	CHECK(mesh.Vertices()[1].R == 0);
	CHECK(mesh.Vertices()[2].R == 0);
}

TEST_CASE("a three-stop gradient splits the shape at its keypoint", "[render][interfacemesh]") {
	// **The reason `PushShaded` exists.** A ramp that is white, then black, then
	// white again is two linear segments; one quad interpolated across both
	// would read the midpoint as white at every pixel of it, which is the
	// opposite of what was authored.
	engine::gui::DrawList list;
	engine::gui::DrawCommand fill = Rectangle(0.0f, 0.0f, 100.0f, 40.0f);
	fill.Gradient = 0;
	list.Commands.push_back(fill);

	engine::gui::DrawGradient ramp;
	ramp.Color.Add(engine::core::ColorKeypoint{0.0f, engine::core::Color3{1.0f, 1.0f, 1.0f}});
	ramp.Color.Add(engine::core::ColorKeypoint{0.5f, engine::core::Color3{0.0f, 0.0f, 0.0f}});
	ramp.Color.Add(engine::core::ColorKeypoint{1.0f, engine::core::Color3{1.0f, 1.0f, 1.0f}});
	ramp.Origin = Vector2{0.0f, 0.0f};
	ramp.Axis = Vector2{100.0f, 0.0f};
	list.Gradients.push_back(ramp);

	GlyphAtlas atlas;
	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	// Two bands, four corners each: the cut runs down the middle of the quad.
	REQUIRE(mesh.Vertices().size() == 8);

	bool sawSeam = false;
	for (const engine::render::InterfaceVertex &vertex : mesh.Vertices()) {
		if (vertex.X > 49.0f && vertex.X < 51.0f) {
			sawSeam = true;
			CHECK(vertex.R == 0);
		}
	}
	CHECK(sawSeam);
}

TEST_CASE("a gradient's transparency adds to what the command already has", "[render][interfacemesh]") {
	// Roblox's composition, and the half that is easy to get backwards: a
	// gradient is a modifier on what the element draws, so a half-faded element
	// under a ramp that fades to one ends up fully clear rather than half.
	engine::gui::DrawList list;
	engine::gui::DrawCommand fill = Rectangle(0.0f, 0.0f, 100.0f, 40.0f);
	fill.Transparency = 0.5f;
	fill.Gradient = 0;
	list.Commands.push_back(fill);

	engine::gui::DrawGradient ramp;
	ramp.Color = engine::core::ColorSequence{engine::core::Color3{1.0f, 1.0f, 1.0f}};
	ramp.Transparency = engine::core::NumberSequence{0.0f, 1.0f};
	ramp.Origin = Vector2{0.0f, 0.0f};
	ramp.Axis = Vector2{100.0f, 0.0f};
	list.Gradients.push_back(ramp);

	GlyphAtlas atlas;
	InterfaceMesh mesh;
	mesh.Build(list, atlas);

	REQUIRE(mesh.Vertices().size() == 4);
	CHECK(mesh.Vertices()[0].A == 128);
	CHECK(mesh.Vertices()[1].A == 0);
}

TEST_CASE("a clip is scaled from canvas units into the target's pixels", "[render][interfacemesh]") {
	// **The bug this exists for.** The studio lays a viewport's game interface
	// out against the panel's logical size - that is the space imgui reports
	// the pointer in - while the panel's texture is allocated at the display's
	// pixel density. A scissor taken straight from the clip therefore cut the
	// whole interface down to the top-left quarter of a 2x panel, while the
	// vertex shader went on stretching it across the whole panel.
	const engine::core::Rect clip{engine::core::Vector2{0.0f, 0.0f}, engine::core::Vector2{800.0f, 450.0f}};

	const InterfaceScissor doubled =
		ScissorFor(clip, engine::core::Vector2{800.0f, 450.0f}, engine::core::Vector2{1600.0f, 900.0f});
	CHECK(doubled.X == 0);
	CHECK(doubled.Y == 0);
	CHECK(doubled.Width == 1600);
	CHECK(doubled.Height == 900);

	// The ordinary display, where the two are the same number - which is why
	// this went unnoticed for as long as it did.
	const InterfaceScissor plain =
		ScissorFor(clip, engine::core::Vector2{800.0f, 450.0f}, engine::core::Vector2{800.0f, 450.0f});
	CHECK(plain.Width == 800);
	CHECK(plain.Height == 450);
}

TEST_CASE("an offset clip keeps its place under scaling", "[render][interfacemesh]") {
	// A nested frame's clip is not at the origin, so a scale applied to the size
	// and not to the origin would move every inner panel towards the top-left as
	// the density went up - which reads as the layout being wrong rather than
	// the scissor.
	const engine::core::Rect clip{
		engine::core::Vector2{100.0f, 50.0f}, engine::core::Vector2{300.0f, 250.0f}
	};

	const InterfaceScissor scaled =
		ScissorFor(clip, engine::core::Vector2{800.0f, 450.0f}, engine::core::Vector2{1600.0f, 900.0f});
	CHECK(scaled.X == 200);
	CHECK(scaled.Y == 100);
	CHECK(scaled.Width == 400);
	CHECK(scaled.Height == 400);
}

TEST_CASE("a clip is rounded outwards and never starts before the target", "[render][interfacemesh]") {
	// **Outwards, because the scissor is an early-out and the shader is the
	// clip.** A fraction of a pixel rounded inwards discards a fragment
	// `interface.frag` would have kept, which is a hairline of missing text
	// along a panel edge and nothing to point at.
	//
	// A clip left of the target comes from a frame positioned off-screen; a
	// negative scissor origin is a validation error on some backends and
	// silently ignored on others, which is the worst pair.
	const engine::core::Rect fractional{
		engine::core::Vector2{10.2f, 10.9f}, engine::core::Vector2{20.1f, 20.4f}
	};
	const InterfaceScissor rounded =
		ScissorFor(fractional, engine::core::Vector2{100.0f, 100.0f}, engine::core::Vector2{100.0f, 100.0f});
	CHECK(rounded.X == 10);
	CHECK(rounded.Y == 10);
	CHECK(rounded.X + rounded.Width == 21);
	CHECK(rounded.Y + rounded.Height == 21);

	const engine::core::Rect offscreen{
		engine::core::Vector2{-50.0f, -20.0f}, engine::core::Vector2{-10.0f, 30.0f}
	};
	const InterfaceScissor clamped =
		ScissorFor(offscreen, engine::core::Vector2{100.0f, 100.0f}, engine::core::Vector2{100.0f, 100.0f});
	CHECK(clamped.X == 0);
	CHECK(clamped.Y == 0);
	CHECK(clamped.Empty());
}

TEST_CASE("a canvas of zero leaves the clip alone", "[render][interfacemesh]") {
	// The frame before the first layout has no canvas, and a ratio against zero
	// is not a number. Taking the clip as pixels is the answer that draws
	// something rather than nothing.
	const engine::core::Rect clip{engine::core::Vector2{0.0f, 0.0f}, engine::core::Vector2{40.0f, 30.0f}};

	const InterfaceScissor none =
		ScissorFor(clip, engine::core::Vector2{0.0f, 0.0f}, engine::core::Vector2{1600.0f, 900.0f});
	CHECK(none.Width == 40);
	CHECK(none.Height == 30);

	const InterfaceScissor unsized =
		ScissorFor(clip, engine::core::Vector2{800.0f, 450.0f}, engine::core::Vector2{0.0f, 0.0f});
	CHECK(unsized.Width == 40);
	CHECK(unsized.Height == 30);
}
