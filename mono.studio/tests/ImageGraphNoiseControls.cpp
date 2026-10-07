#include "../src/ImageGraphNoiseControls.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <string_view>
#include <studio/ImageGraph.hpp>
#include <utility>

TEST_SUITE_ID("studio.imagegraph.noise_controls")

namespace {
	using engine::imagegraph::EnumValue;
	using engine::imagegraph::Node;

	struct NoiseChoiceFrame {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		std::optional<bool> Result;
		ImVec2 Center{};
		bool Changed = false;

		NoiseChoiceFrame() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {600, 400};
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~NoiseChoiceFrame() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
		}

		void Draw(std::string_view type, std::string_view property, EnumValue &value) {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({320, 120});
			ImGui::Begin(
				"Noise choice",
				nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
			);
			Result = studio::detail::DrawImageGraphNoiseChoice(type, property, value);
			Changed = Changed || Result == true;
			const auto minimum = ImGui::GetItemRectMin();
			const auto maximum = ImGui::GetItemRectMax();
			Center = {(minimum.x + maximum.x) * .5f, (minimum.y + maximum.y) * .5f};
			ImGui::End();
			ImGui::Render();
		}

		void Change(std::string_view type, std::string_view property, EnumValue &value, unsigned downCount) {
			Changed = false;
			Draw(type, property, value);
			auto &io = ImGui::GetIO();
			io.AddMousePosEvent(Center.x, Center.y);
			Draw(type, property, value);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			Draw(type, property, value);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			Draw(type, property, value);
			for (unsigned index = 0; index < downCount; ++index) {
				io.AddKeyEvent(ImGuiKey_DownArrow, true);
				Draw(type, property, value);
				io.AddKeyEvent(ImGuiKey_DownArrow, false);
				Draw(type, property, value);
			}
			io.AddKeyEvent(ImGuiKey_Enter, true);
			Draw(type, property, value);
			io.AddKeyEvent(ImGuiKey_Enter, false);
			Draw(type, property, value);
		}
	};
}

TEST_CASE("Noise selectors are explicit static choices and preserve their defaults", "[studio][imagegraph]") {
	CHECK(studio::detail::IsImageGraphNoiseSelector("value.noise_field", "mode"));
	CHECK(studio::detail::IsImageGraphNoiseSelector("value.noise_field", "dimension"));
	CHECK(studio::detail::IsImageGraphNoiseSelector("value.noise_field", "output_type"));
	CHECK(studio::detail::IsImageGraphNoiseSelector("value.sample_noise", "output_type"));
	CHECK_FALSE(studio::detail::IsImageGraphNoiseSelector("value.sample_noise", "dimension"));
	CHECK_FALSE(studio::detail::IsImageGraphNoiseSelector("value.other", "output_type"));

	NoiseChoiceFrame ui;
	EnumValue mode{0}, dimension{2}, output{1};
	ui.Draw("value.noise_field", "mode", mode);
	CHECK(ui.Result == std::optional<bool>{false});
	CHECK(mode.Value == 0);
	ui.Draw("value.noise_field", "dimension", dimension);
	CHECK(ui.Result == std::optional<bool>{false});
	CHECK(dimension.Value == 2);
	ui.Draw("value.sample_noise", "output_type", output);
	CHECK(ui.Result == std::optional<bool>{false});
	CHECK(output.Value == 1);

	CHECK_FALSE(studio::detail::DrawImageGraphNoiseChoice("value.other", "mode", mode).has_value());
	CHECK_FALSE(
		studio::detail::DrawImageGraphNoiseChoice("value.sample_noise", "dimension", dimension).has_value()
	);
}

TEST_CASE("Noise selectors change through the rendered ImGui combo", "[studio][imagegraph]") {
	NoiseChoiceFrame ui;
	EnumValue mode{0}, dimension{1}, fieldOutput{1}, sampleOutput{1};

	ui.Change("value.noise_field", "mode", mode, 1);
	CHECK(mode.Value == 1);
	CHECK(ui.Changed);

	ui.Change("value.noise_field", "dimension", dimension, 2);
	CHECK(dimension.Value == 3);
	CHECK(ui.Changed);

	ui.Change("value.noise_field", "output_type", fieldOutput, 2);
	CHECK(fieldOutput.Value == 3);
	CHECK(ui.Changed);

	ui.Change("value.sample_noise", "output_type", sampleOutput, 1);
	CHECK(sampleOutput.Value == 2);
	CHECK(ui.Changed);
}

TEST_CASE("Noise property visibility hides generator position only", "[studio][imagegraph]") {
	Node field;
	field.Type = "value.noise_field";
	field.Values = {{"mode", EnumValue{0}}};
	CHECK_FALSE(studio::detail::ImageGraphNoisePropertyVisible(field, "position"));
	CHECK(studio::detail::ImageGraphNoisePropertyVisible(field, "frequency"));

	field.Values.front().Data = EnumValue{1};
	CHECK(studio::detail::ImageGraphNoisePropertyVisible(field, "position"));

	Node sample;
	sample.Type = "value.sample_noise";
	CHECK(studio::detail::ImageGraphNoisePropertyVisible(sample, "position"));
}

TEST_CASE(
	"Computed noise supplies a zero position matching each selected dimension", "[studio][imagegraph]"
) {
	using namespace engine::imagegraph;
	for (int64_t dimension = 1; dimension <= 3; ++dimension) {
		DYNAMIC_SECTION("dimension " << dimension) {
			Node node{
				"noise",
				"value.noise_field",
				"",
				{},
				{{"mode", EnumValue{1}}, {"dimension", EnumValue{dimension}}, {"output_type", EnumValue{1}}}
			};
			const auto position = studio::detail::ImageGraphNoisePositionDefault(node);
			REQUIRE(position);
			if (dimension == 1) CHECK(*position == Value{0.0});
			if (dimension == 2) CHECK(*position == Value{Vector2{}});
			if (dimension == 3) CHECK(*position == Value{Vector3{}});

			Document document;
			document.FormatVersion = 9;
			document.Nodes.push_back(std::move(node));
			document.Outputs = {{"out", "noise", "value"}};
			Diagnostic diagnostic;
			REQUIRE(studio::SetImageGraphValue(document, "noise", "position", *position, diagnostic));
			Plan plan;
			REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
			EvaluatedValue result;
			REQUIRE(EvaluateValue(document, plan, "out", {}, result, diagnostic) == Status::Ok);
			CHECK(std::holds_alternative<double>(result.Data));
		}
	}
}
