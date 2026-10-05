#include "../src/ImageGraphCacheControls.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>
#include <nodegraph/Editor.hpp>
#include <nodegraph/Registry.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("studio.imagegraph.cache_controls")

namespace {
	using namespace engine::imagegraph;

	struct CacheControlsFrame {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document Authored;
		studio::detail::ImageGraphCacheGroupEdit Edit;
		CacheGroupReplayState Groups;
		nodegraph::Graph CanvasGraph;
		nodegraph::Canvas Canvas;
		std::optional<bool> Request;
		ImVec2 CheckboxCenter, EditButton, CanvasOrigin;

		CacheControlsFrame() {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {1000, 700};
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			Authored.FormatVersion = 9;
			Node owner;
			owner.Id = "cache";
			owner.Type = "pc.cache";
			owner.SourceProperties = {{"serialize", false}};
			Node member;
			member.Id = "producer";
			member.Type = "pc.solid";
			Authored.Nodes = {owner, member};

			nodegraph::NodeType type;
			type.Id = "fixture.cache-control";
			type.Title = "Cache control target";
			nodegraph::NodeTypes::Register(type);
		}
		~CacheControlsFrame() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
		}

		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({300, 190});
			ImGui::Begin(
				"Cache controls",
				nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
			);
			const ImVec2 checkbox = ImGui::GetCursorScreenPos();
			const float checkboxSide = ImGui::GetFrameHeight();
			CheckboxCenter = {checkbox.x + checkboxSide * 0.5f, checkbox.y + checkboxSide * 0.5f};
			Request = studio::detail::DrawImageGraphCacheControls(Authored.Nodes[0], Edit, Groups);
			if (Edit.OwnerId.empty()) {
				const ImVec2 minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
				EditButton = {(minimum.x + maximum.x) * 0.5f, (minimum.y + maximum.y) * 0.5f};
			}
			ImGui::End();

			ImGui::SetNextWindowPos({350, 20});
			ImGui::SetNextWindowSize({400, 360});
			ImGui::Begin(
				"Cache members",
				nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
			);
			CanvasOrigin = ImGui::GetCursorScreenPos();
			Canvas.Draw(CanvasGraph);
			ImGui::End();
			ImGui::Render();
		}

		std::optional<bool> Click(ImVec2 point) {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.AddMousePosEvent(point.x, point.y);
			Frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			Frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			Frame();
			return Request;
		}

		void ClickCanvasNode(nodegraph::NodeId id) {
			const auto *node = CanvasGraph.Find(id);
			const auto layout = nodegraph::LayoutOf(*node);
			const ImVec2 point{
				CanvasOrigin.x + node->X + layout.Width * 0.5f,
				CanvasOrigin.y + node->Y + layout.Height * 0.5f
			};
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.AddMousePosEvent(point.x, point.y);
			Frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			Frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			Frame();
		}
	};
}

TEST_CASE(
	"Serialize checkbox returns a request without editing the document", "[studio][imagegraph][cache]"
) {
	CacheControlsFrame ui;
	const Document before = ui.Authored;
	ui.Frame();
	const auto request = ui.Click(ui.CheckboxCenter);
	REQUIRE(request.has_value());
	CHECK(*request);
	CHECK(ui.Authored == before);
	CHECK(ui.Edit.OwnerId.empty());
}

TEST_CASE(
	"Edit group button queues member clicks without changing canvas selection", "[studio][imagegraph][cache]"
) {
	CacheControlsFrame ui;
	const auto ownerCanvasId = ui.CanvasGraph.Add("fixture.cache-control", 20, 20);
	const auto memberCanvasId = ui.CanvasGraph.Add("fixture.cache-control", 220, 20);
	ui.Canvas.Select(memberCanvasId);
	ui.Canvas.Signals.ClickNode = [&](nodegraph::NodeId id) {
		ui.Edit.QueueClick(id == ownerCanvasId ? "cache" : "producer");
		return true;
	};
	ui.Frame();
	const auto beforeSelection = ui.Canvas.Selection();
	(void)ui.Click(ui.EditButton);
	CHECK(ui.Edit.OwnerId == "cache");
	CHECK(ui.Canvas.Selection() == beforeSelection);
	CHECK(ui.CanvasGraph.Find(ownerCanvasId) != nullptr);

	ui.Frame();
	ui.ClickCanvasNode(memberCanvasId);
	CHECK(ui.Edit.OwnerId == "cache");
	CHECK(ui.Edit.PendingMember == "producer");
	CHECK(ui.Canvas.Selection() == beforeSelection);

	ui.ClickCanvasNode(ownerCanvasId);
	CHECK(ui.Edit.OwnerId.empty());
	CHECK(ui.Edit.PendingMember.empty());
	CHECK(ui.Canvas.Selection() == beforeSelection);
}

TEST_CASE("cache group edit drops owners that disappear or change type", "[studio][imagegraph][cache]") {
	CacheControlsFrame ui;
	ui.Edit.Toggle(ui.Authored.Nodes[0]);
	Document missing = ui.Authored;
	missing.Nodes.erase(missing.Nodes.begin());
	ui.Edit.Reconcile(missing);
	CHECK(ui.Edit.OwnerId.empty());

	ui.Edit.Toggle(ui.Authored.Nodes[0]);
	Document replaced = ui.Authored;
	replaced.Nodes[0].Type = "pc.cache_array";
	ui.Edit.Reconcile(replaced);
	CHECK(ui.Edit.OwnerId.empty());
}
