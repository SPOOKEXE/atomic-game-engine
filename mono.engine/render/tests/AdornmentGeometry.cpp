// Adornments, resolved into lines.
//
// **The half `gui` could not do**, and the reason is `D00022`'s: an adornment
// says what to outline and turning that into geometry needs the adornee's
// `CFrame` and stud extent, which are `scene`'s. `gui` links neither. This
// module links both, which is what "whoever draws one has both operands" meant.
//
// Headless, because resolving an adornment is a transform and twelve corners.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Adornments.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/AdornmentGeometry.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

TEST_SUITE_ID("engine.render.adornmentgeometry")
TEST_DEPENDS("engine.gui.adornments")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::render::AdornmentGeometry;
using engine::render::AdornmentLine;

namespace {
	struct World {
		Store Data;
		Entity Workspace;

		explicit World(std::string_view name) : Data(name) {
			engine::gui::RegisterGuiClasses();
			(void)engine::scene::PartClass();
			Workspace = Data.CreateInstance(
				engine::ecs::Classes::Find(engine::core::Name("Instance")),
				std::string(engine::gui::WORKSPACE)
			);
		}

		Entity Part(const Vector3 &position, const Vector3 &halfExtent) {
			const Entity part = Data.CreateInstance(engine::scene::PartClass(), "Rock");
			Data.SetParent(part, Workspace);
			Data.Set(part, engine::scene::Transform{CFrame(position)});
			Data.Set(part, engine::scene::Bounds{halfExtent});
			return part;
		}

		Entity Adorn(const char *klass, Entity parent) {
			const Entity made = Data.CreateInstance(engine::gui::GuiClass(klass), klass);
			Data.SetParent(made, parent);
			return made;
		}
	};
}

TEST_CASE("a selection box is the twelve edges of its adornee", "[render][adornmentgeometry]") {
	World world("adornment_geometry.box");

	const Entity part = world.Part(Vector3{10.0f, 0.0f, 0.0f}, Vector3{1.0f, 2.0f, 3.0f});
	world.Adorn("SelectionBox", part);

	AdornmentGeometry geometry;
	geometry.Build(world.Data);

	// Twelve edges, not six faces and not eight corners — a box drawn as lines
	// has twelve, and a count that came out 8 or 24 is a table written wrong.
	REQUIRE(geometry.Lines().size() == 12);

	// Every endpoint sits on the part's own corners, swollen a hair so the
	// outline is not coplanar with the surface. Checked as a bound rather than
	// exactly, because the swell is the renderer's business and not this
	// assertion's.
	for (const AdornmentLine &line : geometry.Lines()) {
		for (const Vector3 &point : {line.From, line.To}) {
			CHECK(std::abs(point.X - 10.0f) == Approx(1.0f).margin(0.01f));
			CHECK(std::abs(point.Y) == Approx(2.0f).margin(0.01f));
			CHECK(std::abs(point.Z) == Approx(3.0f).margin(0.01f));
		}
	}

	// **Swollen, not exact.** Coplanar edges z-fight along every side, which
	// makes a selection box flicker and reads as a driver fault.
	const float reach = std::max({
		std::abs(geometry.Lines()[0].From.Y),
		std::abs(geometry.Lines()[0].To.Y),
	});
	CHECK(reach > 2.0f);
}

TEST_CASE("a rotated part gets a rotated box", "[render][adornmentgeometry]") {
	// **Through the adornee's own frame**, which is the whole reason the corners
	// are transformed rather than built in world space from a position and an
	// extent. An axis-aligned box around a tilted object reads as the selection
	// being wrong rather than the renderer.
	World world("adornment_geometry.rotated");

	const Entity part = world.Part(Vector3::Zero, Vector3{2.0f, 0.5f, 0.5f});
	world.Data.Set(part, engine::scene::Transform{CFrame::Angles(0.0f, 1.5707963f, 0.0f)});
	world.Adorn("SelectionBox", part);

	AdornmentGeometry geometry;
	geometry.Build(world.Data);
	REQUIRE(geometry.Lines().size() == 12);

	// Rotated a quarter turn about Y, so the long axis is now Z. A box that
	// ignored rotation would still reach 2 on X.
	float reachX = 0.0f;
	float reachZ = 0.0f;
	for (const AdornmentLine &line : geometry.Lines()) {
		reachX = std::max(reachX, std::abs(line.From.X));
		reachZ = std::max(reachZ, std::abs(line.From.Z));
	}

	CHECK(reachZ > 1.9f);
	CHECK(reachX < 0.6f);
}

TEST_CASE("a handle uses its own size and offset", "[render][adornmentgeometry]") {
	World world("adornment_geometry.handle");

	const Entity part = world.Part(Vector3::Zero, Vector3{5.0f, 5.0f, 5.0f});
	const Entity handle = world.Adorn("BoxHandleAdornment", part);

	engine::gui::HandleShape shape;
	shape.Offset = CFrame(Vector3{0.0f, 10.0f, 0.0f});
	shape.Size = Vector3{1.0f, 1.0f, 1.0f};
	world.Data.Set(handle, shape);

	AdornmentGeometry geometry;
	geometry.Build(world.Data);
	REQUIRE(geometry.Lines().size() == 12);

	// **The handle's size, not the adornee's**, which is the distinction that
	// makes a gizmo a grab target rather than a second selection box. Offset ten
	// up and half a unit across.
	for (const AdornmentLine &line : geometry.Lines()) {
		CHECK(std::abs(line.From.Y - 10.0f) == Approx(0.5f).margin(0.01f));
		CHECK(std::abs(line.From.X) == Approx(0.5f).margin(0.01f));
	}
}

TEST_CASE("an adornee with no transform produces nothing", "[render][adornmentgeometry]") {
	// **A box around the origin is worse than no box.** A `Folder` can legally
	// be an `Adornee`, and outlining it at (0,0,0) looks like a bug in the
	// selection rather than an adornee that has no position.
	World world("adornment_geometry.notransform");

	const Entity folder = world.Data.CreateInstance(
		engine::ecs::Classes::Find(engine::core::Name("Instance")), "Group"
	);
	world.Data.SetParent(folder, world.Workspace);
	world.Adorn("SelectionBox", folder);

	AdornmentGeometry geometry;
	geometry.Build(world.Data);
	CHECK(geometry.Lines().empty());
}

TEST_CASE("colour and depth mode travel to every line", "[render][adornmentgeometry]") {
	World world("adornment_geometry.style");

	const Entity part = world.Part(Vector3::Zero, Vector3{1.0f, 1.0f, 1.0f});
	const Entity box = world.Adorn("SelectionBox", part);

	engine::gui::Adornment state;
	state.Color = engine::core::Color3{1.0f, 0.0f, 0.5f};
	state.Transparency = 0.25f;
	state.AlwaysOnTop = false;
	world.Data.Set(box, state);

	AdornmentGeometry geometry;
	geometry.Build(world.Data);
	REQUIRE(!geometry.Lines().empty());

	for (const AdornmentLine &line : geometry.Lines()) {
		CHECK(line.Colour.R == Approx(1.0f));
		CHECK(line.Colour.B == Approx(0.5f));
		CHECK(line.Transparency == Approx(0.25f));
		CHECK_FALSE(line.AlwaysOnTop);
	}
}

TEST_CASE("an adornment outside a container draws nothing", "[render][adornmentgeometry]") {
	// Containment is `gui::EachAdornment`'s and is not re-derived here — a
	// second answer to where an adornment may live would disagree with the
	// first the day one was fixed. This asserts the walk is actually used.
	World world("adornment_geometry.uncontained");

	const Entity loose = world.Data.CreateInstance(engine::scene::PartClass(), "Loose");
	world.Data.Set(loose, engine::scene::Transform{CFrame(Vector3::Zero)});
	world.Data.Set(loose, engine::scene::Bounds{Vector3{1.0f, 1.0f, 1.0f}});
	world.Adorn("SelectionBox", loose);

	AdornmentGeometry geometry;
	geometry.Build(world.Data);
	CHECK(geometry.Lines().empty());
}
