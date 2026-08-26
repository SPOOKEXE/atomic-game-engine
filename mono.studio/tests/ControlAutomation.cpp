#include "ControlAutomation.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("studio.controlautomation")

using studio::automation::ParseScreenshotTarget;
using studio::automation::PlanScreenshots;
using studio::automation::ScreenshotTarget;
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
