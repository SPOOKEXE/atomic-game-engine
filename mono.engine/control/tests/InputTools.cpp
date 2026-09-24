// Control input automation reaches the host only after the public MCP boundary validates it.

#include "HookFixture.hpp"

#include <engine/control/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.control.input_tools")
TEST_DEPENDS("engine.control.surface")

using engine::control::InputAutomationEvent;
using engine::control::InputAutomationKind;
using engine::control::InputAutomationState;
using engine::control::Surface;
using nlohmann::json;

namespace {
	json Call(Surface &surface, std::string_view name, json arguments, bool &failed) {
		const json request{
			{"jsonrpc", "2.0"},
			{"id", 1},
			{"method", "tools/call"},
			{"params", json{{"name", name}, {"arguments", std::move(arguments)}}},
		};
		const json reply = json::parse(surface.Answer(request.dump()));
		REQUIRE(reply.contains("result"));
		failed = reply["result"].value("isError", false);
		return json::parse(reply["result"]["content"][0]["text"].get<std::string>());
	}
}

TEST_CASE(
	"input automation validates MCP arguments before delivering semantic events", "[control][input-tools]"
) {
	std::vector<InputAutomationEvent> received;
	Surface surface("test", "test");
	engine::control::test::Install(
		surface, std::array{engine::control::test::Custom("input-automation", [&received](Surface &owner) {
			owner.AddInputTools([&received](const InputAutomationEvent &event, std::string &) {
				received.push_back(event);
				return json{{"queued", true}};
			});
		})}
	);

	bool failed = false;
	CHECK(Call(surface, "emulate_mouse_move", {{"x", 7.5}, {"y", 3.0}}, failed)["queued"]);
	REQUIRE_FALSE(failed);
	REQUIRE(received.size() == 1);
	CHECK(received.back().Kind == InputAutomationKind::MouseMove);
	CHECK(received.back().X == 7.5f);
	CHECK(received.back().Y == 3.0f);

	CHECK(Call(surface, "emulate_click", {{"x", 1.0}, {"y", 2.0}, {"button", "right"}}, failed)["queued"]);
	REQUIRE_FALSE(failed);
	CHECK(received.back().Kind == InputAutomationKind::MouseButton);
	CHECK(received.back().State == InputAutomationState::Click);
	CHECK(received.back().Button == "right");

	CHECK(Call(surface, "emulate_click", {{"x", 1.0}, {"y", 2.0}, {"down", false}}, failed)["queued"]);
	REQUIRE_FALSE(failed);
	CHECK(received.back().State == InputAutomationState::Up);

	CHECK(Call(
		surface,
		"emulate_key",
		{{"key", "Return"}, {"down", true}, {"modifiers", json::array({"shift", "control"})}},
		failed
	)["queued"]);
	REQUIRE_FALSE(failed);
	CHECK(received.back().Kind == InputAutomationKind::Key);
	CHECK(received.back().State == InputAutomationState::Down);
	CHECK(received.back().Modifiers == std::vector<std::string>{"shift", "control"});

	CHECK(Call(surface, "emulate_mouse_wheel", {{"notches", -2.0}}, failed)["queued"]);
	REQUIRE_FALSE(failed);
	CHECK(received.back().Kind == InputAutomationKind::MouseWheel);
	CHECK(received.back().Wheel == -2.0f);

	CHECK(Call(surface, "emulate_text", {{"text", "h\xC3\xA9"}}, failed)["queued"]);
	REQUIRE_FALSE(failed);
	CHECK(received.back().Kind == InputAutomationKind::Text);
	CHECK(received.back().Text == "h\xC3\xA9");

	const size_t delivered = received.size();
	(void)Call(surface, "emulate_click", {{"x", 1.0}, {"y", 2.0}, {"button", 1}}, failed);
	CHECK(failed);
	CHECK(received.size() == delivered);
	(void)Call(surface, "emulate_click", {{"x", 1.0}, {"y", 2.0}, {"down", "yes"}}, failed);
	CHECK(failed);
	CHECK(received.size() == delivered);
	(void)Call(
		surface, "emulate_key", {{"key", "A"}, {"modifiers", json::array({"shift", "shift"})}}, failed
	);
	CHECK(failed);
	CHECK(received.size() == delivered);
	(void)Call(surface, "emulate_key", {{"key", "A"}, {"modifiers", "shift"}}, failed);
	CHECK(failed);
	CHECK(received.size() == delivered);
	(void)Call(surface, "emulate_text", {{"text", ""}}, failed);
	CHECK(failed);
	CHECK(received.size() == delivered);
}
