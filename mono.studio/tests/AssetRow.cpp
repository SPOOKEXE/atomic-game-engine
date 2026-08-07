// The content list's row, checked without a window.
//
// **This suite exists because the editor aborted and no test could have said
// so.** `mono.studio/AGENTS.md` records that most of this program needs a
// window, a device and an imgui frame — and two of those three are not true.
// `engine.ui.theme` already makes the point in one line: *an imgui context is
// not a device*. `ImGui::CreateContext` allocates a style table and a font atlas
// description and touches no driver, so a frame can be submitted, a mouse can be
// moved and clicked, and everything that is not a rasteriser can be checked.
//
// What was not checked, and what cost an `abort()` on the first frame the asset
// picker opened, is Dear ImGui's own contract:
//
//     Code uses SetCursorPos()/SetCursorScreenPos() to extend window/parent
//     boundaries. Please submit an item e.g. Dummy() afterwards.
//
// That is an `IM_ASSERT` inside `EndChild`, so it is not a drawing glitch — the
// process ends. The first case below is the detector for it, and it is written
// to *fail on the old code* rather than merely to pass on the new: a check that
// has never seen the fault it is for is a check nobody knows works.
//
// **`imgui_internal.h`, deliberately.** `window->DC.IsSetPos` is the exact flag
// `ErrorCheckUsingSetCursorPosToExtendParentBoundaries` reads, and asserting on
// anything else would be asserting on a proxy. The alternative is to let the
// real assert fire, which aborts the test binary instead of failing a case.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <studio/AssetRow.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("studio.assetrow")

using studio::DrawAssetRow;
using studio::RowAction;

namespace {
	// How tall every row in these cases is.
	constexpr float SIDE = 48.0f;

	// Where the test window sits, so a case can compute what to click.
	constexpr float WINDOW_X = 40.0f;
	constexpr float WINDOW_Y = 30.0f;

	// A bare imgui context, created and destroyed around one case.
	//
	// **Per case rather than shared**, unlike the benchmark's: a case that fails
	// mid-frame leaves a context with a window open on the stack, and the next
	// case would inherit it and fail for a reason that is not its own.
	//
	// No backends, so `NewFrame` is given what a platform backend would have
	// supplied — a display size and a built font atlas. A zero-sized display
	// clips every window to nothing, which would make every hit test miss and
	// every case pass for the wrong reason.
	class Context {
	  public:
		// @param assertOnError Whether imgui's own `IM_ASSERT_USER_ERROR` fires.
		//        **Off for the one case that provokes an error on purpose** —
		//        the assert calls `abort()`, which ends the test binary instead
		//        of failing a case, so the detector could not otherwise be run
		//        against the fault it detects. Everything else leaves it on, so
		//        a case that trips imgui by accident is loud.
		explicit Context(bool assertOnError = true) {
			IMGUI_CHECKVERSION();
			Handle = ImGui::CreateContext();

			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1280.0f, 720.0f);
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
			io.DeltaTime = 1.0f / 60.0f;

			// No ini file: a suite that read one would lay out differently on
			// every machine and would write one into whatever directory it ran
			// from.
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;

			io.ConfigErrorRecoveryEnableAssert = assertOnError;
			io.ConfigErrorRecoveryEnableTooltip = false;

			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}

		~Context() {
			ImGui::DestroyContext(Handle);
		}

		Context(const Context &) = delete;
		Context &operator=(const Context &) = delete;

	  private:
		ImGuiContext *Handle = nullptr;
	};

	// Where the pointer is and whether it is down, for the next frame.
	struct Mouse {
		float X = -1.0f;
		float Y = -1.0f;
		bool Down = false;
	};

	// Submits one frame with `body` inside a window at a known place.
	//
	// The window has no title bar and no scrollbar so a row's screen position is
	// the window's plus the padding, which is what lets a case click a row by
	// arithmetic rather than by guessing.
	template <class Body> void Frame(const Mouse &mouse, Body &&body) {
		ImGuiIO &io = ImGui::GetIO();
		io.AddMousePosEvent(mouse.X, mouse.Y);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, mouse.Down);

		ImGui::NewFrame();

		ImGui::SetNextWindowPos(ImVec2(WINDOW_X, WINDOW_Y));
		ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
		if (ImGui::Begin(
				"rows",
				nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings
			)) {
			body();
		}
		ImGui::End();

		ImGui::Render();
	}

	// Whether a cursor was left dangling: set by `SetCursorPos` with no item
	// submitted after it. This is what `EndChild` aborts on.
	bool CursorDangling() {
		return ImGui::GetCurrentWindow()->DC.IsSetPos;
	}

	// Where the first row's middle is, in screen space.
	//
	// Taken from the window rather than assumed, because the padding is a style
	// value and a case that hardcoded it would break on a theme change and say
	// nothing useful about why.
	ImVec2 FirstRowCentre() {
		const ImGuiStyle &style = ImGui::GetStyle();
		return ImVec2(
			WINDOW_X + style.WindowPadding.x + 20.0f, WINDOW_Y + style.WindowPadding.y + SIDE * 0.5f
		);
	}
}

TEST_CASE("the old row shape leaves a cursor imgui aborts on", "[studio][assetrow]") {
	// **The detector, proved against the fault it is for.** This is the exact
	// sequence the asset picker had: a selectable, a rewind to draw over it, and
	// a final rewind to place the next row that nothing submits an item after.
	// It is kept as a case rather than described in a comment because a check
	// that has never seen its fault is a check nobody knows works — and the
	// whole point of this suite is that this shape reached a release.
	Context context(false);

	bool dangling = false;
	Frame(Mouse{}, [&] {
		const ImVec2 start = ImGui::GetCursorPos();
		ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, SIDE));

		ImGui::SetCursorPos(start);
		ImGui::Dummy(ImVec2(SIDE, SIDE));

		ImGui::SameLine();
		ImGui::SetCursorPos(ImVec2(start.x + SIDE + 8.0f, start.y));
		ImGui::TextUnformatted("Bricks075A.amat");

		// The line that aborts the editor. Everything above it submits an item
		// after moving the cursor; this one does not.
		ImGui::SetCursorPos(ImVec2(start.x, start.y + SIDE + ImGui::GetStyle().ItemSpacing.y));

		dangling = CursorDangling();
	});

	CHECK(dangling);
}

TEST_CASE("a row leaves no dangling cursor", "[studio][assetrow]") {
	Context context;

	bool dangling = true;
	Frame(Mouse{}, [&] {
		for (int row = 0; row < 3; row++) {
			ImGui::PushID(row);
			DrawAssetRow(false, SIDE, [](ImVec2) {});
			ImGui::PopID();
		}
		dangling = CursorDangling();
	});

	// **Three rows and not one**, because the fault was on the *last* iteration
	// of a loop: a single row hid it, since the row after it submitted an item
	// and cleared the flag.
	CHECK_FALSE(dangling);
}

TEST_CASE("a row that paints nothing still reserves its height", "[studio][assetrow]") {
	// The picture and the label are painted rather than submitted, so the row's
	// height comes from the selectable alone. A row that measured its contents
	// would collapse to a line of text the moment a thumbnail was missing, and
	// the list would reflow as previews arrived — `DrawPreview`'s own rule about
	// a list nobody can click in.
	Context context;

	float first = 0.0f;
	float second = 0.0f;
	Frame(Mouse{}, [&] {
		first = ImGui::GetCursorPosY();
		DrawAssetRow(false, SIDE, [](ImVec2) {});
		second = ImGui::GetCursorPosY();
	});

	CHECK(second - first >= SIDE);
}

TEST_CASE("a click on a row chooses it", "[studio][assetrow]") {
	Context context;

	const auto run = [](const Mouse &mouse, RowAction &action) {
		Frame(mouse, [&] {
			action = DrawAssetRow(false, SIDE, [](ImVec2) {});
		});
	};

	RowAction action = RowAction::None;

	// A frame with the pointer resting on the row, because imgui needs one frame
	// to know what is under it before a press can land on anything.
	ImVec2 centre(0.0f, 0.0f);
	Frame(Mouse{}, [&] {
		centre = FirstRowCentre();
		DrawAssetRow(false, SIDE, [](ImVec2) {});
	});

	run(Mouse{.X = centre.x, .Y = centre.y, .Down = false}, action);
	run(Mouse{.X = centre.x, .Y = centre.y, .Down = true}, action);
	CHECK(action == RowAction::None);

	// A `Selectable` answers on release, which is what makes a drag off it a
	// cancel rather than a choice.
	run(Mouse{.X = centre.x, .Y = centre.y, .Down = false}, action);
	CHECK(action == RowAction::Chose);
}

TEST_CASE("a click on the picture is a click on the row", "[studio][assetrow]") {
	// **A property worth pinning, and pointedly not a bug that was fixed.** The
	// first version of `AssetRow.hpp` claimed the picture — an item submitted
	// over the selectable — was stealing the click, which is why a picker
	// "didn't select". It was not: the case below this one drives the same click
	// through the old shape and it lands. The crash was the whole of the bug.
	//
	// It stays because it is what a person aims at in a picker, and because
	// nothing else says so.
	Context context;

	RowAction action = RowAction::None;
	const auto row = [&] {
		action = DrawAssetRow(false, SIDE, [](ImVec2 corner) {
			// What the editor paints: a border and a picture, straight to the
			// draw list, submitting no item.
			ImGui::GetWindowDrawList()->AddRect(
				corner, ImVec2(corner.x + SIDE, corner.y + SIDE), IM_COL32_WHITE
			);
		});
	};

	// Squarely inside the picture rather than out on the name.
	ImVec2 picture(0.0f, 0.0f);
	Frame(Mouse{}, [&] {
		const ImGuiStyle &style = ImGui::GetStyle();
		picture = ImVec2(
			WINDOW_X + style.WindowPadding.x + SIDE * 0.5f,
			WINDOW_Y + style.WindowPadding.y + SIDE * 0.5f
		);
		row();
	});

	Frame(Mouse{.X = picture.x, .Y = picture.y, .Down = false}, row);
	Frame(Mouse{.X = picture.x, .Y = picture.y, .Down = true}, row);
	Frame(Mouse{.X = picture.x, .Y = picture.y, .Down = false}, row);

	CHECK(action == RowAction::Chose);
}

TEST_CASE("the old row shape did deliver its clicks", "[studio][assetrow]") {
	// **Written to refute a diagnosis, and kept because it did.** "Selection
	// doesn't work" and "an item over a selectable steals its hover" is a
	// plausible pair, and acting on it would have been a fix for a fault that
	// was not there — `AGENTS.md`: do not fix what you have not reproduced.
	//
	// Overlapping items are still gone, for the reasons `AssetRow.hpp` gives:
	// one copy of the row's geometry, one item per row. Neither is this.
	Context context(false);

	bool clicked = false;
	const auto row = [&] {
		const ImVec2 start = ImGui::GetCursorPos();
		if (ImGui::Selectable(
				"##row", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, SIDE)
			)) {
			clicked = true;
		}
		ImGui::SetCursorPos(start);
		ImGui::Dummy(ImVec2(SIDE, SIDE));
		ImGui::SameLine();
		ImGui::SetCursorPos(ImVec2(start.x + SIDE + 8.0f, start.y));
		ImGui::TextUnformatted("Bricks075A.amat");
		ImGui::Dummy(ImVec2(0.0f, 0.0f));
	};

	const ImGuiStyle &style = ImGui::GetStyle();
	const float x = WINDOW_X + style.WindowPadding.x + SIDE * 0.5f;
	const float y = WINDOW_Y + style.WindowPadding.y + SIDE * 0.5f;

	Frame(Mouse{}, row);
	Frame(Mouse{.X = x, .Y = y, .Down = false}, row);
	Frame(Mouse{.X = x, .Y = y, .Down = true}, row);
	Frame(Mouse{.X = x, .Y = y, .Down = false}, row);

	CHECK(clicked);
}

TEST_CASE("a click beside the list chooses nothing", "[studio][assetrow]") {
	Context context;

	RowAction action = RowAction::Chose;
	const auto row = [&] {
		action = DrawAssetRow(false, SIDE, [](ImVec2) {});
	};

	// Below the row, inside the window. A picker whose empty space selected the
	// last row would confirm something nobody pointed at.
	const float below = WINDOW_Y + 200.0f;

	Frame(Mouse{}, row);
	Frame(Mouse{.X = WINDOW_X + 60.0f, .Y = below, .Down = false}, row);
	Frame(Mouse{.X = WINDOW_X + 60.0f, .Y = below, .Down = true}, row);
	Frame(Mouse{.X = WINDOW_X + 60.0f, .Y = below, .Down = false}, row);

	CHECK(action == RowAction::None);
}

TEST_CASE("a double click confirms rather than merely choosing", "[studio][assetrow]") {
	Context context;

	RowAction action = RowAction::None;
	RowAction last = RowAction::None;
	const auto row = [&] {
		action = DrawAssetRow(false, SIDE, [](ImVec2) {});
		if (action != RowAction::None) {
			last = action;
		}
	};

	ImVec2 centre(0.0f, 0.0f);
	Frame(Mouse{}, [&] {
		centre = FirstRowCentre();
		row();
	});

	const Mouse over{.X = centre.x, .Y = centre.y, .Down = false};
	const Mouse press{.X = centre.x, .Y = centre.y, .Down = true};

	Frame(over, row);
	Frame(press, row);
	Frame(over, row);
	Frame(press, row);
	Frame(over, row);

	// **Both clicks answer, and the second answers `Confirmed`.** A picker that
	// only reported the double would make a single click select nothing, which
	// is the behaviour somebody reads as a broken list.
	CHECK(last == RowAction::Confirmed);
}

TEST_CASE("the row's hover survives the body", "[studio][assetrow]") {
	// `Editor::HoverPreview` calls `ImGui::IsItemHovered()` itself, at six call
	// sites, and this is what makes that keep working through a row: the body
	// paints and submits nothing, so `LastItemData` is still the selectable's
	// when it runs. A body that submitted an item would silently point the
	// preview at that item instead.
	Context context;

	bool hovered = false;
	const auto row = [&] {
		DrawAssetRow(false, SIDE, [](ImVec2 corner) {
			ImGui::GetWindowDrawList()->AddRect(
				corner, ImVec2(corner.x + SIDE, corner.y + SIDE), IM_COL32_WHITE
			);
		});
		hovered = ImGui::IsItemHovered();
	};

	ImVec2 centre(0.0f, 0.0f);
	Frame(Mouse{}, [&] {
		centre = FirstRowCentre();
		row();
	});

	Frame(Mouse{.X = centre.x, .Y = centre.y, .Down = false}, row);
	CHECK(hovered);

	Frame(Mouse{.X = 5.0f, .Y = 5.0f, .Down = false}, row);
	CHECK_FALSE(hovered);
}

TEST_CASE("a child full of rows closes without tripping imgui", "[studio][assetrow]") {
	// **The picker's own shape, with imgui's assert left on.** Every case above
	// reads `DC.IsSetPos` directly, which is precise and is not the thing that
	// actually happened: what happened is that `EndChild` called `abort()`. This
	// runs the real check by running the real code — a scrolling child, a run of
	// rows, and an `EndChild` that is allowed to assert. If it ever does, this
	// case ends the binary, which is exactly as loud as the editor was.
	Context context;

	Frame(Mouse{}, [] {
		if (ImGui::BeginChild("##rows", ImVec2(0.0f, 200.0f), ImGuiChildFlags_Borders)) {
			for (int row = 0; row < 12; row++) {
				ImGui::PushID(row);
				DrawAssetRow(row == 3, SIDE, [](ImVec2 corner) {
					ImGui::GetWindowDrawList()->AddRect(
						corner, ImVec2(corner.x + SIDE, corner.y + SIDE), IM_COL32_WHITE
					);
					ImGui::GetWindowDrawList()->AddText(
						ImVec2(corner.x + SIDE + 8.0f, corner.y + 16.0f), IM_COL32_WHITE, "asset.amat"
					);
				});
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
	});

	// Reaching here is the assertion. Catch2 wants one anyway.
	SUCCEED("the child closed");
}

TEST_CASE("a tile that rewinds inside a group is closed by the group", "[studio][assetrow]") {
	// **The gallery has the row's old shape and does not crash, and this is
	// why.** Its trailing `SetCursorPos` sits immediately before `EndGroup`, and
	// `EndGroup` calls `ItemSize` — so the flag is cleared by the group rather
	// than by an item the author remembered to add.
	//
	// It is checked rather than reasoned about because the two are one edit
	// apart: moving that `EndGroup`, or dropping the group for a tile that no
	// longer needs one, turns the gallery into the picker.
	Context context;

	bool dangling = true;
	Frame(Mouse{}, [&] {
		ImGui::BeginGroup();
		const ImVec2 start = ImGui::GetCursorPos();
		ImGui::Selectable("##tile", false, ImGuiSelectableFlags_None, ImVec2(100.0f, 120.0f));
		ImGui::SetCursorPos(ImVec2(start.x + 4.0f, start.y + 4.0f));
		ImGui::TextUnformatted("caption");
		ImGui::SetCursorPos(ImVec2(start.x, start.y + 120.0f));
		ImGui::EndGroup();

		dangling = CursorDangling();
	});

	CHECK_FALSE(dangling);
}
