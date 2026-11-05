#include "../src/PresentationEventCodec.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

TEST_SUITE_ID("client.presentation_event_codec")

TEST_CASE("presentation event codec preserves input values and owns text", "[client][gui][input]") {
	client::AccessibilityAction source;
	source.Target = engine::ecs::Entity(100);
	source.Collector = engine::ecs::Entity(200);
	source.WorldEpoch = 300;
	source.FrameSequence = 301;
	source.NativeNodeId = 302;
	source.NativeAction = 304;
	source.NativeAction = 304;
	source.Status = client::AccessibilityActionStatus::Unsupported;
	source.RejectedCount = 303;
	source.Action = engine::gui::SemanticAction::Focus;
	auto bytes = client::EncodePresentationEvent(source);
	REQUIRE(bytes);
	auto event = client::DecodePresentationEvent(*bytes);
	REQUIRE(event);
	const auto action = std::get<client::AccessibilityAction>(*event);
	CHECK(action.Target == source.Target);
	CHECK(action.Collector == source.Collector);
	CHECK(action.WorldEpoch == source.WorldEpoch);
	CHECK(action.FrameSequence == source.FrameSequence);
	CHECK(action.NativeNodeId == source.NativeNodeId);
	CHECK(action.NativeAction == source.NativeAction);
	CHECK(action.NativeAction == source.NativeAction);
	CHECK(action.Status == source.Status);
	CHECK(action.RejectedCount == source.RejectedCount);
	CHECK(action.Action == source.Action);
	source.Status = static_cast<client::AccessibilityActionStatus>(255);
	const auto invalidStatus = client::EncodePresentationEvent(source);
	REQUIRE(invalidStatus);
	CHECK_FALSE(client::DecodePresentationEvent(*invalidStatus));

	bytes = client::EncodePresentationEvent(engine::scene::KeyCode::Left);
	REQUIRE(bytes);
	event = client::DecodePresentationEvent(*bytes);
	REQUIRE(event);
	CHECK(std::get<engine::scene::KeyCode>(*event) == engine::scene::KeyCode::Left);

	engine::scene::ControllerState controllers;
	controllers.Slots[2].Buttons = 7;
	controllers.Slots[2].PreviousButtons = 3;
	controllers.Slots[2].PressedButtons = 4;
	controllers.Slots[2].Axes[1] = 0.5f;
	controllers.Slots[2].PreviousAxes[1] = -0.75f;
	controllers.Slots[2].Connected = true;
	controllers.Slots[2].Mapped = true;
	bytes = client::EncodePresentationEvent(controllers);
	REQUIRE(bytes);
	event = client::DecodePresentationEvent(*bytes);
	REQUIRE(event);
	const auto &slot = std::get<engine::scene::ControllerState>(*event).Slots[2];
	CHECK(slot.Buttons == 7);
	CHECK(slot.PreviousButtons == 3);
	CHECK(slot.PressedButtons == 4);
	CHECK(slot.Axes[1] == 0.5f);
	CHECK(slot.PreviousAxes[1] == -0.75f);
	CHECK(slot.Connected);
	CHECK(slot.Mapped);

	std::string text = "hello \xc3\xa9";
	bytes = client::EncodePresentationEvent(client::DeferredEdit{.Text = text, .Extend = true});
	REQUIRE(bytes);
	text.assign("changed");
	event = client::DecodePresentationEvent(*bytes);
	REQUIRE(event);
	CHECK(std::get<client::DeferredEdit>(*event).Text == "hello \xc3\xa9");
	CHECK(std::get<client::DeferredEdit>(*event).Extend);
	bytes = client::EncodePresentationEvent(
		client::DeferredComposition{.Text = "preedit", .Start = 1, .Length = 2, .Revision = 42, .Apply = true}
	);
	REQUIRE(bytes);
	event = client::DecodePresentationEvent(*bytes);
	REQUIRE(event);
	const auto composition = std::get<client::DeferredComposition>(*event);
	CHECK(composition.Text == "preedit");
	CHECK(composition.Start == 1);
	CHECK(composition.Length == 2);
	CHECK(composition.Revision == 42);
	CHECK(composition.Apply);

	engine::render::AdornmentPointer adornment;
	adornment.Ray = engine::core::Ray{{1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, -1.0f}};
	adornment.Position = {12.0f, 23.0f};
	adornment.PrimaryDown = true;
	adornment.Moved = true;
	bytes = client::EncodePresentationEvent(client::DeferredAdornment{adornment, 800.0f, 600.0f});
	REQUIRE(bytes);
	event = client::DecodePresentationEvent(*bytes);
	REQUIRE(event);
	const auto deferred = std::get<client::DeferredAdornment>(*event);
	const auto restored = deferred.Pointer;
	CHECK(deferred.Width == 800.0f);
	CHECK(deferred.Height == 600.0f);
	CHECK(restored.Ray == adornment.Ray);
	CHECK(restored.Position == adornment.Position);
	CHECK(restored.PrimaryDown);
	CHECK(restored.Moved);
}

TEST_CASE("presentation event codec rejects incomplete or oversized records", "[client][gui][input]") {
	const std::string large(engine::input::PresentationInput::MAXIMUM_TEXT_BYTES + 1, 'x');
	CHECK_FALSE(client::EncodePresentationEvent(client::DeferredEdit{.Text = large}));
	const std::string preedit(engine::input::TextComposition::MAXIMUM_BYTES + 1, 'x');
	CHECK_FALSE(client::EncodePresentationEvent(client::DeferredComposition{.Text = preedit}));
	const auto bytes = client::EncodePresentationEvent(client::DeferredEdit{.Text = "abc"});
	REQUIRE(bytes);
	for (size_t size = 0; size < bytes->size(); ++size)
		CHECK_FALSE(client::DecodePresentationEvent(std::span(*bytes).first(size)));
	auto invalid = *bytes;
	invalid.push_back(std::byte{});
	CHECK_FALSE(client::DecodePresentationEvent(invalid));
	invalid = *bytes;
	invalid[0] = std::byte{0xff};
	CHECK_FALSE(client::DecodePresentationEvent(invalid));
	invalid = *bytes;
	invalid[2] = std::byte{2};
	CHECK_FALSE(client::DecodePresentationEvent(invalid));
	engine::render::AdornmentPointer pointer;
	pointer.Position.X = std::numeric_limits<float>::infinity();
	const auto nonfinite = client::EncodePresentationEvent(client::DeferredAdornment{.Pointer = pointer});
	REQUIRE(nonfinite);
	CHECK_FALSE(client::DecodePresentationEvent(*nonfinite));
}
