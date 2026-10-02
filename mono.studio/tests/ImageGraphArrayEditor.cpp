#include "ImageGraphArrayEditor.hpp"

#include "ImageGraphDocumentEdit.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("studio.imagegraph.array_editor")
TEST_DEPENDS("studio.imagegraph")
namespace {
	using namespace engine::imagegraph;
	struct ArrayEditor {
		ImGuiContext *Previous = ImGui::GetCurrentContext();
		ImGuiContext *Context = ImGui::CreateContext();
		Document Doc;
		studio::ImageGraphHistory History;
		size_t Page = 0;
		ImVec2 Action;
		unsigned Changes = 0;
		ArrayEditor(ArrayValue array) {
			ImGui::SetCurrentContext(Context);
			auto &io = ImGui::GetIO();
			io.DisplaySize = {1000, 600};
			io.DeltaTime = 1.f / 60;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			Doc.FormatVersion = 9;
			Doc.Nodes = {{"input", "pc.group_input", "", {}, {{"parent_value", std::move(array)}}}};
		}
		~ArrayEditor() {
			ImGui::SetCurrentContext(Context);
			ImGui::DestroyContext(Context);
			ImGui::SetCurrentContext(Previous);
		}
		const ArrayValue &Value() const {
			return std::get<ArrayValue>(Doc.Nodes[0].Values[0].Data);
		}
		void Frame() {
			ImGui::SetCurrentContext(Context);
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({20, 20});
			ImGui::SetNextWindowSize({900, 500});
			ImGui::Begin("Array inspector", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
			auto replacement = Value();
			bool changed = studio::detail::DrawImageGraphSourceArray(replacement, Page);
			auto lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
			Action = {(lo.x + hi.x) * .5f, (lo.y + hi.y) * .5f};
			ImGui::End();
			ImGui::Render();
			if (changed && studio::ApplyImageGraphDocumentEdit(Doc, History, [&](Document &staged) {
					staged.Nodes[0].Values[0].Data = std::move(replacement);
				}))
				++Changes;
		}
		void ClickLastItem() {
			Frame();
			Frame();
			auto &io = ImGui::GetIO();
			io.AddMousePosEvent(Action.x, Action.y);
			Frame();
			io.AddMouseButtonEvent(0, true);
			Frame();
			io.AddMouseButtonEvent(0, false);
			Frame();
		}
	};
}
TEST_CASE(
	"Any array inspector Add preserves heterogeneous nested items and records one undo",
	"[studio][array_editor]"
) {
	ArrayValue value{
		ValueType::Any,
		{},
		{},
		{SourceArrayItem{ElementValue{std::string{"kept"}}},
		 SourceArrayItem{ElementValue{4.0}},
		 SourceArrayItem{
			 std::vector<SourceArrayItem>{{ElementValue{true}}, {ElementValue{std::string{"nested"}}}}
		 }}
	};
	ArrayEditor ui(value);
	const auto original = ui.Doc;
	ui.ClickLastItem();
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Value().Items.size() == 4);
	for (size_t index = 0; index < value.Items.size(); ++index)
		CHECK(ui.Value().Items[index] == value.Items[index]);
	CHECK(ui.Value().Items.back() == SourceArrayItem{ElementValue{false}});
	auto edited = ui.Doc;
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == original);
	CHECK_FALSE(ui.History.CanUndo());
	REQUIRE(ui.History.Redo(ui.Doc));
	CHECK(ui.Doc == edited);
}
TEST_CASE(
	"Any inspector paginates later items and preserves retained resource payloads", "[studio][array_editor]"
) {
	ArrayValue value;
	value.ElementType = ValueType::Any;
	for (size_t index = 0; index < 70; ++index)
		value.Items.push_back({ElementValue{double(index)}});
	Image image;
	image.Width = 1;
	image.Height = 1;
	image.Pixels = {7, 8, 9, 255};
	value.Items[66] = {image};
	ArrayEditor ui(value);
	ui.Page = 64;
	ui.ClickLastItem();
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Value().Items.size() == 71);
	CHECK(ui.Page == 64);
	for (size_t index = 0; index < value.Items.size(); ++index)
		CHECK(ui.Value().Items[index] == value.Items[index]);
}
TEST_CASE(
	"nested Boolean array inspector edits leaf and restores exact shape with undo", "[studio][array_editor]"
) {
	ArrayValue value{
		ValueType::Boolean, {}, {{ElementValue{true}, ElementValue{false}}, {ElementValue{true}}}, {}
	};
	ArrayEditor ui(value);
	const auto before = ui.Doc;
	ui.Frame();
	ui.Frame();
	CHECK(ui.Value() == value);
	CHECK_FALSE(ui.History.CanUndo());
	ui.ClickLastItem();
	REQUIRE(ui.Changes == 1);
	REQUIRE(ui.Value().Nested.size() == 2);
	REQUIRE(ui.Value().Nested[0].size() == 2);
	REQUIRE(ui.Value().Nested[1].size() == 1);
	CHECK(ui.Value().Nested[0] == value.Nested[0]);
	CHECK(ui.Value().Nested[1][0] == ElementValue{false});
	CHECK(ui.Value().Items.empty());
	REQUIRE(ui.History.Undo(ui.Doc));
	CHECK(ui.Doc == before);
	CHECK_FALSE(ui.History.CanUndo());
}
