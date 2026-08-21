// The interface, as triangles, at the rate a display asks for them.
//
// **This is the other half of the UI and the one nothing measured.**
// `engine.gui.bench.interface` covers the half that decides *what* to draw -
// layout, and the compile into a `gui::DrawList`. What happens next is
// arithmetic: quad generation, corner tessellation, nine-slice, text laid out
// against a `GlyphAtlas`, and the batch boundaries a scissor change forces. That
// is `render::InterfaceMesh`, it runs on the CPU, it needs no device, and it
// runs **per frame at the display's rate** rather than per tick.
//
// That last point is the whole reason the rows here are worth having. A tick is
// thirty a second; a frame at 300 Hz is ten times that, and an interface is
// rebuilt on every one of them. A millisecond in this file is a third of a
// 300 Hz frame budget spent turning rectangles into rectangles.
//
// **The batch rows are the ones that would surprise somebody.** A batch is a
// range between scissor changes, so a list whose elements each clip to
// themselves produces one batch per element and a list that shares a clip
// produces one batch for all of them. Same triangles either way. The pair of
// rows prices the difference, and it is the difference between an interface
// that submits one draw and one that submits a thousand - which is a device
// cost this file cannot see and a CPU cost it can.
//
// **Text is measured separately because it is not a quad.** A glyph is a lookup,
// an advance and a quad, per character, and a run has to be measured before it
// can be aligned - so a centred label walks its own string twice. Wrapped text
// walks it more. The rows separate a plain run from a wrapped one so the
// wrapping is a number rather than an assumption.
//
// **`GlyphAtlas::Build` is here even though it is not per frame.** It rasterises
// and packs every glyph of every face at one pixel size, which is a startup cost
// and then a cost again on every DPI change - and a DPI change is somebody
// dragging a window between two monitors, which is a moment where a stall is
// extremely visible. It is also the one thing here that allocates a texture's
// worth of bytes.
//
// No device is created. The vertices, indices and batches this produces are
// exactly what the pass would upload, so everything decidable without a GPU is
// decided here, and `Overlay.cpp` draws the same line for the debug panels.

#include <engine/core/Name.hpp>
#include <engine/core/Paths.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/Enums.hpp>
#include <engine/render/GlyphAtlas.hpp>
#include <engine/render/InterfaceMesh.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

TEST_SUITE_ID("engine.render.bench.interface")

using engine::core::Rect;
using engine::core::Vector2;
using engine::gui::DrawCommand;
using engine::gui::DrawKind;
using engine::gui::DrawList;
using engine::gui::FontFace;
using engine::gui::ScaleType;
using engine::render::GlyphAtlas;
using engine::render::InterfaceMesh;
using engine::testing::Consume;

namespace interface_bench {
	// Commands per list. A thousand is a busy game HUD with an inventory open;
	// ten thousand is a studio with several docked panels and a property grid,
	// and is the count at which the sixteen-bit index buffer starts to matter -
	// four vertices a quad puts ten thousand quads at forty thousand vertices,
	// comfortably inside the limit and not by a wide margin.
	constexpr size_t ELEMENTS = 1000;
	constexpr size_t MANY = 10'000;

	// The canvas the list is compiled against.
	constexpr float WIDTH = 1920.0f;
	constexpr float HEIGHT = 1080.0f;

	// Points `core::Paths` at a directory that has fonts in it.
	//
	// **A benchmark binary is not a staged program and so has no assets of its
	// own.** `GlyphAtlas::Build` reads `Paths::Assets() / "fonts"`, which for
	// this executable is the `bench/` directory beside the staged programs -
	// and nothing stages a font there. Without this the atlas builds empty,
	// every text row silently measures the no-glyph path, and the figures look
	// *better* than the plain-rectangle rows, which is exactly the shape of a
	// benchmark measuring nothing.
	//
	// So it borrows: the first sibling of this binary's own directory that has
	// `fonts/Inter.ttf` under it. A build where none does still runs, and the
	// text rows then honestly report what a client whose fonts failed to stage
	// draws - which `InterfaceMesh::Build` documents as a supported state.
	void BorrowFonts() {
		namespace fs = std::filesystem;
		const fs::path here = engine::core::Paths::Base();
		if (fs::exists(here / "fonts" / "Inter.ttf")) {
			return;
		}

		std::error_code failed;
		for (const fs::directory_entry &sibling : fs::directory_iterator(here.parent_path(), failed)) {
			if (sibling.is_directory() && fs::exists(sibling.path() / "fonts" / "Inter.ttf")) {
				engine::core::Paths::SetAssetsOverride(sibling.path());
				return;
			}
		}
	}

	// The atlas, built once at a size a real interface uses.
	//
	// **Not rebuilt per row.** Building one rasterises every glyph of every
	// face; doing that inside a measured body would measure the rasteriser in
	// every row that happened to draw text.
	const GlyphAtlas &Atlas() {
		static const GlyphAtlas atlas = [] {
			BorrowFonts();
			GlyphAtlas built;
			built.Build(16.0f);
			return built;
		}();
		return atlas;
	}

	// An element's slot on a grid across the canvas, so the commands are spread
	// out rather than stacked - a mesh builder that culled coincident quads
	// would otherwise look fast on a pile of identical rectangles.
	Rect SlotOf(size_t index) {
		const float column = static_cast<float>(index % 40);
		const float row = static_cast<float>((index / 40) % 27);
		const Vector2 corner{column * 48.0f, row * 40.0f};
		return Rect{corner, Vector2{corner.X + 44.0f, corner.Y + 36.0f}};
	}

	// The command every row starts from: a plain filled rectangle, clipped to
	// the whole canvas so nothing forces a batch boundary.
	DrawCommand Base(size_t index) {
		DrawCommand command;
		command.Kind = DrawKind::Rectangle;
		command.Bounds = SlotOf(index);
		command.Clip = Rect{Vector2{0.0f, 0.0f}, Vector2{WIDTH, HEIGHT}};
		command.Tint = {0.2f, 0.5f, 0.9f};
		command.Transparency = 0.0f;
		return command;
	}

	// A list built once per shape and kept.
	//
	// A deque, so a reference handed out earlier survives a later shape being
	// built.
	//
	// @param name  What this shape is, which is also its cache key.
	// @param count How many commands.
	// @param shape Applied to each command after `Base`.
	template <class Shape> const DrawList &Listing(const char *name, size_t count, Shape &&shape) {
		struct Built {
			const char *Name;
			size_t Count;
			DrawList List;
		};
		static std::deque<Built> built;

		for (const Built &entry : built) {
			if (entry.Name == name && entry.Count == count) {
				return entry.List;
			}
		}

		DrawList list;
		list.CanvasSize = Vector2{WIDTH, HEIGHT};
		list.Elements = count;
		list.Commands.reserve(count);
		for (size_t index = 0; index < count; index++) {
			DrawCommand command = Base(index);
			shape(command, index);
			list.Commands.push_back(std::move(command));
		}

		built.push_back(Built{name, count, std::move(list)});
		return built.back().List;
	}

	// Stands in for a texture table: every name resolves to one 32x32 sheet.
	//
	// **Absent is not a cheaper version of present, it is a different path.**
	// With no resolver an `Image` command has a source size of zero, and the
	// slice, tile, fit and crop branches all test that before they run - so a
	// list of nine-sliced images with no resolver draws a thousand plain quads
	// and reports slicing as free. Supplying one is what makes those rows
	// measure what they are named after.
	engine::render::InterfaceImageInfo Sheet(const engine::core::Name &) {
		engine::render::InterfaceImageInfo info;
		info.Size = Vector2{32.0f, 32.0f};
		return info;
	}

	// Builds the mesh and reports its size, so nothing can be elided and a
	// builder that stopped emitting is visible rather than fast.
	size_t Build(const DrawList &list) {
		static InterfaceMesh mesh;
		mesh.Build(list, Atlas(), Sheet);
		return mesh.Vertices().size() + mesh.Indices().size() + mesh.Batches().size();
	}
}

using namespace interface_bench;

// --- the plain case -----------------------------------------------------------

BENCH("InterfaceMesh::Build · 1000 rectangles", 1) {
	Consume(Build(Listing("plain", ELEMENTS, [](DrawCommand &, size_t) {})));
}

BENCH("InterfaceMesh::Build · 10,000 rectangles", 1) {
	// Ten times the commands. Ten times the cost is a builder that scales with
	// its input; anything steeper is a per-command search over what came before.
	Consume(Build(Listing("plain", MANY, [](DrawCommand &, size_t) {})));
}

// --- the shapes that are not one quad -----------------------------------------

BENCH("InterfaceMesh::Build · 1000 rounded rectangles", 1) {
	// **A corner is tessellated, not sampled.** A rounded rectangle is several
	// times the triangles of a square one and the arc segment count is chosen
	// per command, so this against the plain row is what a corner radius costs
	// on a HUD that rounds everything - which most of them do.
	Consume(Build(Listing("rounded", ELEMENTS, [](DrawCommand &command, size_t) {
		command.CornerRadius = 8.0f;
	})));
}

BENCH("InterfaceMesh::Build · 1000 outlines", 1) {
	// A border is four quads or a ring of them depending on the join, and every
	// panel in a studio has one.
	Consume(Build(Listing("outline", ELEMENTS, [](DrawCommand &command, size_t) {
		command.Kind = DrawKind::Outline;
		command.Thickness = 2.0f;
	})));
}

BENCH("InterfaceMesh::Build · 1000 rounded outlines", 1) {
	// Both at once, which is what a selected panel is.
	//
	// **They do not add.** As measured this is well over the rounded row plus
	// the outline row, because a rounded outline is a ring of tessellated
	// segments rather than a tessellated fill with a border drawn round it - so
	// the corner segment count is paid on an inner arc and an outer one. A
	// studio that draws a rounded border on every panel is paying the most
	// expensive shape in this file for its most common one.
	Consume(Build(Listing("rounded-outline", ELEMENTS, [](DrawCommand &command, size_t) {
		command.Kind = DrawKind::Outline;
		command.Thickness = 2.0f;
		command.CornerRadius = 8.0f;
	})));
}

BENCH("InterfaceMesh::Build · 1000 nine-sliced images", 1) {
	// **Nine quads where a stretch is one.** A sliced image is how every button
	// and every frame in a themed interface is drawn, so this is the ordinary
	// case for a skinned UI rather than an exotic one.
	Consume(Build(Listing("sliced", ELEMENTS, [](DrawCommand &command, size_t) {
		command.Kind = DrawKind::Image;
		command.Scale = ScaleType::Slice;
		command.SliceCenter = Rect{Vector2{8.0f, 8.0f}, Vector2{24.0f, 24.0f}};
		command.SliceScale = 1.0f;
	})));
}

// --- text ---------------------------------------------------------------------

BENCH("InterfaceMesh::Build · 1000 text runs of 24 characters", 1) {
	// Twenty-four thousand glyphs, each a lookup, an advance and a quad. A
	// centred run is measured before it is placed, so the string is walked
	// twice - which is what `GlyphAtlas::Measure` below prices on its own.
	//
	// **The most expensive thing an interface does, by an order of magnitude
	// over a rectangle.** Every other row in this file is a handful of vertices
	// per command; a label is one quad per character and a measure pass before
	// them. An interface that is slow is almost always an interface with a lot
	// of text in it, and this is the row that says so rather than the one
	// somebody guesses.
	Consume(Build(Listing("text", ELEMENTS, [](DrawCommand &command, size_t index) {
		command.Kind = DrawKind::Text;
		command.Text = "label " + std::to_string(index) + " of many";
		command.TextSize = 16;
		command.Font = FontFace::Regular;
	})));
}

BENCH("InterfaceMesh::Build · 1000 wrapped text runs of 24 characters", 1) {
	// The same runs asked to wrap inside bounds too narrow for them. Wrapping
	// walks the run looking for break opportunities and then lays out each line,
	// so this against the row above is what a wrapped label costs over a
	// clipped one.
	Consume(Build(Listing("wrapped", ELEMENTS, [](DrawCommand &command, size_t index) {
		command.Kind = DrawKind::Text;
		command.Text = "label " + std::to_string(index) + " of many";
		command.TextSize = 16;
		command.Font = FontFace::Regular;
		command.Wrapped = true;
	})));
}

BENCH("GlyphAtlas::Measure · 100k short strings", 100'000) {
	// **Per label per frame, before anything is drawn.** An alignment cannot be
	// resolved without a width, so every centred or right-aligned label in an
	// interface pays this whether or not its text changed. It is the most
	// frequently executed call in the interface path.
	static const std::vector<std::string> labels = [] {
		std::vector<std::string> built;
		for (size_t index = 0; index < 256; index++) {
			built.push_back("label " + std::to_string(index) + " of many");
		}
		return built;
	}();

	float total = 0.0f;
	for (size_t call = 0; call < 100'000; call++) {
		total += Atlas().Measure(engine::render::Typeface::Interface, labels[call % labels.size()]);
	}
	Consume(total);
}

// --- what a scissor costs -----------------------------------------------------
//
// Same triangles, same commands, different clip rectangles. The only difference
// between these two rows is whether consecutive commands share a scissor, and
// the result is one batch or a thousand.

BENCH("InterfaceMesh::Build · 1000 rectangles under one scissor", 1) {
	Consume(Build(Listing("one-clip", ELEMENTS, [](DrawCommand &, size_t) {})));
}

BENCH("InterfaceMesh::Build · 1000 rectangles each clipped to itself", 1) {
	// **What a deeply nested interface produces without meaning to.** A `Clip`
	// is already the intersection of every clipping ancestor, so an element
	// inside a scrolling list inside a panel gets a rectangle of its own - and a
	// batch of its own with it. The device cost of a thousand draws is not
	// visible from here; the CPU cost of a thousand batch records is, and this
	// row is it.
	//
	// **As measured it is free** - the two rows are inside each other's noise -
	// so the thousand-batch interface costs nothing extra to *build*. Whatever
	// it costs is entirely on the device, and a CPU profile would show nothing
	// at all. That is worth knowing before somebody spends a week merging
	// scissor rectangles on the strength of a flame graph.
	Consume(Build(Listing("own-clip", ELEMENTS, [](DrawCommand &command, size_t index) {
		const Rect slot = SlotOf(index);
		command.Clip = slot;
	})));
}

// --- the atlas ----------------------------------------------------------------

BENCH("GlyphAtlas::Build · 16 px", 1) {
	// Startup, and again on every DPI change - which is somebody dragging a
	// window between two monitors, where a stall is as visible as a stall gets.
	BorrowFonts();
	GlyphAtlas atlas;
	Consume(atlas.Build(16.0f));
	Consume(atlas.Coverage().size());
}

BENCH("GlyphAtlas::Build · 32 px", 1) {
	// Four times the coverage bytes at twice the pixel size. Read against the
	// row above: a rasteriser bounded by area grows fourfold, one bounded by
	// glyph count does not.
	//
	// **As measured it barely moves** - about a third dearer for four times the
	// area - so the cost is per glyph rather than per texel: outline decoding
	// and packing, not filling. Which means the sensible response to a DPI
	// change is not to build a smaller atlas.
	BorrowFonts();
	GlyphAtlas atlas;
	Consume(atlas.Build(32.0f));
	Consume(atlas.Coverage().size());
}
