// The Render Pipeline and Assets Pipeline node editors.
//
// **The panel, and nothing else.** Everything that decides what a canvas looks
// like is `graph::PipelineView` and `nodeview` — where a node sits, what joins
// two of them, what is under a point, what is selected. All of that is arithmetic
// and all of it has tests. What is here is the part that cannot have any: turning
// a mouse into those calls, and turning their answers into rectangles.
//
// **Drawn with an ImGui draw list**, which is `DEFERRED.md` D00041's decision. A
// `gui` subtree cannot live inside an `ImGui::Begin` block — the two are separate
// renderers with separate draw lists — so the engine's own tree would need a
// render target per open editor. This is the editor's chrome rather than a game's
// interface, so it draws the way the rest of the editor does.

#include <studio/Editor.hpp>

#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/PipelineView.hpp>
#include <engine/nodeview/Assets.hpp>

#include <imgui.h>

namespace studio {

	namespace {
		// The spacing both canvases are laid out and hit-tested with. One value
		// rather than two, because `nodeview::PickAt` takes the style it was
		// built with and passing a different one picks the wrong node.
		const engine::nodeview::CanvasStyle STYLE;

		ImU32 Colour(float r, float g, float b, float a = 1.0f) {
			return ImGui::GetColorU32(ImVec4{r, g, b, a});
		}

		// Where the canvas's origin sits on screen, given the panel's scroll.
		ImVec2 OriginOf(const engine::nodeview::CanvasState &state) {
			const ImVec2 corner = ImGui::GetCursorScreenPos();
			return ImVec2{corner.x + state.PanX, corner.y + state.PanY};
		}

		// Drags the canvas with the right mouse button held, which is what every
		// node editor does and what leaves the left button free for selection.
		void PanWithMouse(engine::nodeview::CanvasState &state) {
			if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
				const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
				engine::nodeview::Pan(state, delta.x, delta.y);
				ImGui::ResetMouseDragDelta(ImGuiMouseButton_Right);
			}
		}

		// One box, plus its label.
		void DrawBox(
			ImDrawList &list, const ImVec2 &origin, float left, float top, std::string_view text,
			bool selected, ImU32 fill
		) {
			const ImVec2 a{origin.x + left, origin.y + top};
			const ImVec2 b{a.x + STYLE.NodeWidth, a.y + STYLE.NodeHeight};

			list.AddRectFilled(a, b, fill, 4.0f);
			list.AddRect(
				a,
				b,
				selected ? Colour(0.95f, 0.78f, 0.35f) : Colour(0.35f, 0.39f, 0.46f),
				4.0f,
				0,
				selected ? 2.5f : 1.0f
			);
			list.AddText(ImVec2{a.x + 8.0f, a.y + 8.0f}, Colour(0.88f, 0.90f, 0.94f), text.data(),
						 text.data() + text.size());
		}
	}

	void Editor::DrawRenderPipeline() {
		if (!ShowRenderPipeline) {
			return;
		}

		if (!ImGui::Begin("Render Pipeline", &ShowRenderPipeline)) {
			ImGui::End();
			return;
		}

		// **The world's pipeline when it has one, the engine's frame otherwise.**
		// A world that has never been near this editor still has a frame, and
		// showing it is what lets somebody see what they are about to change.
		engine::graph::RenderGraph graph = engine::graph::StandardGraph();
		bool authored = false;

		if (const engine::world::WorldId world = Active; world.IsValid() && Universe != nullptr) {
			Universe->Enter(world, [&graph, &authored](engine::ecs::Store &store) {
				const auto *set = store.Resource<engine::graph::PipelineSet>();
				if (set == nullptr) {
					return;
				}
				const auto *document = set->Find(engine::core::Name("main"));
				if (document == nullptr) {
					return;
				}

				engine::graph::RenderGraph built;
				engine::core::Name offender;
				if (engine::graph::Build(*document, built, offender) ==
					engine::graph::PipelineDocumentStatus::Ok) {
					graph = std::move(built);
					authored = true;
				}
			});
		}

		ImGui::TextDisabled("%s", authored ? "this world's pipeline" : "the engine's standard frame");
		ImGui::Separator();

		engine::graph::CompiledGraph compiled;
		engine::core::Name offender;
		if (graph.Compile(compiled, offender) != engine::graph::GraphStatus::Ok) {
			// A pipeline that will not compile is the case an editor exists to
			// show, so it says which node and stops rather than drawing nothing.
			ImGui::TextColored(
				ImVec4{0.95f, 0.5f, 0.4f, 1.0f},
				"will not compile: %s",
				offender.IsValid() ? std::string(offender.Text()).c_str() : "?"
			);
			ImGui::End();
			return;
		}

		const engine::graph::PipelineLayout layout = engine::graph::LayoutPipeline(graph, compiled);

		PanWithMouse(RenderPipelineState);

		const ImVec2 corner = ImGui::GetCursorScreenPos();
		if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			const ImVec2 mouse = ImGui::GetMousePos();
			engine::nodeview::Click(
				RenderPipelineState, layout, STYLE, mouse.x - corner.x, mouse.y - corner.y
			);
		}

		ImDrawList &list = *ImGui::GetWindowDrawList();
		const ImVec2 origin = OriginOf(RenderPipelineState);

		// Edges first, so a box is never drawn under its own lines.
		for (const engine::graph::PlacedEdge &edge : layout.Edges) {
			const auto centre = [&](engine::graph::NodeId id) {
				for (const engine::graph::PlacedNode &placed : layout.Nodes) {
					if (placed.Node == id) {
						return ImVec2{
							origin.x + STYLE.Margin +
								static_cast<float>(placed.Column) * (STYLE.NodeWidth + STYLE.ColumnGap) +
								STYLE.NodeWidth * 0.5f,
							origin.y + STYLE.Margin +
								static_cast<float>(placed.Where) *
									(STYLE.NodeHeight + STYLE.BandGap + STYLE.RowGap) +
								STYLE.NodeHeight * 0.5f,
						};
					}
				}
				return ImVec2{0.0f, 0.0f};
			};

			list.AddLine(centre(edge.From), centre(edge.To), Colour(0.42f, 0.47f, 0.55f), 1.5f);
		}

		for (const engine::graph::PlacedNode &placed : layout.Nodes) {
			const float left =
				STYLE.Margin + static_cast<float>(placed.Column) * (STYLE.NodeWidth + STYLE.ColumnGap);
			const float top = STYLE.Margin + static_cast<float>(placed.Where) *
												 (STYLE.NodeHeight + STYLE.BandGap + STYLE.RowGap);

			DrawBox(
				list,
				origin,
				left,
				top,
				placed.Name.Text(),
				engine::nodeview::IsSelected(RenderPipelineState, placed.Name),
				placed.Where == engine::graph::Band::PerView ? Colour(0.20f, 0.26f, 0.22f)
															 : Colour(0.16f, 0.22f, 0.30f)
			);
		}

		// The inspection stage, in its smallest honest form: what is selected and
		// which band it runs in.
		if (RenderPipelineState.Selected.IsValid()) {
			for (const engine::graph::PlacedNode &placed : layout.Nodes) {
				if (placed.Name == RenderPipelineState.Selected) {
					ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 28.0f);
					ImGui::Text(
						"%s — %s",
						std::string(placed.Name.Text()).c_str(),
						engine::graph::Describe(placed.Where)
					);
				}
			}
		}

		ImGui::End();
	}

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

		PanWithMouse(AssetsPipelineState);

		ImDrawList &list = *ImGui::GetWindowDrawList();
		const ImVec2 origin = OriginOf(AssetsPipelineState);

		for (const engine::nodeview::PlacedAsset &placed : layout.Nodes) {
			const float left =
				STYLE.Margin + static_cast<float>(placed.Column) * (STYLE.NodeWidth + STYLE.ColumnGap);
			const float top =
				STYLE.Margin + static_cast<float>(placed.Row) * (STYLE.NodeHeight + STYLE.RowGap);

			DrawBox(list, origin, left, top, placed.Kind, false, Colour(0.24f, 0.20f, 0.28f));
		}

		ImGui::End();
	}
}
