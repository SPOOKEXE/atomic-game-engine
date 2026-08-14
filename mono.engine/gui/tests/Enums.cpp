#include <engine/gui/Enums.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

TEST_SUITE_ID("engine.gui.enums")

using namespace engine::gui;

namespace {
	// The member names of one set, in stored order.
	//
	// **This file is a format test and not a spelling test**, which is the
	// distinction worth holding on to. Every enum here is stored as its ordinal
	// in a trivially-copied component, so a `TextXAlignment` of 2 has to mean
	// `Right` in a game file this engine wrote and in one it did not. Reordering
	// a set loads cleanly and lays everything out somewhere plausible - nothing
	// at load time could catch it - so this list is the only thing that does.
	//
	// Renaming a member is a format change too, because the name is what a save
	// file carries and what a script assigns.
	template <class E, size_t N>
	void Expect(const std::array<std::string_view, N> &names, size_t origin = 0) {
		for (size_t index = 0; index < N; index++) {
			CHECK(std::string_view(Describe(static_cast<E>(index + origin))) == names[index]);
		}
	}
}

TEST_CASE("the layout sets keep Roblox's order", "[gui][enums]") {
	Expect<SizeConstraint, 3>({"RelativeXY", "RelativeXX", "RelativeYY"});
	Expect<AutomaticSize, 4>({"None", "X", "Y", "XY"});
	Expect<BorderMode, 3>({"Outline", "Middle", "Inset"});
	Expect<FillDirection, 2>({"Horizontal", "Vertical"});
	Expect<HorizontalAlignment, 3>({"Left", "Center", "Right"});
	Expect<VerticalAlignment, 3>({"Top", "Center", "Bottom"});
	Expect<SortOrder, 3>({"Name", "Custom", "LayoutOrder"});
	Expect<StartCorner, 4>({"TopLeft", "TopRight", "BottomLeft", "BottomRight"});
	Expect<AspectType, 2>({"FitWithinMaxSize", "ScaleWithParentSize"});
	Expect<DominantAxis, 2>({"Width", "Height"});
}

TEST_CASE("the text sets keep Roblox's order", "[gui][enums]") {
	Expect<TextXAlignment, 3>({"Left", "Center", "Right"});

	// **Not the same order as `TextXAlignment`'s**, which is the one a tidy-up
	// would break: Roblox numbers the vertical set Top, Center, Bottom and the
	// horizontal one Left, Center, Right, so the middle member is 1 in both and
	// the ends differ in meaning. Asserted separately for that reason.
	Expect<TextYAlignment, 3>({"Top", "Center", "Bottom"});

	Expect<TextTruncate, 2>({"None", "AtEnd"});
	Expect<FontFace, 4>({"Regular", "Bold", "Italic", "Code"});
	Expect<ScaleType, 5>({"Stretch", "Slice", "Tile", "Fit", "Crop"});
}

TEST_CASE("the collector sets keep Roblox's order", "[gui][enums]") {
	Expect<ZIndexBehavior, 2>({"Global", "Sibling"});
	Expect<SurfaceSizingMode, 2>({"FixedSize", "PixelsPerStud"});

	// **Starts at one.** `ScrollingDirection` is a bit pair in Roblox - X is 1,
	// Y is 2 and XY is 3 - rather than a counter, and the ordinal is the format
	// so it is kept. `Classes.cpp`'s `EnumOrigin` applies the offset in both
	// directions, and this is the assertion that says why it has to.
	Expect<ScrollingDirection, 3>({"X", "Y", "XY"}, 1);
	CHECK(static_cast<int>(ScrollingDirection::X) == 1);
}

TEST_CASE("gui::Face agrees with scene::NormalId", "[gui][enums]") {
	// **Two modules register `NormalId` and both must mean the same six
	// numbers.** `gui` may not link `scene` - both are `shared`, so the tier
	// check cannot catch the edge and `Enums.hpp` refuses it in prose - which
	// means the agreement is held by a pair of tests rather than by a shared
	// declaration.
	//
	// This is the `gui` half. `scene/tests/Enums.cpp` is the other, and the
	// values written out below are the ones it pins: a face of 1 is `Top` in a
	// game file, whichever module wrote it.
	Expect<Face, 6>({"Right", "Top", "Back", "Left", "Bottom", "Front"});

	CHECK(static_cast<int>(Face::Right) == 0);
	CHECK(static_cast<int>(Face::Top) == 1);
	CHECK(static_cast<int>(Face::Back) == 2);
	CHECK(static_cast<int>(Face::Left) == 3);
	CHECK(static_cast<int>(Face::Bottom) == 4);
	CHECK(static_cast<int>(Face::Front) == 5);
}
