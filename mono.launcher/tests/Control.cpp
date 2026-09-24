#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <launcher/Launcher.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

TEST_SUITE_ID("launcher.control")

namespace launcher {
	struct LauncherControlProbe {
		static nlohmann::json Answer(Launcher &launcher, const nlohmann::json &request) {
			return nlohmann::json::parse(launcher.ControlSurface.Answer(request.dump()));
		}
		static void Frame(Launcher &launcher) {
			launcher.Frame(1.0f / 60.0f);
		}
		static void Pump(Launcher &launcher) {
			launcher.PumpControl();
		}
	};
}

namespace {
	using nlohmann::json;

	json
	Ask(launcher::Launcher &launcher, int id, std::string_view method, json parameters = json::object()) {
		return launcher::LauncherControlProbe::Answer(
			launcher,
			json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(parameters)}}
		);
	}

	json Call(launcher::Launcher &launcher, int id, const char *name, json arguments = json::object()) {
		return Ask(launcher, id, "tools/call", {{"name", name}, {"arguments", std::move(arguments)}});
	}

	json Value(const json &response) {
		REQUIRE_FALSE(response.at("result").value("isError", false));
		return json::parse(response.at("result").at("content").at(0).at("text").get<std::string>());
	}

	json Fixture() {
		std::ifstream input(std::filesystem::path(__FILE__).parent_path() / "fixtures" / "McpContracts.json");
		REQUIRE(input.good());
		return json::parse(input);
	}

	json StableInitialize(json value) {
		value["serverInfo"].erase("version");
		return value;
	}

	json StableNegotiation(json value) {
		value.erase("control_generation");
		value.erase("engine_version");
		return value;
	}
}

TEST_CASE("headless launcher MCP manifest is the reviewed contract", "[launcher][mcp]") {
	launcher::Launcher launcher;
	launcher::Options options;
	options.Headless = true;
	options.ControlPort = 0;
	REQUIRE(launcher.Initialise(options));
	const json expected = Fixture();

	CHECK(StableInitialize(Ask(launcher, 1, "initialize").at("result")) == expected.at("initialize"));
	CHECK(Ask(launcher, 2, "tools/list").at("result").at("tools") == expected.at("tools"));
	CHECK(Ask(launcher, 3, "resources/list").at("result").at("resources") == expected.at("resources"));
	CHECK(Ask(launcher, 4, "prompts/list").at("result").at("prompts") == expected.at("prompts"));
	CHECK(
		StableNegotiation(
			Value(Call(launcher, 5, "negotiate", {{"requested_channels", {"rgb_linear_hdr"}}}))
		) == expected.at("negotiate")
	);
}

TEST_CASE("headless launcher MCP controls its mode and exits cleanly", "[launcher][mcp]") {
	launcher::Launcher launcher;
	launcher::Options options;
	options.Headless = true;
	options.ControlPort = 0;
	REQUIRE(launcher.Initialise(options));

	CHECK(Value(Call(launcher, 10, "emulate_click", {{"x", 5}, {"y", 5}})).at("queued") == true);
	launcher::LauncherControlProbe::Frame(launcher);
	CHECK(ImGui::IsMouseDown(ImGuiMouseButton_Left));
	launcher::LauncherControlProbe::Pump(launcher);
	launcher::LauncherControlProbe::Frame(launcher);
	CHECK_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Left));
	CHECK(Value(Call(launcher, 11, "emulate_key", {{"key", "F5"}})).at("queued") == true);
	launcher::LauncherControlProbe::Frame(launcher);
	CHECK(ImGui::IsKeyDown(ImGuiKey_F5));
	launcher::LauncherControlProbe::Pump(launcher);
	launcher::LauncherControlProbe::Frame(launcher);
	CHECK_FALSE(ImGui::IsKeyDown(ImGuiKey_F5));
	CHECK(Value(Call(launcher, 12, "emulate_text", {{"text", "headless control"}})).at("queued") == true);
	launcher::LauncherControlProbe::Frame(launcher);
	launcher::LauncherControlProbe::Pump(launcher);

	const json status = Value(Call(launcher, 2, "launcher_status"));
	CHECK(status.at("child_state") == "idle");
	CHECK(status.at("open_mode") == "");
	REQUIRE_FALSE(status.at("modes").empty());

	for (const json &mode : status.at("modes")) {
		if (!mode.at("available").get<bool>()) continue;
		const std::string id = mode.at("id").get<std::string>();
		CHECK(Value(Call(launcher, 3, "launcher_open_mode", {{"mode", id}})).at("open_mode") == id);
		const json changed =
			Call(launcher, 4, "launcher_set_option", {{"name", "mcp-port"}, {"values", {"8799"}}});
		CHECK_FALSE(changed.at("result").value("isError", false));
		const json configured = Value(Call(launcher, 5, "launcher_status"));
		const json &commandLine = configured.at("command_line");
		CHECK(std::find(commandLine.begin(), commandLine.end(), json("--mcp-port")) != commandLine.end());
		CHECK(std::find(commandLine.begin(), commandLine.end(), json("8799")) != commandLine.end());
		break;
	}

	CHECK(Value(Call(launcher, 6, "launcher_quit")).at("quitting") == true);
	CHECK(launcher.Run() == 0);
}
