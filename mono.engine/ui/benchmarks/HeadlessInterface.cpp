// Measures headless editor widget layout and the retained geometry signature.

#include <engine/render/Renderer.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/ui/Interface.hpp>

#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <stdexcept>

TEST_SUITE_ID("engine.ui.bench.headless-interface")

namespace {
	struct InterfaceFixture {
		engine::render::Renderer Renderer;
		engine::ui::Interface Interface;

		InterfaceFixture() {
			engine::ui::InterfaceSettings settings;
			settings.Docking = false;
			settings.DisplayWidth = 1600;
			settings.DisplayHeight = 900;
			if (!Interface.Initialise(Renderer, nullptr, settings)) {
				throw std::runtime_error("could not initialise headless UI benchmark");
			}
		}

		uint64_t Frame(const size_t controlCount) {
			Interface.Begin(1.0f / 60.0f);
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(1560.0f, 860.0f), ImGuiCond_Always);
			ImGui::Begin(
				"Asset browser benchmark",
				nullptr,
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
					ImGuiWindowFlags_NoSavedSettings
			);
			if (ImGui::BeginTable("assets", 12, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
				for (size_t index = 0; index < controlCount; ++index) {
					ImGui::TableNextColumn();
					ImGui::PushID(static_cast<int>(index));
					ImGui::Text("Asset %03zu", index);
					ImGui::SameLine();
					ImGui::Button("Open", ImVec2(48.0f, 20.0f));
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			ImGui::End();
			Interface.End();
			const ImDrawData *draw = ImGui::GetDrawData();
			if (draw == nullptr || draw->TotalVtxCount < static_cast<int>(controlCount * 4)) {
				throw std::runtime_error("headless UI benchmark did not draw every control");
			}
			return Interface.Signature();
		}
	};

	InterfaceFixture &Fixture() {
		static InterfaceFixture fixture;
		return fixture;
	}
}

BENCH_PER_ITEM("headless UI frame and geometry signature, 96 asset controls", 96) {
	engine::testing::Consume(Fixture().Frame(96));
}
