// The zoom a panel applies to its own text, checked without a GPU.
//
// **What is pinned here is that a zoom survives a child window**, because that
// is the one property the obvious implementation does not have and the one
// nothing else would notice was missing.
//
// The script editor and the output panel both scale their own text and both put
// that text inside a child window — `InputTextMultiline` makes one for itself,
// and the log draws into an explicit one. imgui's `SetWindowFontScale` is
// per-window state: a child begins at scale 1 whatever its parent was set to,
// so scaling the panel zoomed the frame around the code and left every glyph in
// it alone. `ScopedFont` pushes a *size* instead, which is context state, and
// this is the check that says so.
//
// An imgui context is not a device — `CreateContext` allocates a style table and
// a font atlas description and touches no driver — so the whole thing runs here.

#include <engine/testing/Suite.hpp>
#include <engine/ui/Fonts.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

TEST_SUITE_ID("engine.ui.fonts")

namespace {
	// A bare context per case, so a case that fails mid-frame does not leave a
	// window on the stack for the next one to inherit and fail inside.
	class Context {
	  public:
		Context() {
			IMGUI_CHECKVERSION();
			Handle = ImGui::CreateContext();

			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1280.0f, 720.0f);
			io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.ConfigErrorRecoveryEnableTooltip = false;

			// **No `LoadFonts`, which is deliberate.** The staged fonts may not
			// be beside a test binary, and the face a panel asks for is allowed
			// to be missing — so this runs the path where it is. A zoom has to
			// work in imgui's default face too, or an editor started from
			// somewhere unexpected loses its zoom along with its shapes.
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}

		~Context() {
			ImGui::DestroyContext(Handle);
		}

		Context(const Context &) = delete;
		Context &operator=(const Context &) = delete;

	  private:
		ImGuiContext *Handle = nullptr;
	};
}

TEST_CASE("a scoped zoom reaches into a child window", "[ui][fonts]") {
	const Context context;

	ImGui::NewFrame();

	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(600.0f, 400.0f));
	REQUIRE(ImGui::Begin("panel", nullptr, ImGuiWindowFlags_NoSavedSettings));

	const float plain = ImGui::GetFontSize();
	REQUIRE(plain > 0.0f);

	{
		const engine::ui::ScopedFont zoomed(
			engine::ui::Typeface::Monospace, engine::ui::TextSize::Body, 2.0f
		);

		const float outer = ImGui::GetFontSize();

		// It zooms at all. A scale that quietly did nothing would satisfy the
		// interesting check below for the least useful reason.
		CHECK(outer > plain);

		// **The check this file exists for.** A child begins its own window
		// state; the size has to come from the context or the code inside one
		// draws at the size somebody stopped choosing.
		ImGui::BeginChild("##inner", ImVec2(200.0f, 100.0f));
		CHECK(ImGui::GetFontSize() == outer);
		ImGui::EndChild();
	}

	// Popped with the scope, so the panel's next row is the panel's own size
	// rather than the code's.
	CHECK(ImGui::GetFontSize() == plain);

	ImGui::End();
	ImGui::Render();
}

TEST_CASE("a scoped font without a zoom leaves the size alone", "[ui][fonts]") {
	const Context context;

	ImGui::NewFrame();
	REQUIRE(ImGui::Begin("panel", nullptr, ImGuiWindowFlags_NoSavedSettings));

	const float plain = ImGui::GetFontSize();

	// The two-argument form every other panel uses. Adding a zoom parameter must
	// not have made a plain face push a size of its own.
	{
		const engine::ui::ScopedFont face(engine::ui::Typeface::Monospace);
		CHECK(ImGui::GetFontSize() == plain);
	}

	CHECK(ImGui::GetFontSize() == plain);

	ImGui::End();
	ImGui::Render();
}
