// The two seams between this program and `Vendor::nodegraph`.
//
// **The library's own suite is in the library**, and moved there with it when
// `D00113` closed: the cycle guard, the content hash, folding, the save format,
// the async evaluator and the canvas are `mono.vendor/nodegraph`'s 415 checks,
// run by its own `nodegraph_tests`. Re-asserting any of that here would be the
// second copy the whole entry was about, one directory further out.
//
// What is left is what neither repository can check alone, and both of them are
// silent when they go wrong:
//
// - **A picture is pixels in an agreed order.** `PreviewImage` promises red
//   first and the top row first at four bytes a pixel and `TextureFormat::RGBA8`
//   means the same thing, so `NodePreviewTexture` is a copy - until one side
//   changes its mind, and then it is a thumbnail with its channels swapped,
//   which still draws.
// - **The library draws chrome with this editor's theme.** Nothing fails if
//   `ApplyNodeChrome` stops being called; the palette popup simply keeps the
//   grey the library ships with, on a machine whose owner picked a light theme
//   and a 2× interface scale.

#include <engine/assets/Texture.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <imgui.h>
#include <nodegraph/Demo.hpp>
#include <nodegraph/Evaluate.hpp>
#include <nodegraph/Graph.hpp>
#include <nodegraph/Preview.hpp>
#include <nodegraph/Registry.hpp>
#include <nodegraph/Types.hpp>
#include <string>
#include <studio/Editor.hpp>
#include <thread>
#include <vector>

TEST_SUITE_ID("studio.nodegraph")

namespace {
	// A bare imgui context around one case - `studio.assetrow` carries why this
	// is per case and not shared. `ApplyEditorTheme` writes into `ImGui::
	// GetStyle()`, so there has to be one for it to write into.
	class Context {
	  public:
		Context() {
			IMGUI_CHECKVERSION();
			Handle = ImGui::CreateContext();

			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1280.0f, 720.0f);
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
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

	// The demo graph, run until nothing is still working, with a ceiling so a
	// case fails rather than hangs. Two of its node types are async.
	void Settle(nodegraph::Evaluator &runner, const nodegraph::Graph &graph) {
		nodegraph::RunReport report = runner.Run(graph);
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);

		while ((runner.Busy() || report.Waiting > 0) && std::chrono::steady_clock::now() < until) {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
			report = runner.Run(graph);
		}
	}

	// Whether any pixel's red and blue differ.
	//
	// **What makes the byte-for-byte comparison mean anything.** A height field
	// draws as a grey ramp, and a picture where every pixel is `r == g == b`
	// survives having its channels swapped - so a case that took the first
	// picture it found would assert the seam and prove nothing about it.
	bool Coloured(const nodegraph::PreviewImage &image) {
		for (size_t index = 0; index + 3 < image.Rgba.size(); index += 4) {
			if (image.Rgba[index] != image.Rgba[index + 2]) {
				return true;
			}
		}
		return false;
	}

	// A colour picture the demo graph produced, from whichever node made one.
	//
	// Taken from a real evaluation rather than filled in by hand, because a
	// hand-made buffer would agree with whatever this file assumed and the point
	// of the case is that the *library's* buffer does.
	bool ColourPicture(nodegraph::PreviewImage &out) {
		using namespace nodegraph;

		Graph graph;
		BuildDemoGraph(graph);

		Evaluator runner;
		Settle(runner, graph);

		for (const Node &node : graph.Nodes()) {
			const NodeType *type = NodeTypes::Find(node.Type);
			if (type == nullptr || type->Outputs.empty()) {
				continue;
			}
			const PortSpec &port = type->Outputs.front();
			const std::any *payload = runner.Output(node.Id, port.Name);
			if (payload != nullptr && PictureOf(type, port.Type, *payload, out) && Coloured(out)) {
				return true;
			}
		}
		return false;
	}
}

// --- pixels -------------------------------------------------------------------

TEST_CASE("a node's picture is a texture the renderer takes", "[studio][nodegraph]") {
	nodegraph::PreviewImage image;
	REQUIRE(ColourPicture(image));
	REQUIRE(image.Valid());

	engine::assets::TextureData texture;
	REQUIRE(studio::NodePreviewTexture(image, texture));

	// The picture is square and the texture is not asked to guess.
	CHECK(texture.Width == image.Side);
	CHECK(texture.Height == image.Side);
	CHECK(texture.Format == engine::assets::TextureFormat::RGBA8);

	// Four bytes a pixel on both sides. A format that meant three would leave
	// the buffer a quarter too long and the renderer reading past its rows.
	REQUIRE(texture.Pixels.size() == static_cast<size_t>(texture.Width) * texture.Height * 4);
	CHECK(texture.Pixels.size() == image.Rgba.size());

	// **Byte for byte, in the order the library wrote them.** Channel order is
	// the half of this seam that cannot be spotted by looking at the editor: a
	// red-blue swap is a picture, and a picture is what somebody expects.
	bool same = true;
	for (size_t index = 0; index < image.Rgba.size(); index++) {
		same = same && texture.Pixels[index] == static_cast<std::byte>(image.Rgba[index]);
	}
	CHECK(same);
}

TEST_CASE("a payload with no picture makes no texture", "[studio][nodegraph]") {
	// What a node carrying a number produces. The editor holds the answer as a
	// null handle and never asks again, so a conversion that answered `true`
	// here would put an empty texture in the atlas once per such node.
	engine::assets::TextureData texture;
	CHECK_FALSE(studio::NodePreviewTexture(nodegraph::PreviewImage{}, texture));

	// A buffer that does not match the side it claims is the same answer. It is
	// the one shape `PreviewImage::Valid` exists to catch, and the one a partly
	// filled preview would produce.
	nodegraph::PreviewImage ragged;
	ragged.Side = 8;
	ragged.Rgba.assign(8 * 8 * 4 - 1, 0);
	CHECK_FALSE(studio::NodePreviewTexture(ragged, texture));
}

// --- the theme ----------------------------------------------------------------

TEST_CASE("the editor's theme is what the node canvas draws chrome with", "[studio][nodegraph]") {
	Context context;

	engine::ui::ApplyEditorTheme(1.0f);
	studio::ApplyNodeChrome();

	const nodegraph::Chrome &chrome = nodegraph::HostChrome();
	CHECK(chrome.Muted == engine::ui::MutedColour());
	CHECK(chrome.Accent == engine::ui::AccentColour());
	CHECK(chrome.Warning == engine::ui::WarningColour());

	// `Scaled` is the library's only spelling of a fixed size, so this is what
	// every popup width and thumbnail row in it is built from.
	CHECK(chrome.Scale == 1.0f);
	CHECK(nodegraph::Scaled(100.0f) == 100.0f);

	// **A scale changed while the panel is open follows it.** The whole reason
	// this is copied every frame rather than at start-up: the Settings panel can
	// move it, and a canvas left at the launch scale draws popups a third of the
	// size of the rest of the editor.
	engine::ui::ApplyEditorTheme(2.0f);
	studio::ApplyNodeChrome();
	CHECK(chrome.Scale == 2.0f);
	CHECK(nodegraph::Scaled(100.0f) == 200.0f);

	// And so does a colour, which is the half a scale test would not catch: the
	// palette is chosen separately from the scale and neither implies the other.
	const unsigned int before = chrome.Muted;
	engine::ui::SetPalette(engine::ui::Palette::Shadow);
	studio::ApplyNodeChrome();
	CHECK(chrome.Muted == engine::ui::MutedColour());
	CHECK(chrome.Muted != before);

	engine::ui::SetPalette(engine::ui::Palette::Dark);
	engine::ui::ApplyEditorTheme(1.0f);
}
