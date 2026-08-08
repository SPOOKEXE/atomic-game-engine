// The Render Pipeline and Assets Pipeline node editors.
//
// **The panel, and nothing else.** Everything that decides what a canvas means
// is `nodeview::Editor` — where a port sits, what is under the pointer, whether
// a wire may be dropped, what the add menu lists, how a graph becomes a file.
// All of it is arithmetic and all of it has tests. What is here is the part
// that cannot have any: turning a mouse into those calls, and turning their
// answers into rectangles.
//
// **Drawn with an ImGui draw list**, which is `DEFERRED.md` D00041's decision. A
// `gui` subtree cannot live inside an `ImGui::Begin` block — the two are separate
// renderers with separate draw lists — so the engine's own tree would need a
// render target per open editor. This is the editor's chrome rather than a game's
// interface, so it draws the way the rest of the editor does.
//
// ## The bindings, in one place so they can be argued with
//
//   - **Left** drags a node by its title bar, drags a wire from a port, and
//     selects. Dropping a wire on empty canvas breaks it.
//   - **Right** pans, and on a click without a drag opens a menu: the add-node
//     search over empty canvas, the node's own menu over a node.
//   - **Wheel** zooms about the pointer.
//   - **Delete** removes the selection.
//
// These are ComfyUI's, deliberately. A node editor that invented its own
// bindings would be one somebody has to learn twice.

#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/PipelineView.hpp>
#include <engine/nodeview/Assets.hpp>
#include <engine/nodeview/Editor.hpp>

#include <imgui.h>

#include <algorithm>
#include <string>

namespace studio {

	namespace {
		using engine::core::Name;
		using engine::graph::PortDirection;
		using engine::graph::PortRef;
		using engine::graph::ResourceKind;
		using engine::nodeview::EditorGraph;
		using engine::nodeview::EditorLink;
		using engine::nodeview::EditorNode;
		using engine::nodeview::NodeStyle;
		using engine::nodeview::Point;

		// The spacing the canvas is drawn and hit-tested with. One value rather
		// than two, because `nodeview::HitTest` takes the style it was drawn
		// with and passing a different one picks the wrong thing.
		const NodeStyle STYLE;

		// The Assets canvas is still the read-only diagram `LayoutAssets`
		// produces, so it keeps a column-and-row spacing of its own until it is
		// rebuilt on the seam above. **Named constants rather than a second
		// style struct**: two structs of spacing would be the thing to keep in
		// step, and this one is four numbers with one reader.
		//@{
		constexpr float ASSET_MARGIN = 16.0f;
		constexpr float ASSET_WIDTH = 140.0f;
		constexpr float ASSET_HEIGHT = 44.0f;
		constexpr float ASSET_GAP = 24.0f;
		//@}

		ImU32 Colour(float r, float g, float b, float a = 1.0f) {
			return ImGui::GetColorU32(ImVec4{r, g, b, a});
		}

		// A resource kind's colour, used for its port dot and every wire
		// carrying it.
		//
		// **The wire and the dot are the same colour on purpose.** That is the
		// whole of how a typed editor is read at a glance: what a slot takes and
		// what a wire carries are one fact, and two palettes would make somebody
		// check.
		ImU32 ColourOf(ResourceKind kind) {
			switch (kind) {
			case ResourceKind::Colour:
				return Colour(0.95f, 0.72f, 0.35f);
			case ResourceKind::Depth:
				return Colour(0.45f, 0.68f, 0.95f);
			case ResourceKind::Texture:
				return Colour(0.55f, 0.85f, 0.55f);
			}
			return Colour(0.7f, 0.7f, 0.7f);
		}

		ImU32 ColourOf(engine::graph::NodeCategory category) {
			switch (category) {
			case engine::graph::NodeCategory::Draw:
				return Colour(0.20f, 0.26f, 0.22f);
			case engine::graph::NodeCategory::Composite:
				return Colour(0.24f, 0.20f, 0.28f);
			case engine::graph::NodeCategory::Interface:
				return Colour(0.16f, 0.22f, 0.30f);
			case engine::graph::NodeCategory::Output:
				return Colour(0.28f, 0.20f, 0.20f);
			}
			return Colour(0.18f, 0.18f, 0.18f);
		}

		ImVec2 Screen(const engine::nodeview::CanvasView &view, Point canvas, const ImVec2 &corner) {
			const Point at = view.ToScreen(canvas);
			return ImVec2{corner.x + at.X, corner.y + at.Y};
		}

		// The wire, as a curve rather than a line.
		//
		// **Horizontal tangents scaled by the gap**, which is what makes a
		// backwards wire — a node dragged left of the one feeding it — leave and
		// arrive on the correct sides instead of cutting through both boxes. A
		// straight line cannot express that at all.
		void Wire(ImDrawList &list, const ImVec2 &from, const ImVec2 &to, ImU32 colour, float thickness) {
			const float reach = std::clamp(std::abs(to.x - from.x) * 0.6f, 24.0f, 140.0f);
			list.AddBezierCubic(
				from, ImVec2{from.x + reach, from.y}, ImVec2{to.x - reach, to.y}, to, colour, thickness
			);
		}

		// The dotted background, so a pan has something to move against.
		//
		// **Without it a canvas with nothing selected looks frozen**: dragging
		// empty space moves boxes that are off screen and nothing on screen
		// changes, which reads as the panel having stopped responding.
		void DrawGrid(ImDrawList &list, const engine::nodeview::CanvasView &view, const ImVec2 &corner,
					  const ImVec2 &size) {
			constexpr float SPACING = 32.0f;
			const float step = SPACING * view.Zoom;
			if (step < 6.0f) {
				return;
			}

			const ImU32 ink = Colour(1.0f, 1.0f, 1.0f, 0.045f);
			for (float x = std::fmod(view.Pan.X, step); x < size.x; x += step) {
				list.AddLine(ImVec2{corner.x + x, corner.y}, ImVec2{corner.x + x, corner.y + size.y}, ink);
			}
			for (float y = std::fmod(view.Pan.Y, step); y < size.y; y += step) {
				list.AddLine(ImVec2{corner.x, corner.y + y}, ImVec2{corner.x + size.x, corner.y + y}, ink);
			}
		}
	}

	// --- the Render Pipeline ---------------------------------------------------

	void Editor::DrawRenderPipeline() {
		if (!ShowRenderPipeline) {
			return;
		}

		if (!ImGui::Begin("Render Pipeline", &ShowRenderPipeline)) {
			ImGui::End();
			return;
		}

		engine::graph::RegisterStandardNodeKinds();

		// **Loaded once per world rather than derived per frame.** A panel that
		// rebuilt its graph every frame would throw away a wire on the frame it
		// was dragged and would have nowhere to put a node somebody moved.
		if (RenderPipelineLoaded != Active || RenderPipelineGraph.Nodes.empty()) {
			RenderPipelineLoaded = Active;
			RenderPipelineDirty = false;

			engine::graph::PipelineDocument document = engine::graph::StandardDocument();
			if (Active.IsValid() && Universe != nullptr) {
				Universe->Enter(Active, [&document](engine::ecs::Store &store) {
					if (const auto *set = store.Resource<engine::graph::PipelineSet>()) {
						if (const auto *stored = set->Find(Name("main"))) {
							document = *stored;
						}
					}
				});
			}
			RenderPipelineGraph = engine::nodeview::FromDocument(document, STYLE);
		}

		// --- the toolbar -------------------------------------------------------

		if (ImGui::Button("Add node")) {
			RenderPipelineAddAt = RenderPipelineCanvas.ToCanvas(Point{120.0f, 120.0f});
			RenderPipelineSearch.clear();
			ImGui::OpenPopup("add-node");
		}
		ImGui::SameLine();
		if (ImGui::Button("Arrange")) {
			// **The old diagram, kept as a button.** `LayoutPipeline` is still
			// the right answer for "where does this pass run in the frame", and
			// a canvas somebody has made a mess of wants one click back to it.
			engine::graph::RenderGraph built;
			Name offender;
			engine::graph::CompiledGraph compiled;
			if (engine::graph::Build(
					engine::nodeview::ToDocument(RenderPipelineGraph), built, offender
				) == engine::graph::PipelineDocumentStatus::Ok &&
				built.Compile(compiled, offender) == engine::graph::GraphStatus::Ok) {
				const engine::graph::PipelineLayout layout = engine::graph::LayoutPipeline(built, compiled);
				for (const engine::graph::PlacedNode &placed : layout.Nodes) {
					if (EditorNode *node = RenderPipelineGraph.Find(placed.Name)) {
						node->At = {
							static_cast<float>(placed.Column) * (STYLE.Width + 90.0f),
							static_cast<float>(placed.Where) * 260.0f +
								static_cast<float>(placed.Row) * 120.0f,
						};
					}
				}
				RenderPipelineDirty = true;
			}
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!RenderPipelineDirty || !Active.IsValid() || Universe == nullptr);
		if (ImGui::Button("Save to world")) {
			const engine::graph::PipelineDocument document =
				engine::nodeview::ToDocument(RenderPipelineGraph);
			Universe->Enter(Active, [&document](engine::ecs::Store &store) {
				engine::graph::RegisterPipelineComponents();
				if (!store.HasResource<engine::graph::PipelineSet>()) {
					store.SetResource(engine::graph::PipelineSet{});
				}
				store.ResourceMutable<engine::graph::PipelineSet>()->Set(Name("main"), document);
			});
			RenderPipelineDirty = false;
			Say("saved the render pipeline into this world");
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::TextDisabled("%s", RenderPipelineDirty ? "edited" : "saved");

		// **Compiled every frame, and the result is the status line.** A
		// pipeline half-wired is the ordinary state of one being built, so this
		// reports rather than refuses — the editor's job is to say what is wrong
		// while it is being fixed.
		engine::graph::RenderGraph runnable;
		Name offender;
		engine::graph::CompiledGraph compiled;
		const engine::graph::PipelineDocumentStatus builds =
			engine::graph::Build(engine::nodeview::ToDocument(RenderPipelineGraph), runnable, offender);
		const engine::graph::GraphStatus compiles = builds == engine::graph::PipelineDocumentStatus::Ok
														? runnable.Compile(compiled, offender)
														: engine::graph::GraphStatus::Ok;

		ImGui::Separator();

		// --- the canvas --------------------------------------------------------

		const ImVec2 corner = ImGui::GetCursorScreenPos();
		const ImVec2 size = ImGui::GetContentRegionAvail();
		const ImVec2 area{std::max(size.x, 64.0f), std::max(size.y - 26.0f, 64.0f)};

		// **Both buttons, because the right one pans and opens the menu.** An
		// invisible button claims the area so ImGui's own window drag does not
		// take the gesture first.
		ImGui::InvisibleButton(
			"canvas", area, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight
		);
		const bool hovered = ImGui::IsItemHovered();
		const ImVec2 mouse = ImGui::GetMousePos();
		const Point pointer = RenderPipelineCanvas.ToCanvas(Point{mouse.x - corner.x, mouse.y - corner.y});

		ImDrawList &list = *ImGui::GetWindowDrawList();
		list.PushClipRect(corner, ImVec2{corner.x + area.x, corner.y + area.y}, true);
		list.AddRectFilled(corner, ImVec2{corner.x + area.x, corner.y + area.y}, Colour(0.09f, 0.10f, 0.12f));
		DrawGrid(list, RenderPipelineCanvas, corner, area);

		// Pan and zoom.
		if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
			const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
			RenderPipelineCanvas.Pan.X += delta.x;
			RenderPipelineCanvas.Pan.Y += delta.y;
			ImGui::ResetMouseDragDelta(ImGuiMouseButton_Right);
		}
		if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
			engine::nodeview::ZoomAbout(
				RenderPipelineCanvas,
				Point{mouse.x - corner.x, mouse.y - corner.y},
				ImGui::GetIO().MouseWheel > 0.0f ? 1.12f : 1.0f / 1.12f
			);
		}

		const engine::nodeview::Hit under = engine::nodeview::HitTest(RenderPipelineGraph, STYLE, pointer);

		// Press.
		if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			switch (under.What) {
			case engine::nodeview::HitKind::Port:
				// **Grabbing a bound input picks the wire up rather than
				// starting a second one**, which is what every node editor does
				// and what makes rewiring one gesture instead of two.
				if (under.Port.Direction == PortDirection::Input) {
					(void)engine::nodeview::Disconnect(RenderPipelineGraph, under.Port);
					RenderPipelineDirty = true;
				}
				RenderPipelineWire = under.Port;
				break;
			case engine::nodeview::HitKind::Header: {
				RenderPipelineSelected = under.Node;
				RenderPipelineDragging = under.Node;
				const EditorNode *node = RenderPipelineGraph.Find(under.Node);
				RenderPipelineGrab = {pointer.X - node->At.X, pointer.Y - node->At.Y};
				break;
			}
			case engine::nodeview::HitKind::Body:
				RenderPipelineSelected = under.Node;
				break;
			case engine::nodeview::HitKind::None:
				RenderPipelineSelected = Name{};
				break;
			}
		}

		// Drag.
		if (RenderPipelineDragging.IsValid()) {
			if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				if (EditorNode *node = RenderPipelineGraph.Find(RenderPipelineDragging)) {
					node->At = {pointer.X - RenderPipelineGrab.X, pointer.Y - RenderPipelineGrab.Y};
					RenderPipelineDirty = true;
				}
			} else {
				RenderPipelineDragging = Name{};
			}
		}

		// Release, which is where a wire lands.
		if (RenderPipelineWire.IsValid() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
			if (under.What == engine::nodeview::HitKind::Port) {
				if (engine::nodeview::Connect(RenderPipelineGraph, RenderPipelineWire, under.Port)) {
					RenderPipelineDirty = true;
				}
			}
			RenderPipelineWire = {};
		}

		// Right-click without a drag opens a menu.
		// **A release that did not travel is a click.** Testing the drag delta
		// rather than "was the button released" is what keeps a pan from ending
		// in a menu every time.
		const ImVec2 travelled = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
		if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
			travelled.x * travelled.x + travelled.y * travelled.y < 16.0f) {
			if (under.What == engine::nodeview::HitKind::None) {
				RenderPipelineAddAt = pointer;
				RenderPipelineSearch.clear();
				ImGui::OpenPopup("add-node");
			} else {
				RenderPipelineSelected = under.Node;
				ImGui::OpenPopup("node-menu");
			}
		}

		if (ImGui::IsWindowFocused() && RenderPipelineSelected.IsValid() &&
			ImGui::IsKeyPressed(ImGuiKey_Delete)) {
			if (engine::nodeview::RemoveNode(RenderPipelineGraph, RenderPipelineSelected)) {
				RenderPipelineSelected = Name{};
				RenderPipelineDirty = true;
			}
		}

		// --- drawing -----------------------------------------------------------

		// Wires first, so a box is never drawn under its own.
		for (const EditorLink &link : engine::nodeview::LinksOf(RenderPipelineGraph)) {
			const EditorNode *from = RenderPipelineGraph.Find(link.FromNode);
			const EditorNode *to = RenderPipelineGraph.Find(link.ToNode);
			if (from == nullptr || to == nullptr) {
				continue;
			}
			Wire(list,
				 Screen(RenderPipelineCanvas,
						engine::nodeview::PortAt(*from, PortDirection::Output, link.FromSlot, STYLE), corner),
				 Screen(RenderPipelineCanvas,
						engine::nodeview::PortAt(*to, PortDirection::Input, link.ToSlot, STYLE), corner),
				 ColourOf(RenderPipelineGraph.KindOf(link.Resource)),
				 2.0f * RenderPipelineCanvas.Zoom);
		}

		// The wire in flight, tinted by whether it could be dropped here. **The
		// verdict is shown while the wire is still in the air**, which is the
		// whole point of a typed editor — a refusal after the fact is a compile
		// error with extra steps.
		if (RenderPipelineWire.IsValid()) {
			const EditorNode *held = RenderPipelineGraph.Find(RenderPipelineWire.Node);
			const engine::nodeview::DropVerdict verdict = engine::nodeview::EvaluateDrop(
				RenderPipelineGraph,
				RenderPipelineWire,
				under.What == engine::nodeview::HitKind::Port ? under.Port : PortRef{}
			);

			if (held != nullptr) {
				const ImU32 ink = verdict.Allowed	  ? Colour(0.55f, 0.95f, 0.55f)
								  : verdict.Why.empty() ? Colour(0.75f, 0.75f, 0.80f)
														: Colour(0.95f, 0.45f, 0.40f);
				Wire(list,
					 Screen(RenderPipelineCanvas,
							engine::nodeview::PortAt(
								*held, RenderPipelineWire.Direction, RenderPipelineWire.Slot, STYLE
							),
							corner),
					 mouse,
					 ink,
					 2.5f * RenderPipelineCanvas.Zoom);

				if (!verdict.Why.empty()) {
					ImGui::SetTooltip("%s", verdict.Why.c_str());
				}
			}
		}

		for (const EditorNode &node : RenderPipelineGraph.Nodes) {
			const engine::graph::NodeKindSpec *spec = engine::graph::NodeCatalogue::Find(node.Kind);
			const float height = engine::nodeview::HeightOf(node, STYLE);

			const ImVec2 a = Screen(RenderPipelineCanvas, node.At, corner);
			const ImVec2 b = Screen(
				RenderPipelineCanvas, Point{node.At.X + STYLE.Width, node.At.Y + height}, corner
			);
			const float radius = 5.0f * RenderPipelineCanvas.Zoom;
			const bool selected = node.Name == RenderPipelineSelected;

			list.AddRectFilled(a, b, Colour(0.13f, 0.14f, 0.17f, node.Enabled ? 0.98f : 0.55f), radius);
			list.AddRectFilled(
				a,
				ImVec2{b.x, a.y + STYLE.HeaderHeight * RenderPipelineCanvas.Zoom},
				spec != nullptr ? ColourOf(spec->Category) : Colour(0.2f, 0.2f, 0.2f),
				radius,
				ImDrawFlags_RoundCornersTop
			);
			list.AddRect(
				a,
				b,
				selected ? Colour(0.95f, 0.78f, 0.35f) : Colour(0.32f, 0.36f, 0.42f),
				radius,
				0,
				selected ? 2.5f : 1.0f
			);

			const std::string title(node.Name.Text());
			list.AddText(
				ImVec2{a.x + 8.0f * RenderPipelineCanvas.Zoom, a.y + 6.0f * RenderPipelineCanvas.Zoom},
				Colour(0.92f, 0.94f, 0.97f, node.Enabled ? 1.0f : 0.55f),
				title.c_str()
			);

			if (spec == nullptr) {
				continue;
			}

			// The ports, with their slot names beside them. **Drawn from the
			// catalogue and filled from the binding**, so an empty slot is a
			// hollow dot with a label rather than a gap somebody has to guess at.
			const auto side = [&](PortDirection direction, const std::vector<engine::graph::PortSpec> &ports,
								  const std::vector<Name> &bound) {
				for (uint32_t slot = 0; slot < ports.size(); slot++) {
					const ImVec2 dot = Screen(
						RenderPipelineCanvas, engine::nodeview::PortAt(node, direction, slot, STYLE), corner
					);
					const ImU32 ink = ColourOf(ports[slot].Kind);
					const bool filled = slot < bound.size() && bound[slot].IsValid();

					list.AddCircleFilled(
						dot,
						STYLE.PortRadius * RenderPipelineCanvas.Zoom,
						filled ? ink : Colour(0.13f, 0.14f, 0.17f)
					);
					list.AddCircle(dot, STYLE.PortRadius * RenderPipelineCanvas.Zoom, ink, 0, 1.5f);

					const std::string label(ports[slot].Name.Text());
					const float width = ImGui::CalcTextSize(label.c_str()).x;
					const float inset = 10.0f * RenderPipelineCanvas.Zoom;
					list.AddText(
						ImVec2{
							direction == PortDirection::Input ? dot.x + inset : dot.x - inset - width,
							dot.y - ImGui::GetTextLineHeight() * 0.5f,
						},
						Colour(0.72f, 0.76f, 0.82f),
						label.c_str()
					);
				}
			};

			side(PortDirection::Input, spec->Inputs, node.Inputs);
			side(PortDirection::Output, spec->Outputs, node.Outputs);
		}

		list.PopClipRect();

		// --- the menus ---------------------------------------------------------

		if (ImGui::BeginPopup("add-node")) {
			ImGui::TextDisabled("add a node");
			ImGui::SetNextItemWidth(200.0f);
			if (ImGui::IsWindowAppearing()) {
				ImGui::SetKeyboardFocusHere();
			}
			TextField("##search", RenderPipelineSearch, "search");

			ImGui::Separator();
			for (const engine::nodeview::CatalogueMatch &match :
				 engine::nodeview::SearchCatalogue(RenderPipelineSearch)) {
				const std::string label = match.Spec->Label.empty() ? std::string(match.Spec->Kind.Text())
																	: match.Spec->Label;
				if (ImGui::MenuItem(label.c_str())) {
					const Name added = engine::nodeview::AddNode(
						RenderPipelineGraph, match.Spec->Kind, RenderPipelineAddAt
					);
					RenderPipelineSelected = added;
					RenderPipelineDirty = true;
				}
				if (!match.Spec->Summary.empty() && ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", match.Spec->Summary.c_str());
				}
			}
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopup("node-menu")) {
			if (EditorNode *node = RenderPipelineGraph.Find(RenderPipelineSelected)) {
				ImGui::TextDisabled("%s", std::string(node->Name.Text()).c_str());
				ImGui::Separator();

				// **Disabled and deleted are different things and the menu keeps
				// them apart**, which is `graph::Node::Enabled`'s whole reason: a
				// pass nobody demanded is dead, and one somebody switched off is
				// disabled.
				if (ImGui::MenuItem(node->Enabled ? "Disable" : "Enable")) {
					node->Enabled = !node->Enabled;
					RenderPipelineDirty = true;
				}
				if (ImGui::MenuItem("Delete", "Del")) {
					engine::nodeview::RemoveNode(RenderPipelineGraph, RenderPipelineSelected);
					RenderPipelineSelected = Name{};
					RenderPipelineDirty = true;
				}
			}
			ImGui::EndPopup();
		}

		// --- the status line ---------------------------------------------------

		ImGui::SetCursorScreenPos(ImVec2{corner.x, corner.y + area.y + 4.0f});
		if (builds != engine::graph::PipelineDocumentStatus::Ok) {
			ImGui::TextColored(
				ImVec4{0.95f, 0.5f, 0.4f, 1.0f},
				"will not build: %s (%s)",
				engine::graph::Describe(builds),
				offender.IsValid() ? std::string(offender.Text()).c_str() : "?"
			);
		} else if (compiles != engine::graph::GraphStatus::Ok) {
			ImGui::TextColored(
				ImVec4{0.95f, 0.7f, 0.35f, 1.0f},
				"will not run: %s (%s)",
				engine::graph::Describe(compiles),
				offender.IsValid() ? std::string(offender.Text()).c_str() : "?"
			);
		} else {
			ImGui::TextDisabled(
				"%zu node(s), %zu wire(s) — runs in %zu step(s)",
				RenderPipelineGraph.Nodes.size(),
				engine::nodeview::LinksOf(RenderPipelineGraph).size(),
				compiled.Shared.size() + compiled.PerView.size() + compiled.Final.size()
			);
		}

		ImGui::End();
	}

	// --- the Assets Pipeline ---------------------------------------------------

	void Editor::DrawAssetsPipeline() {
		if (!ShowAssetsPipeline) {
			return;
		}

		if (!ImGui::Begin("Assets Pipeline", &ShowAssetsPipeline)) {
			ImGui::End();
			return;
		}

		// **Nothing authored yet, and the panel says so rather than looking
		// broken.** A world carries no bake documents until the format question
		// in the v0.11 roadmap is settled — `bake` holds the image and model
		// decoders, so `game` cannot embed them the way it embeds render
		// pipelines. Until then this draws an empty canvas over an empty
		// document, which is the same code path a populated one takes.
		const engine::bake::Document document;
		const engine::nodeview::AssetLayout layout = engine::nodeview::LayoutAssets(document);

		ImGui::TextDisabled(
			"no asset pipeline stored in this world yet — see DEFERRED.md D00039 and the v0.11 roadmap"
		);
		ImGui::Separator();

		if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
			const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
			engine::nodeview::Pan(AssetsPipelineState, delta.x, delta.y);
			ImGui::ResetMouseDragDelta(ImGuiMouseButton_Right);
		}

		ImDrawList &list = *ImGui::GetWindowDrawList();
		const ImVec2 base = ImGui::GetCursorScreenPos();
		const ImVec2 origin{base.x + AssetsPipelineState.PanX, base.y + AssetsPipelineState.PanY};

		for (const engine::nodeview::PlacedAsset &placed : layout.Nodes) {
			const float left = ASSET_MARGIN + static_cast<float>(placed.Column) * (ASSET_WIDTH + ASSET_GAP);
			const float top = ASSET_MARGIN + static_cast<float>(placed.Row) * (ASSET_HEIGHT + ASSET_GAP);

			const ImVec2 a{origin.x + left, origin.y + top};
			const ImVec2 b{a.x + ASSET_WIDTH, a.y + ASSET_HEIGHT};
			list.AddRectFilled(a, b, Colour(0.24f, 0.20f, 0.28f), 4.0f);
			list.AddRect(a, b, Colour(0.35f, 0.39f, 0.46f), 4.0f);
			list.AddText(
				ImVec2{a.x + 8.0f, a.y + 8.0f},
				Colour(0.88f, 0.90f, 0.94f),
				placed.Kind.data(),
				placed.Kind.data() + placed.Kind.size()
			);
		}

		ImGui::End();
	}
}
