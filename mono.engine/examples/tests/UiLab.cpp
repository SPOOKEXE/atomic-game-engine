// The authored UI lab, exercised through the same layout and input path as a client.

#include <engine/core/Paths.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Services.hpp>
#include <engine/gui/Typing.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.examples.ui-lab")
TEST_DEPENDS("engine.scripthost.scripting")

using Catch::Approx;
using engine::core::Name;
using engine::core::Vector2;
using engine::ecs::AttributeValue;
using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::ecs::Scheduler;
using engine::ecs::Store;

namespace {
	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	struct Lab {
		StagedAssets Assets;
		Store World{"examples.ui_lab"};
		Scheduler Systems;
		std::shared_ptr<engine::script::Runtime> Runtime;
		engine::gui::CompileRequest Request;
		engine::gui::Compiled Compiled;
		engine::gui::Router Router;
		Entity Screen = NULL_ENTITY;

		bool Load(std::string &error) {
			engine::script::RuntimeLimits limits;
			limits.Role = engine::script::HostRole::OfBoth();
			if (!engine::examples::LoadScene(
					World, Systems, engine::examples::ExamplePath("UiLab.luau"), error, &Runtime, &limits
				))
				return false;
			const Entity starter = World.FindFirstRoot("StarterGui");
			Screen = World.FindFirstChild(starter, "LabScreen");
			return Runtime != nullptr && Screen != NULL_ENTITY;
		}

		Entity Find(std::string_view name) const {
			Entity found = NULL_ENTITY;
			World.EachDescendant(Screen, [&](Entity entity) {
				if (found == NULL_ENTITY && World.InstanceNameOf(entity) == Name(name)) found = entity;
			});
			return found;
		}

		const engine::gui::Resolved *Resolved(Entity entity) const {
			return World.Get<engine::gui::Resolved>(entity);
		}

		void Resize(float width, float height) {
			Request.Display.Width = width;
			Request.Display.Height = height;
			Request.Hovered = Router.Hovered();
			Request.Pressed = Router.Pressed();
			Compiled.Rebuild(World, Request);
		}

		Vector2 Centre(Entity entity) const {
			const auto *box = Resolved(entity);
			if (box == nullptr) return {};
			return {
				box->AbsolutePosition.X + box->AbsoluteSize.X * 0.5f,
				box->AbsolutePosition.Y + box->AbsoluteSize.Y * 0.5f
			};
		}

		std::vector<engine::gui::GuiEvent> Point(Vector2 position, bool down = false, float wheel = 0.0f) {
			Resize(Request.Display.Width, Request.Display.Height);
			engine::gui::Pointer pointer;
			pointer.Position = position;
			pointer.Down = down;
			pointer.Wheel = wheel;
			pointer.ScreenOnly = true;
			const auto events = Router.Update(World, Compiled.Commands(), pointer);
			std::vector<engine::gui::GuiEvent> copied(events.begin(), events.end());
			Runtime->DeliverGuiEvents(copied);
			return copied;
		}

		bool Beat() {
			return Runtime->Heartbeat(0.0f);
		}

		bool Click(Entity entity) {
			const Vector2 point = Centre(entity);
			Point(point, true);
			Point(point);
			return Beat();
		}

		bool Integer(std::string_view name, int64_t &value) const {
			AttributeValue attribute;
			if (!engine::ecs::GetAttribute(World, Screen, Name(name), attribute)) return false;
			if (attribute.Type == engine::ecs::PropertyType::Int64) {
				value = attribute.Int64;
				return true;
			}
			if (attribute.Type == engine::ecs::PropertyType::Int32) {
				value = attribute.Int32;
				return true;
			}
			if (attribute.Type == engine::ecs::PropertyType::Double && std::isfinite(attribute.Double) &&
				std::trunc(attribute.Double) == attribute.Double &&
				attribute.Double >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
				attribute.Double < 9223372036854775808.0) {
				value = static_cast<int64_t>(attribute.Double);
				return true;
			}
			return false;
		}

		bool String(std::string_view name, std::string &value) const {
			AttributeValue attribute;
			if (!engine::ecs::GetAttribute(World, Screen, Name(name), attribute) ||
				attribute.Type != engine::ecs::PropertyType::String)
				return false;
			value = attribute.String;
			return true;
		}

		bool Boolean(std::string_view name, bool &value) const {
			AttributeValue attribute;
			if (!engine::ecs::GetAttribute(World, Screen, Name(name), attribute) ||
				attribute.Type != engine::ecs::PropertyType::Bool)
				return false;
			value = attribute.Bool;
			return true;
		}

		bool Number(std::string_view name, float &value) const {
			AttributeValue attribute;
			if (!engine::ecs::GetAttribute(World, Screen, Name(name), attribute)) return false;
			if (attribute.Type == engine::ecs::PropertyType::Float) {
				value = attribute.Float;
				return true;
			}
			if (attribute.Type == engine::ecs::PropertyType::Double) {
				value = static_cast<float>(attribute.Double);
				return true;
			}
			return false;
		}
	};

	bool Has(const std::vector<engine::gui::GuiEvent> &events, engine::gui::EventKind kind, Entity entity) {
		for (const auto &event : events)
			if (event.Kind == kind && event.Instance == entity) return true;
		return false;
	}

	bool Usable(const Lab &lab, Entity entity, float width, float height) {
		const auto *box = lab.Resolved(entity);
		if (box == nullptr || !box->Rendered || box->AbsoluteSize.X < 20.0f || box->AbsoluteSize.Y < 20.0f)
			return false;
		const Vector2 centre = lab.Centre(entity);
		return centre.X >= 0.0f && centre.X <= width && centre.Y >= 0.0f && centre.Y <= height &&
			   centre.X >= box->Clip.Min.X && centre.X <= box->Clip.Max.X && centre.Y >= box->Clip.Min.Y &&
			   centre.Y <= box->Clip.Max.Y;
	}

	bool ScrollTo(Lab &lab, Entity entity) {
		const Entity content = lab.Find("Content");
		if (content == NULL_ENTITY) return false;
		for (int attempt = 0; attempt < 20; attempt++) {
			lab.Resize(lab.Request.Display.Width, lab.Request.Display.Height);
			if (Usable(lab, entity, lab.Request.Display.Width, lab.Request.Display.Height)) return true;
			const auto *viewport = lab.Resolved(content);
			if (viewport == nullptr) return false;
			lab.Point(
				{viewport->AbsolutePosition.X + 2.0f, viewport->AbsolutePosition.Y + 90.0f}, false, -3.0f
			);
		}
		lab.Resize(lab.Request.Display.Width, lab.Request.Display.Height);
		return Usable(lab, entity, lab.Request.Display.Width, lab.Request.Display.Height);
	}

	Vector2 VisibleCentre(const engine::gui::Resolved &box) {
		const float left = std::max(box.AbsolutePosition.X, box.Clip.Min.X);
		const float top = std::max(box.AbsolutePosition.Y, box.Clip.Min.Y);
		const float right = std::min(box.AbsolutePosition.X + box.AbsoluteSize.X, box.Clip.Max.X);
		const float bottom = std::min(box.AbsolutePosition.Y + box.AbsoluteSize.Y, box.Clip.Max.Y);
		return {(left + right) * 0.5f, (top + bottom) * 0.5f};
	}
}

TEST_CASE(
	"UI lab keeps its primary controls usable across display sizes", "[examples][gui][headless][ui-lab]"
) {
	Lab lab;
	std::string error;
	INFO(error);
	REQUIRE(lab.Load(error));

	const std::array<std::string_view, 3> controls{"ClickButton", "ImageButton", "Entry"};
	for (std::string_view name : controls) {
		INFO(name);
		REQUIRE(lab.Find(name) != NULL_ENTITY);
	}

	const std::array<Vector2, 5> displays{{
		{360.0f, 640.0f},
		{640.0f, 360.0f},
		{960.0f, 540.0f},
		{1280.0f, 720.0f},
		{1920.0f, 1080.0f},
	}};
	for (const Vector2 display : displays) {
		INFO(display.X << "x" << display.Y);
		lab.Resize(display.X, display.Y);
		CHECK_FALSE(lab.Compiled.Commands().Commands.empty());
		const auto *content = lab.Resolved(lab.Find("Content"));
		REQUIRE(content != nullptr);
		CHECK(content->AbsoluteSize.X <= 620.1f);
		CHECK(content->AbsoluteSize.X <= display.X * 0.94f + 0.1f);
		CHECK(content->AbsolutePosition.X >= 0.0f);
		CHECK(content->AbsolutePosition.X + content->AbsoluteSize.X <= display.X + 0.1f);
		for (std::string_view name : controls) {
			INFO(name);
			CHECK(Usable(lab, lab.Find(name), display.X, display.Y));
		}
	}
}

TEST_CASE(
	"UI lab authored scale and aspect controls resolve in layout", "[examples][gui][headless][ui-lab]"
) {
	Lab lab;
	std::string error;
	INFO(error);
	REQUIRE(lab.Load(error));
	lab.Resize(1280.0f, 720.0f);

	const Entity sample = lab.Find("ScaleSample");
	const Entity scaleValue = lab.Find("ScaleValue");
	const Entity aspectTitle = lab.Find("AspectTitle");
	const Entity aspect = lab.Find("AspectSample");
	REQUIRE(sample != NULL_ENTITY);
	REQUIRE(scaleValue != NULL_ENTITY);
	REQUIRE(aspectTitle != NULL_ENTITY);
	REQUIRE(aspect != NULL_ENTITY);
	REQUIRE(lab.Resolved(sample) != nullptr);
	REQUIRE(lab.Resolved(aspect) != nullptr);
	const float baselineWidth = lab.Resolved(sample)->AbsoluteSize.X;
	CHECK(baselineWidth == Approx(164.0f));
	const auto *authoredAspect = lab.World.Get<engine::gui::Element>(aspect);
	REQUIRE(authoredAspect != nullptr);
	CHECK(authoredAspect->Size.Y.Offset == Approx(140.0f));
	const auto *aspectBox = lab.Resolved(aspect);
	CHECK(aspectBox->AbsoluteSize.Y < authoredAspect->Size.Y.Offset);
	CHECK(aspectBox->AbsoluteSize.X / aspectBox->AbsoluteSize.Y == Approx(16.0f / 9.0f).margin(0.01f));

	const Entity smaller = lab.Find("Scale075");
	const Entity larger = lab.Find("Scale150");
	REQUIRE(smaller != NULL_ENTITY);
	REQUIRE(larger != NULL_ENTITY);
	REQUIRE(lab.Click(smaller));
	lab.Resize(1280.0f, 720.0f);
	CHECK(lab.Resolved(sample)->AbsoluteSize.X == Approx(baselineWidth * 0.75f));
	float scale = 0.0f;
	REQUIRE(lab.Number("scale", scale));
	CHECK(scale == Approx(0.75f));

	REQUIRE(lab.Click(larger));
	lab.Resize(1280.0f, 720.0f);
	CHECK(lab.Resolved(sample)->AbsoluteSize.X == Approx(baselineWidth * 1.5f));
	REQUIRE(lab.Number("scale", scale));
	CHECK(scale == Approx(1.5f));
	const auto *scaledSample = lab.Resolved(sample);
	const auto *scaleLabel = lab.Resolved(scaleValue);
	const auto *aspectHeading = lab.Resolved(aspectTitle);
	REQUIRE(scaledSample != nullptr);
	REQUIRE(scaleLabel != nullptr);
	REQUIRE(aspectHeading != nullptr);
	CHECK(scaleLabel->AbsolutePosition.Y >= scaledSample->AbsolutePosition.Y + scaledSample->AbsoluteSize.Y);
	CHECK(aspectHeading->AbsolutePosition.Y >= scaleLabel->AbsolutePosition.Y + scaleLabel->AbsoluteSize.Y);
	REQUIRE(lab.Click(sample));
	int64_t sampleClicks = 0;
	REQUIRE(lab.Integer("sampleClicks", sampleClicks));
	CHECK(sampleClicks == 1);
}

TEST_CASE(
	"UI lab routes authored button, hover, and cancelled press signals", "[examples][gui][headless][ui-lab]"
) {
	Lab lab;
	std::string error;
	INFO(error);
	REQUIRE(lab.Load(error));
	lab.Resize(1280.0f, 720.0f);

	const Entity textButton = lab.Find("ClickButton");
	const Entity imageButton = lab.Find("ImageButton");
	REQUIRE(textButton != NULL_ENTITY);
	REQUIRE(imageButton != NULL_ENTITY);
	REQUIRE(Usable(lab, textButton, 1280.0f, 720.0f));
	REQUIRE(Usable(lab, imageButton, 1280.0f, 720.0f));

	int64_t textClicks = -1;
	int64_t imageClicks = -1;
	REQUIRE(lab.Integer("textClicks", textClicks));
	REQUIRE(lab.Integer("imageClicks", imageClicks));

	const auto hover = lab.Point(lab.Centre(textButton));
	CHECK(Has(hover, engine::gui::EventKind::MouseEnter, textButton));
	CHECK(lab.Router.Hovered() == textButton);
	REQUIRE(lab.Beat());

	const auto down = lab.Point(lab.Centre(textButton), true);
	CHECK(Has(down, engine::gui::EventKind::InputBegan, textButton));
	const auto releasedOutside = lab.Point({-10.0f, -10.0f});
	CHECK_FALSE(Has(releasedOutside, engine::gui::EventKind::Activated, textButton));
	REQUIRE(lab.Beat());
	int64_t afterCancel = -1;
	REQUIRE(lab.Integer("textClicks", afterCancel));
	CHECK(afterCancel == textClicks);

	const auto secondDown = lab.Point(lab.Centre(textButton), true);
	CHECK(Has(secondDown, engine::gui::EventKind::InputBegan, textButton));
	const auto secondUp = lab.Point(lab.Centre(textButton));
	CHECK(Has(secondUp, engine::gui::EventKind::Activated, textButton));
	REQUIRE(lab.Beat());
	REQUIRE(lab.Integer("textClicks", afterCancel));
	CHECK(afterCancel == textClicks + 1);

	CHECK(lab.Click(imageButton));
	REQUIRE(lab.Integer("imageClicks", imageClicks));
	CHECK(imageClicks == 1);
}

TEST_CASE("UI lab submits text through focused keyboard input", "[examples][gui][headless][ui-lab]") {
	Lab lab;
	std::string error;
	INFO(error);
	REQUIRE(lab.Load(error));
	lab.Resize(1280.0f, 720.0f);

	const Entity entry = lab.Find("Entry");
	REQUIRE(entry != NULL_ENTITY);
	REQUIRE(Usable(lab, entry, 1280.0f, 720.0f));
	REQUIRE(lab.Click(entry));
	REQUIRE(engine::gui::FocusedTextBox(lab.World) == entry);

	engine::gui::Typing typing;
	typing.Text = "hello from headless input";
	const auto typed = engine::gui::Type(lab.World, typing);
	CHECK(typed.Instance == entry);
	CHECK(typed.Changed);
	REQUIRE(lab.Beat());

	engine::gui::Typing submit;
	submit.Submit = true;
	const auto submitted = engine::gui::Type(lab.World, submit);
	REQUIRE(submitted.Released);
	engine::gui::GuiEvent release;
	release.Kind = engine::gui::EventKind::FocusReleased;
	release.Instance = submitted.Instance;
	release.Entered = true;
	lab.Runtime->DeliverGuiEvents(std::span<const engine::gui::GuiEvent>(&release, 1));
	REQUIRE(lab.Beat());
	CHECK(engine::gui::FocusedTextBox(lab.World) == NULL_ENTITY);

	std::string value;
	REQUIRE(lab.String("submitted", value));
	CHECK(value == "hello from headless input");
}

TEST_CASE("UI lab scroll clipping gates row selection", "[examples][gui][headless][ui-lab]") {
	Lab lab;
	std::string error;
	INFO(error);
	REQUIRE(lab.Load(error));
	lab.Resize(1280.0f, 720.0f);
	const Entity list = lab.Find("ScrollList");
	const Entity first = lab.Find("ScrollRow1");
	const Entity last = lab.Find("ScrollRow12");
	const Entity dragTarget = lab.Find("DragTarget");
	REQUIRE(list != NULL_ENTITY);
	REQUIRE(first != NULL_ENTITY);
	REQUIRE(last != NULL_ENTITY);
	REQUIRE(dragTarget != NULL_ENTITY);
	REQUIRE(ScrollTo(lab, dragTarget));
	REQUIRE(Usable(lab, first, 1280.0f, 720.0f));
	REQUIRE(lab.Click(first));
	int64_t selected = 0;
	REQUIRE(lab.Integer("selectedRow", selected));
	CHECK(selected == 1);

	for (int attempt = 0; attempt < 12 && !Usable(lab, last, 1280.0f, 720.0f); attempt++) {
		const auto *listBox = lab.Resolved(list);
		REQUIRE(listBox != nullptr);
		lab.Point(VisibleCentre(*listBox), false, -3.0f);
		lab.Resize(1280.0f, 720.0f);
	}
	const auto *scrolling = lab.World.Get<engine::gui::Scrolling>(list);
	const auto *state = lab.World.Get<engine::gui::ScrollState>(list);
	REQUIRE(scrolling != nullptr);
	REQUIRE(state != nullptr);
	CHECK(state->CanvasSize.Y > state->WindowSize.Y);
	CHECK(scrolling->CanvasPosition.Y > 0.0f);
	REQUIRE(Usable(lab, last, 1280.0f, 720.0f));
	CHECK_FALSE(Usable(lab, first, 1280.0f, 720.0f));
	REQUIRE(lab.Click(last));
	REQUIRE(lab.Integer("selectedRow", selected));
	CHECK(selected == 12);

	// A row outside the nested list's clip cannot react at its stale rectangle.
	const Vector2 clippedFirst = lab.Centre(first);
	lab.Point(clippedFirst, true);
	lab.Point(clippedFirst);
	REQUIRE(lab.Beat());
	REQUIRE(lab.Integer("selectedRow", selected));
	CHECK(selected == 12);
}

TEST_CASE("UI lab drag, tree controls, cap, and reset stay functional", "[examples][gui][headless][ui-lab]") {
	Lab lab;
	std::string error;
	INFO(error);
	REQUIRE(lab.Load(error));
	lab.Resize(1280.0f, 720.0f);
	const Entity target = lab.Find("DragTarget");
	const Entity detector = lab.Find("DragDetector");
	REQUIRE(target != NULL_ENTITY);
	REQUIRE(detector != NULL_ENTITY);
	REQUIRE(ScrollTo(lab, target));
	const auto *element = lab.World.Get<engine::gui::Element>(target);
	REQUIRE(element != nullptr);
	const float originalX = element->Position.X.Offset;
	const Vector2 start = lab.Centre(target);
	const Vector2 end{start.X + 40.0f, start.Y + 8.0f};
	const auto began = lab.Point(start, true);
	CHECK(Has(began, engine::gui::EventKind::DragBegan, detector));
	lab.Point(end, true);
	const auto ended = lab.Point(end);
	CHECK(Has(ended, engine::gui::EventKind::DragEnded, detector));
	REQUIRE(lab.Beat());
	REQUIRE(lab.World.Get<engine::gui::Element>(target) != nullptr);
	CHECK(lab.World.Get<engine::gui::Element>(target)->Position.X.Offset > originalX);
	int64_t count = 0;
	REQUIRE(lab.Integer("dragStarts", count));
	CHECK(count == 1);
	REQUIRE(lab.Integer("dragEnds", count));
	CHECK(count == 1);

	const Entity toggle = lab.Find("ToggleVisibility");
	const Entity visibilityTarget = lab.Find("VisibilityTarget");
	const Entity create = lab.Find("CreateItem");
	const Entity destroy = lab.Find("DestroyItem");
	const Entity reset = lab.Find("Reset");
	REQUIRE(toggle != NULL_ENTITY);
	REQUIRE(visibilityTarget != NULL_ENTITY);
	REQUIRE(create != NULL_ENTITY);
	REQUIRE(destroy != NULL_ENTITY);
	REQUIRE(reset != NULL_ENTITY);
	REQUIRE(ScrollTo(lab, toggle));
	REQUIRE(lab.Click(toggle));
	bool visible = true;
	REQUIRE(lab.Boolean("visible", visible));
	CHECK_FALSE(visible);
	lab.Resize(1280.0f, 720.0f);
	REQUIRE(lab.Resolved(visibilityTarget) != nullptr);
	CHECK_FALSE(lab.Resolved(visibilityTarget)->Rendered);
	REQUIRE(lab.Click(toggle));
	REQUIRE(lab.Boolean("visible", visible));
	CHECK(visible);

	for (int index = 0; index < 9; index++)
		REQUIRE(lab.Click(create));
	REQUIRE(lab.Integer("created", count));
	CHECK(count == 8);
	for (int index = 1; index <= 8; index++) {
		INFO(index);
		CHECK(lab.Find("CreatedItem" + std::to_string(index)) != NULL_ENTITY);
	}
	REQUIRE(lab.Click(destroy));
	REQUIRE(lab.Integer("created", count));
	CHECK(count == 7);
	CHECK(lab.Find("CreatedItem8") == NULL_ENTITY);

	REQUIRE(ScrollTo(lab, reset));
	REQUIRE(lab.Click(reset));
	REQUIRE(lab.Integer("created", count));
	CHECK(count == 0);
	CHECK(lab.Find("CreatedItem1") == NULL_ENTITY);
	REQUIRE(lab.Integer("dragStarts", count));
	CHECK(count == 0);
	REQUIRE(lab.Integer("selectedRow", count));
	CHECK(count == 0);
	REQUIRE(lab.Integer("sampleClicks", count));
	CHECK(count == 0);
	REQUIRE(lab.Boolean("visible", visible));
	CHECK(visible);
	float scale = 0.0f;
	REQUIRE(lab.Number("scale", scale));
	CHECK(scale == Approx(1.0f));
	std::string status;
	REQUIRE(lab.String("status", status));
	CHECK(status == "ready");
}
