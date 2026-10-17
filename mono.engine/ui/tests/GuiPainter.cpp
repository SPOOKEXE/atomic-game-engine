#include <engine/core/Paths.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/ShapedText.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/ui/Fonts.hpp>
#include <engine/ui/GuiPainter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <limits>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.ui.guipainter")

namespace {
	void SetLinearSampler(const ImDrawList *, const ImDrawCmd *) {}
	void SetNearestSampler(const ImDrawList *, const ImDrawCmd *) {}

	struct ImGuiContextScope {
		ImGuiContextScope() {
			ImGui::CreateContext();
			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(800.0f, 600.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.Fonts->AddFontDefault();
			engine::ui::LoadFonts(1.0f);
			io.Fonts->Build();
			ImGui::NewFrame();
		}

		~ImGuiContextScope() {
			engine::ui::ShutdownGuiPainter();
			ImGui::EndFrame();
			ImGui::DestroyContext();
		}
	};

	engine::gui::DrawCommand Command(engine::gui::DrawKind kind) {
		engine::gui::DrawCommand command;
		command.Kind = kind;
		command.Bounds = engine::core::Rect{{10.0f, 20.0f}, {210.0f, 80.0f}};
		command.Clip = engine::core::Rect{{0.0f, 0.0f}, {800.0f, 600.0f}};
		return command;
	}

	engine::gui::FontPackage StagedFontPackage(std::string_view name = "fonts/Inter.ttf") {
		std::ifstream file(engine::core::Paths::Fonts() / "Inter.ttf", std::ios::binary | std::ios::ate);
		REQUIRE(file);
		const std::streamsize size = file.tellg();
		REQUIRE(size > 0);
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		file.seekg(0);
		REQUIRE(file.read(reinterpret_cast<char *>(bytes.data()), size));
		engine::gui::FontPackage package;
		REQUIRE(package.Add(engine::core::Name(name), engine::gui::FontFace::Regular, bytes));
		return package;
	}

	bool InRoundedBounds(const ImVec2 &point, float minX, float minY, float maxX, float maxY, float radius) {
		if (point.x < minX - 0.01f || point.x > maxX + 0.01f || point.y < minY - 0.01f ||
			point.y > maxY + 0.01f) {
			return false;
		}
		const float nearestX = std::clamp(point.x, minX + radius, maxX - radius);
		const float nearestY = std::clamp(point.y, minY + radius, maxY - radius);
		const float x = point.x - nearestX;
		const float y = point.y - nearestY;
		return x * x + y * y <= radius * radius + 0.1f;
	}
}

TEST_CASE(
	"the studio painter consumes nested rounded mask operations as clipped geometry", "[ui][guipainter][mask]"
) {
	const ImGuiContextScope context;
	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;
	auto command = Command(engine::gui::DrawKind::Rectangle);
	command.Bounds = {{0.0f, 0.0f}, {100.0f, 100.0f}};
	command.Clip = {{0.0f, 0.0f}, {100.0f, 100.0f}};
	list.Commands.push_back(command);
	list.Operations = {
		{engine::gui::DrawOperationKind::BeginMask,
		 engine::ecs::Entity{91},
		 {},
		 0,
		 {{20.0f, 20.0f}, {80.0f, 80.0f}},
		 {{0.0f, 0.0f}, {100.0f, 100.0f}},
		 20.0f},
		{engine::gui::DrawOperationKind::BeginMask,
		 engine::ecs::Entity{92},
		 {},
		 0,
		 {{30.0f, 30.0f}, {70.0f, 70.0f}},
		 {{0.0f, 0.0f}, {100.0f, 100.0f}},
		 10.0f},
		{engine::gui::DrawOperationKind::EndMask, engine::ecs::Entity{92}, {}, 1, {}, {}, 0.0f, 0.0f},
		{engine::gui::DrawOperationKind::EndMask, engine::ecs::Entity{91}, {}, 1, {}, {}, 0.0f, 0.0f},
	};

	const int before = target->VtxBuffer.Size;
	CHECK(engine::ui::PaintGui(list, target, {}, {}) == 1);
	REQUIRE(target->VtxBuffer.Size > before);
	for (const ImDrawCmd &draw : target->CmdBuffer) {
		for (unsigned int index = 0; index < draw.ElemCount; index++) {
			const unsigned int vertex = draw.VtxOffset + target->IdxBuffer[draw.IdxOffset + index];
			CHECK(InRoundedBounds(target->VtxBuffer[vertex].pos, 30.0f, 30.0f, 70.0f, 70.0f, 10.0f));
		}
	}
}

TEST_CASE(
	"the studio painter composites overlapping group children before applying group alpha",
	"[ui][guipainter][group]"
) {
	const ImGuiContextScope context;
	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;
	auto first = Command(engine::gui::DrawKind::Rectangle);
	first.Bounds = {{20.0f, 20.0f}, {100.0f, 100.0f}};
	first.Clip = {{0.0f, 0.0f}, {200.0f, 200.0f}};
	first.Tint = {1.0f, 0.0f, 0.0f};
	auto second = first;
	second.Bounds = {{60.0f, 20.0f}, {140.0f, 100.0f}};
	second.Tint = {0.0f, 1.0f, 0.0f};
	list.Commands = {first, second};
	list.Operations = {
		{engine::gui::DrawOperationKind::BeginGroup,
		 engine::ecs::Entity{31},
		 {},
		 0,
		 {{20.0f, 20.0f}, {140.0f, 100.0f}},
		 {{0.0f, 0.0f}, {200.0f, 200.0f}},
		 0.0f,
		 0.5f},
		{engine::gui::DrawOperationKind::EndGroup, engine::ecs::Entity{31}, {}, 2, {}, {}, 0.0f, 0.0f},
	};

	const int before = target->VtxBuffer.Size;
	CHECK(engine::ui::PaintGui(list, target, {}, {}) == 2);
	REQUIRE(target->VtxBuffer.Size > before);
	CHECK((target->VtxBuffer[before].col >> IM_COL32_A_SHIFT & 0xFFu) == 128u);
	ImTextureData *texture = nullptr;
	for (const ImDrawCmd &draw : target->CmdBuffer) {
		if (draw.ElemCount > 0 && draw.TexRef._TexData != nullptr &&
			draw.TexRef._TexData != ImGui::GetIO().Fonts->TexData) {
			texture = draw.TexRef._TexData;
			break;
		}
	}
	REQUIRE(texture != nullptr);
	const uint8_t *overlap = texture->Pixels + (40 * texture->Width + 60) * 4;
	CHECK(overlap[3] == 255u);
	CHECK(overlap[1] > overlap[0]);
}

TEST_CASE(
	"the studio painter nests group textures and honours a surrounding rounded mask",
	"[ui][guipainter][group]"
) {
	const ImGuiContextScope context;
	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;
	auto command = Command(engine::gui::DrawKind::Rectangle);
	command.Bounds = {{0.0f, 0.0f}, {100.0f, 100.0f}};
	command.Clip = {{0.0f, 0.0f}, {100.0f, 100.0f}};
	list.Commands.push_back(command);
	list.Operations = {
		{engine::gui::DrawOperationKind::BeginMask,
		 engine::ecs::Entity{43},
		 {},
		 0,
		 {{10.0f, 10.0f}, {90.0f, 90.0f}},
		 {{0.0f, 0.0f}, {100.0f, 100.0f}},
		 16.0f},
		{engine::gui::DrawOperationKind::BeginGroup,
		 engine::ecs::Entity{41},
		 {},
		 0,
		 {{0.0f, 0.0f}, {100.0f, 100.0f}},
		 {{0.0f, 0.0f}, {100.0f, 100.0f}},
		 0.0f,
		 0.25f},
		{engine::gui::DrawOperationKind::BeginGroup,
		 engine::ecs::Entity{42},
		 {},
		 0,
		 {{0.0f, 0.0f}, {100.0f, 100.0f}},
		 {{0.0f, 0.0f}, {100.0f, 100.0f}},
		 0.0f,
		 0.5f},
		{engine::gui::DrawOperationKind::EndGroup, engine::ecs::Entity{42}, {}, 1, {}, {}, 0.0f, 0.0f},
		{engine::gui::DrawOperationKind::EndGroup, engine::ecs::Entity{41}, {}, 1, {}, {}, 0.0f, 0.0f},
		{engine::gui::DrawOperationKind::EndMask, engine::ecs::Entity{43}, {}, 1, {}, {}, 0.0f, 0.0f},
	};

	CHECK(engine::ui::PaintGui(list, target, {}, {}) == 1);
	REQUIRE(target->CmdBuffer.Size >= 1);
	// Distinct opacities prove that the outer same-command group owns the
	// nested boundary. Selecting only the inner group would leave 128 here.
	const uint8_t outerAlpha = static_cast<uint8_t>(target->VtxBuffer[0].col >> IM_COL32_A_SHIFT & 0xFFu);
	CHECK(outerAlpha == 191u);
	ImTextureData *outerTexture = nullptr;
	for (const ImDrawCmd &draw : target->CmdBuffer) {
		if (draw.TexRef._TexData != nullptr && draw.TexRef._TexData != ImGui::GetIO().Fonts->TexData) {
			outerTexture = draw.TexRef._TexData;
			break;
		}
	}
	REQUIRE(outerTexture != nullptr);
	REQUIRE(outerTexture->Pixels != nullptr);
	const int sampleX = outerTexture->Width / 2;
	const int sampleY = outerTexture->Height / 2;
	const uint8_t outerTextureAlpha = outerTexture->Pixels[(sampleY * outerTexture->Width + sampleX) * 4 + 3];
	// The outer target contains the inner 0.5 opacity. The target draw quad
	// then applies the outer 0.75 opacity once to that composited texture.
	CHECK(outerTextureAlpha == 128u);
	CHECK((static_cast<unsigned int>(outerTextureAlpha) * outerAlpha + 127u) / 255u == 96u);
	for (const ImDrawCmd &draw : target->CmdBuffer) {
		for (unsigned int index = 0; index < draw.ElemCount; index++) {
			const unsigned int vertex = draw.VtxOffset + target->IdxBuffer[draw.IdxOffset + index];
			CHECK(InRoundedBounds(target->VtxBuffer[vertex].pos, 10.0f, 10.0f, 90.0f, 90.0f, 16.0f));
		}
	}
}

TEST_CASE(
	"the studio painter composites raw image handles before applying group alpha", "[ui][guipainter][group]"
) {
	const ImGuiContextScope context;
	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;
	auto first = Command(engine::gui::DrawKind::Image);
	first.Bounds = {{0.0f, 0.0f}, {80.0f, 40.0f}};
	first.Image = engine::core::Name("group-image-a");
	auto second = first;
	second.Bounds = {{20.0f, 0.0f}, {100.0f, 40.0f}};
	second.Image = engine::core::Name("group-image-b");
	list.Commands = {first, second};
	list.Operations = {
		{engine::gui::DrawOperationKind::BeginGroup,
		 engine::ecs::Entity{50},
		 {},
		 0,
		 {{0.0f, 0.0f}, {100.0f, 40.0f}},
		 {{0.0f, 0.0f}, {100.0f, 40.0f}},
		 0.0f,
		 0.5f},
		{engine::gui::DrawOperationKind::EndGroup, engine::ecs::Entity{50}, {}, 2, {}, {}, 0.0f, 0.0f},
	};
	engine::ui::ImageSource images;
	images.Resolve = [&](const engine::core::Name &) {
		engine::ui::ImageSource::Resolved resolved;
		resolved.Texture = static_cast<ImTextureID>(1234);
		resolved.Size = {80.0f, 40.0f};
		return resolved;
	};
	images.Sample =
		[](ImTextureID texture, const ImVec2 &, float &red, float &green, float &blue, float &alpha) {
			if (texture != static_cast<ImTextureID>(1234)) return false;
			red = 1.0f;
			green = 0.0f;
			blue = 0.0f;
			alpha = 0.5f;
			return true;
		};

	CHECK(engine::ui::PaintGui(list, target, {}, images) == 2);
	REQUIRE(target->VtxBuffer.Size >= 4);
	CHECK((target->VtxBuffer[0].col >> IM_COL32_A_SHIFT & 0xFFu) == 128u);
	ImTextureData *texture = nullptr;
	for (const ImDrawCmd &draw : target->CmdBuffer) {
		if (draw.ElemCount > 0 && draw.TexRef._TexData != nullptr &&
			draw.TexRef._TexData != ImGui::GetIO().Fonts->TexData) {
			texture = draw.TexRef._TexData;
			break;
		}
	}
	REQUIRE(texture != nullptr);
	const uint8_t *overlap = texture->Pixels + (20 * texture->Width + 50) * 4;
	CHECK(overlap[0] == 255u);
	CHECK(overlap[3] == 191u);
}

TEST_CASE(
	"the studio painter refuses an oversized group target without dropping its children",
	"[ui][guipainter][group]"
) {
	const ImGuiContextScope context;
	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;
	auto command = Command(engine::gui::DrawKind::Rectangle);
	command.Bounds = {{0.0f, 0.0f}, {4097.0f, 4097.0f}};
	command.Clip = command.Bounds;
	list.Commands.push_back(command);
	list.Operations = {
		{engine::gui::DrawOperationKind::BeginGroup,
		 engine::ecs::Entity{60},
		 {},
		 0,
		 command.Bounds,
		 command.Clip,
		 0.0f,
		 0.5f},
		{engine::gui::DrawOperationKind::EndGroup, engine::ecs::Entity{60}, {}, 1, {}, {}, 0.0f, 0.0f},
	};

	CHECK(engine::ui::PaintGui(list, target, {}, {}) == 1);
	REQUIRE(target->VtxBuffer.Size >= 4);
	CHECK((target->VtxBuffer[0].col >> IM_COL32_A_SHIFT & 0xFFu) == 128u);
}

TEST_CASE("the studio painter retains an unchanged CanvasGroup target", "[ui][guipainter][group]") {
	const ImGuiContextScope context;
	engine::gui::DrawList list;
	auto command = Command(engine::gui::DrawKind::Image);
	command.Bounds = {{0.0f, 0.0f}, {40.0f, 40.0f}};
	command.Clip = command.Bounds;
	command.Image = engine::core::Name("retained-group-image");
	list.Commands.push_back(command);
	list.Operations = {
		{engine::gui::DrawOperationKind::BeginGroup,
		 engine::ecs::Entity{61},
		 {},
		 0,
		 command.Bounds,
		 command.Clip,
		 0.0f,
		 0.0f},
		{engine::gui::DrawOperationKind::EndGroup, engine::ecs::Entity{61}, {}, 1, {}, {}, 0.0f, 0.0f},
	};
	engine::ui::ImageSource images;
	images.CompiledSignature = 42;
	images.Revision = 7;
	images.Resolve = [](const engine::core::Name &) {
		engine::ui::ImageSource::Resolved image;
		image.Texture = static_cast<ImTextureID>(5432);
		image.Size = ImVec2{40.0f, 40.0f};
		return image;
	};
	size_t samples = 0;
	images.Sample =
		[&samples](ImTextureID, const ImVec2 &, float &red, float &green, float &blue, float &alpha) {
			samples++;
			red = green = blue = alpha = 1.0f;
			return true;
		};
	ImDrawList *target = ImGui::GetBackgroundDrawList();
	CHECK(engine::ui::PaintGui(list, target, {}, images) == 1);
	REQUIRE(samples > 0);
	const size_t samplesBeforeUnchangedFrame = samples;
	ImGui::EndFrame();
	ImGui::NewFrame();
	target = ImGui::GetBackgroundDrawList();
	CHECK(engine::ui::PaintGui(list, target, {}, images) == 1);
	CHECK(samples - samplesBeforeUnchangedFrame == 0);
}

TEST_CASE(
	"the studio painter preserves group alpha when an image source cannot be sampled",
	"[ui][guipainter][group]"
) {
	const ImGuiContextScope context;
	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;
	auto command = Command(engine::gui::DrawKind::Image);
	command.Bounds = {{0.0f, 0.0f}, {40.0f, 40.0f}};
	command.Clip = command.Bounds;
	command.Image = engine::core::Name("unavailable-group-image");
	list.Commands.push_back(command);
	list.Operations = {
		{engine::gui::DrawOperationKind::BeginGroup,
		 engine::ecs::Entity{62},
		 {},
		 0,
		 command.Bounds,
		 command.Clip,
		 0.0f,
		 0.5f},
		{engine::gui::DrawOperationKind::EndGroup, engine::ecs::Entity{62}, {}, 1, {}, {}, 0.0f, 0.0f},
	};
	engine::ui::ImageSource images;
	images.Resolve = [](const engine::core::Name &) {
		engine::ui::ImageSource::Resolved image;
		image.Texture = static_cast<ImTextureID>(9988);
		image.Size = ImVec2{40.0f, 40.0f};
		return image;
	};
	images.Sample = [](ImTextureID, const ImVec2 &, float &, float &, float &, float &) { return false; };

	CHECK(engine::ui::PaintGui(list, target, {}, images) == 1);
	REQUIRE(target->VtxBuffer.Size >= 4);
	CHECK((target->VtxBuffer[0].col >> IM_COL32_A_SHIFT & 0xFFu) == 128u);
}

TEST_CASE(
	"the studio painter suppresses a rounded mask when ImGui cannot index its clipped mesh",
	"[ui][guipainter][mask]"
) {
	const ImGuiContextScope context;
	ImDrawList *target = ImGui::GetBackgroundDrawList();
	ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
	const int held = static_cast<int>(std::numeric_limits<ImDrawIdx>::max()) - 8192 + 1;
	target->VtxBuffer.resize(held);
	target->_VtxCurrentIdx = static_cast<unsigned int>(held);
	target->_VtxWritePtr = target->VtxBuffer.Data + target->VtxBuffer.Size;

	engine::gui::DrawList list;
	auto command = Command(engine::gui::DrawKind::Rectangle);
	command.Bounds = {{0.0f, 0.0f}, {100.0f, 100.0f}};
	list.Commands.push_back(command);
	list.Operations = {
		{engine::gui::DrawOperationKind::BeginMask,
		 engine::ecs::Entity{71},
		 {},
		 0,
		 {{20.0f, 20.0f}, {80.0f, 80.0f}},
		 {{0.0f, 0.0f}, {100.0f, 100.0f}},
		 20.0f},
		{engine::gui::DrawOperationKind::EndMask, engine::ecs::Entity{71}, {}, 1, {}, {}, 0.0f, 0.0f},
	};

	CHECK(engine::ui::PaintGui(list, target, {}, {}) == 1);
	for (const ImDrawCmd &draw : target->CmdBuffer)
		CHECK(draw.ElemCount == 0);
}

TEST_CASE("the studio painter resolves viewport images by element", "[ui][guipainter]") {
	const ImGuiContextScope context;
	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;
	auto command = Command(engine::gui::DrawKind::Viewport);
	command.Source = engine::ecs::Entity{17};
	list.Commands.push_back(command);

	bool resolved = false;
	engine::ui::ImageSource images;
	images.ResolveViewport = [&](engine::ecs::Entity source) {
		resolved = source == command.Source;
		engine::ui::ImageSource::Resolved image;
		image.Texture = static_cast<ImTextureID>(1);
		image.Size = ImVec2(200.0f, 60.0f);
		return image;
	};

	CHECK(engine::ui::PaintGui(list, target, {}, images) == 1);
	CHECK(resolved);
	CHECK(target->VtxBuffer.Size == 4);
}

TEST_CASE("text stroke and rotation are emitted by the studio painter", "[ui][guipainter]") {
	const ImGuiContextScope context;
	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;
	auto command = Command(engine::gui::DrawKind::Text);
	command.Text = "a long label that truncates";
	command.TextSize = 16;
	command.Truncate = engine::gui::TextTruncate::AtEnd;
	command.StrokeTransparency = 0.0f;
	command.Rotation = 25.0f;
	list.Commands.push_back(command);

	CHECK(engine::ui::PaintGui(list, target, {}) == 9);
	CHECK(target->VtxBuffer.Size > 4);
	CHECK(target->VtxBuffer[0].pos.y != command.Bounds.Min.Y);
}

TEST_CASE("the studio painter honours compiled shaped glyph positions", "[ui][guipainter][text]") {
	const ImGuiContextScope context;
	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;
	auto command = Command(engine::gui::DrawKind::Text);
	command.Text = "ab";
	command.TextSize = 16;
	command.XAlignment = engine::gui::TextXAlignment::Left;
	command.YAlignment = engine::gui::TextYAlignment::Top;
	command.Shaping.Status = engine::gui::TextShapeStatus::Ok;
	command.Shaping.Advance = 80.0f;
	command.Shaping.GraphemeBoundaries = {0, 1, 2};
	command.Shaping.Lines = {
		{.GlyphOffset = 0, .GlyphCount = 1, .SourceBegin = 0, .SourceEnd = 1, .Advance = 10.0f},
		{.GlyphOffset = 1,
		 .GlyphCount = 1,
		 .SourceBegin = 1,
		 .SourceEnd = 2,
		 .Advance = 80.0f,
		 .Baseline = 16.0f},
	};
	command.Shaping.Glyphs = {
		{engine::core::Name{}, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f},
		{engine::core::Name{}, 0, 1, 64.0f, 0.0f, 0.0f, 0.0f},
	};
	list.Commands.push_back(command);

	CHECK(engine::ui::PaintGui(list, target, {}) == 1);
	REQUIRE(target->VtxBuffer.Size >= 8);
	float rightmost = 0.0f;
	float lowest = 0.0f;
	for (const ImDrawVert &vertex : target->VtxBuffer) {
		rightmost = std::max(rightmost, vertex.pos.x);
		lowest = std::max(lowest, vertex.pos.y);
	}
	CHECK(rightmost > command.Bounds.Min.X);
	CHECK(lowest > command.Bounds.Min.Y);
}

TEST_CASE("the studio painter emits package glyph ids into custom atlas quads", "[ui][guipainter][text]") {
	const ImGuiContextScope context;
	const std::filesystem::path fontPath = engine::core::Paths::Fonts() / "Inter.ttf";
	if (!std::filesystem::exists(fontPath)) {
		SUCCEED("no staged font package");
		return;
	}

	const engine::gui::FontPackage package = StagedFontPackage();
	const engine::gui::ShapedText shaped = engine::gui::ShapeText(
		package, {.Text = "AV", .Role = engine::gui::FontFace::Regular, .PixelSize = 18.0f}
	);
	REQUIRE(shaped.Status == engine::gui::TextShapeStatus::Ok);
	REQUIRE(shaped.Glyphs.size() == 2);
	CHECK(shaped.Glyphs[0].Index != shaped.Glyphs[1].Index);

	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;
	auto command = Command(engine::gui::DrawKind::Text);
	command.Text = "AV";
	command.TextSize = 18;
	command.XAlignment = engine::gui::TextXAlignment::Left;
	command.YAlignment = engine::gui::TextYAlignment::Top;
	command.Shaping = shaped;
	list.Commands.push_back(command);

	engine::ui::ImageSource images;
	images.Fonts = &package;
	CHECK(engine::ui::PaintGui(list, target, {}, images) == 1);
	REQUIRE(target->VtxBuffer.Size >= 8);
	REQUIRE(target->CmdBuffer.Size == 1);
	CHECK(target->CmdBuffer[0].TexRef._TexData == ImGui::GetIO().Fonts->TexData);

	const ImVec2 firstUv = target->VtxBuffer[0].uv;
	bool sawDifferentGlyphUv = false;
	for (int index = 1; index < target->VtxBuffer.Size; index++) {
		const ImVec2 uv = target->VtxBuffer[index].uv;
		sawDifferentGlyphUv |= uv.x != firstUv.x || uv.y != firstUv.y;
	}
	CHECK(sawDifferentGlyphUv);
	CHECK(target->VtxBuffer[0].pos.x < target->VtxBuffer[target->VtxBuffer.Size - 1].pos.x);
}

TEST_CASE("the shaped glyph atlas stays fixed while studio paint records", "[ui][guipainter][text]") {
	const ImGuiContextScope context;
	const std::filesystem::path fontPath = engine::core::Paths::Fonts() / "Inter.ttf";
	if (!std::filesystem::exists(fontPath)) {
		SUCCEED("no staged font package");
		return;
	}

	ImFontAtlasRect reserved;
	REQUIRE(engine::ui::ShapedGlyphAtlasRect(&reserved));
	CHECK(reserved.w == 1024);
	CHECK(reserved.h == 1024);

	const engine::gui::FontPackage package = StagedFontPackage();
	const engine::gui::ShapedText shaped = engine::gui::ShapeText(
		package, {.Text = "AV", .Role = engine::gui::FontFace::Regular, .PixelSize = 18.0f}
	);
	REQUIRE(shaped.Status == engine::gui::TextShapeStatus::Ok);

	ImDrawList *target = ImGui::GetBackgroundDrawList();
	target->AddRectFilled(ImVec2{1.0f, 1.0f}, ImVec2{6.0f, 6.0f}, IM_COL32_WHITE);
	REQUIRE(target->VtxBuffer.Size == 4);
	const ImVec2 chromeUv = target->VtxBuffer[0].uv;
	ImTextureData *const texture = ImGui::GetIO().Fonts->TexData;
	const int width = texture->Width;
	const int height = texture->Height;

	engine::gui::DrawList list;
	auto command = Command(engine::gui::DrawKind::Text);
	command.Text = "AV";
	command.TextSize = 18;
	command.StrokeTransparency = 1.0f;
	command.Shaping = shaped;
	list.Commands.push_back(command);
	engine::ui::ImageSource images;
	images.Fonts = &package;

	CHECK(engine::ui::PaintGui(list, target, {}, images) == 1);
	CHECK(ImGui::GetIO().Fonts->TexData == texture);
	CHECK(texture->Width == width);
	CHECK(texture->Height == height);
	CHECK(target->VtxBuffer[0].uv.x == chromeUv.x);
	CHECK(target->VtxBuffer[0].uv.y == chromeUv.y);
	const int afterFirst = target->VtxBuffer.Size;

	CHECK(engine::ui::PaintGui(list, target, {}, images) == 1);
	CHECK(target->VtxBuffer.Size > afterFirst);
	CHECK(ImGui::GetIO().Fonts->TexData == texture);

	const engine::gui::FontPackage replacement = StagedFontPackage("fonts/reloaded-Inter.ttf");
	const engine::gui::ShapedText reloaded = engine::gui::ShapeText(
		replacement, {.Text = "AV", .Role = engine::gui::FontFace::Regular, .PixelSize = 18.0f}
	);
	REQUIRE(reloaded.Status == engine::gui::TextShapeStatus::Ok);
	list.Commands.front().Shaping = reloaded;
	images.Fonts = &replacement;
	CHECK(engine::ui::PaintGui(list, target, {}, images) == 1);
	CHECK(ImGui::GetIO().Fonts->TexData == texture);
	CHECK(texture->Width == width);
	CHECK(texture->Height == height);

	// A same-frame package replacement must not overwrite already submitted
	// glyphs. At the next frame it may reuse the fixed region, but it must drop
	// the replacement package's old lookup entries with that reset.
	ImGui::EndFrame();
	ImGui::NewFrame();
	target = ImGui::GetBackgroundDrawList();
	const int beforeNextFrame = target->VtxBuffer.Size;
	CHECK(engine::ui::PaintGui(list, target, {}, images) == 1);
	CHECK(target->VtxBuffer.Size > beforeNextFrame);
	CHECK(ImGui::GetIO().Fonts->TexData == texture);
	CHECK(texture->Width == width);
	CHECK(texture->Height == height);

	// The fixed region rejects a maliciously oversized request before the
	// rasterizer allocates a matching coverage bitmap. PaintGui's fallback box
	// is deliberately visible, so the missing glyph is diagnosable in Studio.
	list.Commands.front().Shaping.Glyphs.front().PixelSize = 4096.0f;
	const int beforeFallback = target->VtxBuffer.Size;
	CHECK(engine::ui::PaintGui(list, target, {}, images) == 1);
	CHECK(target->VtxBuffer.Size > beforeFallback);
	CHECK(ImGui::GetIO().Fonts->TexData == texture);
}

TEST_CASE("the studio painter carries every compiled primitive and image resampling", "[ui][guipainter]") {
	const ImGuiContextScope context;
	ImGuiPlatformIO &platform = ImGui::GetPlatformIO();
	platform.DrawCallback_SetSamplerLinear = SetLinearSampler;
	platform.DrawCallback_SetSamplerNearest = SetNearestSampler;

	ImDrawList *target = ImGui::GetBackgroundDrawList();
	engine::gui::DrawList list;

	auto fill = Command(engine::gui::DrawKind::Rectangle);
	fill.Gradient = 0;
	list.Commands.push_back(fill);

	auto outline = Command(engine::gui::DrawKind::Outline);
	outline.Thickness = 3.0f;
	list.Commands.push_back(outline);

	auto image = Command(engine::gui::DrawKind::Image);
	image.Image = engine::core::Name("pixel-art");
	image.Resample = engine::gui::ResampleMode::Pixelated;
	list.Commands.push_back(image);

	auto text = Command(engine::gui::DrawKind::Text);
	text.Text = "bold text";
	text.TextSize = 16;
	engine::gui::DrawSpan bold;
	bold.Begin = 0;
	bold.End = 4;
	bold.Font = engine::gui::FontFace::Bold;
	text.Spans.push_back(bold);
	list.Commands.push_back(text);

	engine::gui::DrawGradient gradient;
	gradient.Color = engine::core::ColorSequence{
		engine::core::Color3{1.0f, 0.0f, 0.0f}, engine::core::Color3{0.0f, 0.0f, 1.0f}
	};
	gradient.Origin = engine::core::Vector2{10.0f, 20.0f};
	gradient.Axis = engine::core::Vector2{200.0f, 0.0f};
	list.Gradients.push_back(gradient);

	engine::ui::ImageSource images;
	images.Resolve = [](const engine::core::Name &) {
		engine::ui::ImageSource::Resolved resolved;
		resolved.Texture = static_cast<ImTextureID>(1);
		resolved.Size = ImVec2{32.0f, 32.0f};
		return resolved;
	};

	CHECK(engine::ui::PaintGui(list, target, {}, images) == 5);
	CHECK(target->VtxBuffer.Size > 12);

	bool sawNearest = false;
	bool sawLinear = false;
	for (const ImDrawCmd &draw : target->CmdBuffer) {
		sawNearest = sawNearest || draw.UserCallback == SetNearestSampler;
		sawLinear = sawLinear || draw.UserCallback == SetLinearSampler;
	}
	CHECK(sawNearest);
	CHECK(sawLinear);

	// The fill's left and right vertices were tinted by opposite gradient ends.
	CHECK(target->VtxBuffer[0].col != target->VtxBuffer[1].col);
}
