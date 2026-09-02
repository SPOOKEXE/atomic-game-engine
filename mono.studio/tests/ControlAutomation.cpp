#include "ControlAutomation.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("studio.controlautomation")

using studio::automation::ParseKeyboardModifiers;
using studio::automation::ParseScreenshotTarget;
using studio::automation::PlanScreenshots;
using studio::automation::ScreenshotTarget;
using studio::automation::ValidateTextInput;
using studio::automation::VisibleScene;

TEST_CASE("screenshot targets accept only the public MCP spellings", "[studio][control]") {
	ScreenshotTarget target = ScreenshotTarget::All;
	CHECK(ParseScreenshotTarget("scene", target));
	CHECK(target == ScreenshotTarget::Scene);
	CHECK(ParseScreenshotTarget("studio", target));
	CHECK(target == ScreenshotTarget::Studio);
	CHECK(ParseScreenshotTarget("all", target));
	CHECK(target == ScreenshotTarget::All);
	CHECK_FALSE(ParseScreenshotTarget("window", target));
}

TEST_CASE("keyboard modifiers accept the stable MCP spellings", "[studio][control]") {
	const std::array names{std::string("control"), std::string("shift"), std::string("control")};
	uint8_t modifiers = 0;
	std::string failure;
	CHECK(ParseKeyboardModifiers(names, modifiers, failure));
	CHECK(failure.empty());
	CHECK((modifiers & studio::automation::KeyboardModifierControl) != 0);
	CHECK((modifiers & studio::automation::KeyboardModifierShift) != 0);

	const std::array unknown{std::string("super")};
	CHECK_FALSE(ParseKeyboardModifiers(unknown, modifiers, failure));
	CHECK(failure == "modifier must be shift, control, alt or gui");
}

TEST_CASE("emulated text is nonempty bounded C-compatible UTF-8", "[studio][control]") {
	std::string failure;
	CHECK(ValidateTextInput("hello, Studio", failure));
	CHECK(failure.empty());
	CHECK_FALSE(ValidateTextInput({}, failure));
	CHECK(failure == "text must not be empty");
	CHECK_FALSE(ValidateTextInput(std::string(16 * 1024 + 1, 'x'), failure));
	CHECK(failure == "text must not exceed 16384 UTF-8 bytes");
	CHECK_FALSE(ValidateTextInput(std::string_view("a\0b", 3), failure));
	CHECK(failure == "text must not contain a null byte");
}

TEST_CASE("each visible scene gets a safe distinct screenshot path", "[studio][control]") {
	const std::array visible{
		VisibleScene{"Start/Room", 0},
		VisibleScene{"Start?Room", 1},
		VisibleScene{"Start/Room", 2},
	};
	std::string failure;
	const auto tasks = PlanScreenshots("/tmp/atomic", ScreenshotTarget::All, visible, {}, failure);

	CHECK(failure.empty());
	REQUIRE(tasks.size() == 3);
	CHECK(tasks[0].Path == "/tmp/atomic/scene-Start_Room.bmp");
	CHECK(tasks[0].Scene == "Start/Room");
	CHECK(tasks[0].Slot == 0);
	CHECK(tasks[1].Path == "/tmp/atomic/scene-Start_Room-2.bmp");
	CHECK(tasks[1].Scene == "Start?Room");
	CHECK(tasks[1].Slot == 1);
	CHECK(tasks[2].Path == "/tmp/atomic/studio.bmp");
	CHECK(tasks[2].Studio);
}

TEST_CASE("a named scene must be visible and Studio-only rejects the name", "[studio][control]") {
	const std::array visible{VisibleScene{"Start", 0}};
	std::string failure;

	CHECK(PlanScreenshots("/tmp/atomic", ScreenshotTarget::Scene, visible, "Other", failure).empty());
	CHECK(failure == "no visible viewport shows scene 'Other'");

	CHECK(PlanScreenshots("/tmp/atomic", ScreenshotTarget::Studio, visible, "Start", failure).empty());
	CHECK(failure == "scene can only be used with target scene or all");
}

TEST_CASE(
	"scene capture refuses an empty viewport set while all still captures Studio", "[studio][control]"
) {
	std::string failure;
	CHECK(PlanScreenshots("/tmp/atomic", ScreenshotTarget::Scene, {}, {}, failure).empty());
	CHECK(failure == "there are no visible scene views to capture");

	const auto tasks = PlanScreenshots("/tmp/atomic", ScreenshotTarget::All, {}, {}, failure);
	CHECK(failure.empty());
	REQUIRE(tasks.size() == 1);
	CHECK(tasks.front().Studio);
}
