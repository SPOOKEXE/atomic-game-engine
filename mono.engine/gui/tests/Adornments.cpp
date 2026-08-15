// The 3D branch: which instance an adornment is about, and whether it draws.
//
// **What is tested here is the half `gui` owns**, which is the tree half. An
// adornment is a description - what to outline, in what colour, how solid - and
// turning that into geometry needs the adornee's `CFrame` and stud extent,
// which are `scene`'s and which this module may not link. That split is
// `D00022`'s, arrived at again: whoever draws an adornment has both operands
// and this module has one.
//
// So these cases are about the two questions a drawer would otherwise each
// answer differently: what is this about, and may it be drawn at all.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Adornments.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.gui.adornments")

using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Store;

using namespace engine::gui;

namespace {
	struct World {
		Store Data;
		Entity Workspace;

		explicit World(std::string_view name) : Data(name) {
			RegisterGuiClasses();
			Workspace = Bare(std::string(WORKSPACE));
		}

		// A plain instance, standing in for whatever `scene` would have made.
		Entity Bare(const std::string &name, Entity parent = NULL_ENTITY) {
			const Entity made =
				Data.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Instance")), name);
			if (parent != NULL_ENTITY) {
				Data.SetParent(made, parent);
			}
			return made;
		}

		Entity Adorn(const char *klass, Entity parent) {
			const Entity made = Data.CreateInstance(GuiClass(klass), klass);
			Data.SetParent(made, parent);
			return made;
		}
	};
}

TEST_CASE("the 3D branch hangs off GuiBase", "[gui][adornments]") {
	// **The split `GuiBase` was kept for.** When the 2D branch went in, the
	// comment on that class said `GuiBase3d` "and the adornments hang off
	// `GuiBase` when they arrive, and a tree that had flattened the two would
	// have to grow the split back at exactly the point somebody is adding a
	// feature". This is the assertion that the promise was kept.
	RegisterGuiClasses();
	Store store("adornments.tree");

	const Entity box = store.CreateInstance(GuiClass("SelectionBox"), "Box");

	CHECK(store.IsA(box, GuiClass("SelectionBox")));
	CHECK(store.IsA(box, GuiClass("PVAdornment")));
	CHECK(store.IsA(box, GuiClass("GuiBase3d")));
	CHECK(store.IsA(box, GuiClass("GuiBase")));

	// **And it is not a 2D thing**, which is the half that would be wrong if
	// the branch had been hung off `GuiBase2d` to save a class.
	CHECK_FALSE(store.IsA(box, GuiClass("GuiBase2d")));
	CHECK_FALSE(store.IsA(box, GuiClass("GuiObject")));

	const Entity handle = store.CreateInstance(GuiClass("BoxHandleAdornment"), "Handle");
	CHECK(store.IsA(handle, GuiClass("HandleAdornment")));
	CHECK(store.IsA(handle, GuiClass("PVAdornment")));
}

TEST_CASE("an unset adornee means the parent", "[gui][adornments]") {
	// **Roblox's rule, and the reason an unset `Adornee` is a meaningful value
	// rather than an incomplete one.** It is what makes a `SelectionBox` usable
	// by parenting it to the thing it outlines and setting nothing else, which
	// is how nearly every one is used.
	World world("adornments.parent");

	const Entity part = world.Bare("Rock", world.Workspace);
	const Entity box = world.Adorn("SelectionBox", part);

	CHECK(AdorneeOf(world.Data, box) == part);
	CHECK(AdornmentDrawn(world.Data, box));
}

TEST_CASE("a set adornee wins over the parent", "[gui][adornments]") {
	World world("adornments.explicit");

	const Entity holder = world.Bare("Holder", world.Workspace);
	const Entity target = world.Bare("Target", world.Workspace);
	const Entity box = world.Adorn("SelectionBox", holder);

	Adornment state;
	state.Adornee = target;
	world.Data.Set(box, state);

	CHECK(AdorneeOf(world.Data, box) == target);
}

TEST_CASE("an adornee that is gone is not silently replaced by the parent", "[gui][adornments]") {
	// **The one place the fallback must not fire.** An `Adornee` pointing at
	// something destroyed is an adornment about nothing; quietly re-aiming it at
	// whatever it happens to be parented to would draw a box around the wrong
	// object, which is worse than drawing none - a wrong selection box is a
	// wrong answer to "what am I about to delete".
	World world("adornments.dangling");

	const Entity holder = world.Bare("Holder", world.Workspace);
	const Entity target = world.Bare("Target", world.Workspace);
	const Entity box = world.Adorn("SelectionBox", holder);

	Adornment state;
	state.Adornee = target;
	world.Data.Set(box, state);
	REQUIRE(AdorneeOf(world.Data, box) == target);

	world.Data.Destroy(target);

	CHECK(AdorneeOf(world.Data, box) == NULL_ENTITY);
	CHECK_FALSE(AdornmentDrawn(world.Data, box));
}

TEST_CASE("an adornment draws from the world and the gui containers", "[gui][adornments]") {
	// The same containment `Layout` applies to a `SurfaceGui`, and for the same
	// reason: an adornment hangs off something in the world, so the world is a
	// legal home for it - and so are the two interface containers, because an
	// editor keeps its gizmos somewhere a game's tree does not.
	World world("adornments.contained");

	const Entity starter = world.Bare(std::string(STARTER_GUI));
	const Entity player = world.Bare("Someone");
	const Entity playerGui = world.Bare(std::string(PLAYER_GUI), player);

	const Entity inWorld = world.Adorn("SelectionBox", world.Bare("Rock", world.Workspace));
	const Entity inStarter = world.Adorn("SelectionBox", world.Bare("Thing", starter));
	const Entity inPlayer = world.Adorn("SelectionBox", world.Bare("Thing", playerGui));

	CHECK(AdornmentDrawn(world.Data, inWorld));
	CHECK(AdornmentDrawn(world.Data, inStarter));
	CHECK(AdornmentDrawn(world.Data, inPlayer));
}

TEST_CASE("an adornment outside a container does not draw", "[gui][adornments]") {
	// **Under a `Part` is not contained, and the distinction is the one that
	// would otherwise be discovered by somebody wondering why their handle
	// vanished when they tidied the tree.** The part is what an adornment
	// *adorns*, not where it lives - and a part that is itself outside the
	// world is nowhere at all.
	World world("adornments.uncontained");

	const Entity loose = world.Bare("Loose");
	const Entity box = world.Adorn("SelectionBox", loose);

	// The adornee resolves - its parent is right there - and it still must not
	// draw, which is what separates the two questions.
	CHECK(AdorneeOf(world.Data, box) == loose);
	CHECK_FALSE(AdornmentDrawn(world.Data, box));
}

TEST_CASE("an invisible adornment does not draw", "[gui][adornments]") {
	World world("adornments.invisible");

	const Entity part = world.Bare("Rock", world.Workspace);
	const Entity box = world.Adorn("SelectionBox", part);

	Adornment state;
	state.Visible = false;
	world.Data.Set(box, state);

	CHECK_FALSE(AdornmentDrawn(world.Data, box));
}

TEST_CASE("adornments are visited in ZIndex order", "[gui][adornments]") {
	// **Ordered here rather than by each drawer**, because two drawers sorting
	// independently is two answers to what covers what - and an editor drawing
	// a move gizmo under the selection box it belongs to reads as the gizmo
	// being broken.
	World world("adornments.order");

	const Entity part = world.Bare("Rock", world.Workspace);

	const auto make = [&](const char *name, int32_t zIndex) {
		const Entity made = world.Adorn("SelectionBox", part);
		world.Data.SetInstanceName(made, name);
		Adornment state;
		state.ZIndex = zIndex;
		world.Data.Set(made, state);
		return made;
	};

	make("Third", 5);
	make("First", -1);
	make("Second", 0);

	std::vector<std::string> order;
	EachAdornment(world.Data, [&](Entity adornment, Entity adornee) {
		CHECK(adornee == part);
		order.emplace_back(world.Data.InstanceNameOf(adornment).Text());
	});

	REQUIRE(order.size() == 3);
	CHECK(order[0] == "First");
	CHECK(order[1] == "Second");
	CHECK(order[2] == "Third");
}

TEST_CASE("the walk skips what a drawer must not draw", "[gui][adornments]") {
	// One of each refusal in one walk, so a change that dropped a check fails
	// here whichever check it dropped.
	World world("adornments.skips");

	const Entity part = world.Bare("Rock", world.Workspace);
	const Entity drawn = world.Adorn("SelectionBox", part);

	const Entity hidden = world.Adorn("SelectionBox", part);
	Adornment invisible;
	invisible.Visible = false;
	world.Data.Set(hidden, invisible);

	const Entity loose = world.Adorn("SelectionBox", world.Bare("Nowhere"));

	size_t visited = 0;
	Entity only = NULL_ENTITY;
	EachAdornment(world.Data, [&](Entity adornment, Entity) {
		visited++;
		only = adornment;
	});

	CHECK(visited == 1);
	CHECK(only == drawn);
	CHECK(loose != drawn);
}
