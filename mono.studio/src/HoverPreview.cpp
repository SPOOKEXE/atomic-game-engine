// The big preview that follows the cursor.
//
// **Taken from `explorer-plus`, including the part that is easy to leave out.**
// Its hover preview is a 132-pixel panel that follows the pointer, and the
// detail that makes it usable is the delay:
//
//     const PREVIEW_DELAY = 0.25;
//     // Delayed so that running the cursor down a list doesn't flash thumbnails.
//
// Without it, dragging the cursor down a list of three hundred rows opens and
// closes three hundred previews — which is not merely ugly here, because opening
// one *decodes a mesh and uploads it*. The delay is what turns a hover preview
// from a strobe into a thing you point at.
//
// **And a token, so a superseded hover cannot land.** The reference bumps a
// counter on every hover and the delayed callback checks it still owns the
// hover before setting anything. Without that, moving from row A to row B inside
// the delay window shows A a quarter second after the cursor left it.
//
// **It flips rather than overflows.** Near the right or bottom edge the panel is
// placed on the other side of the cursor instead of running off the panel, which
// is the same rule the reference states and the reason a preview stays readable
// in the corner of a docked window.

#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Preview.hpp>

namespace studio {

	namespace {
		// How long the cursor has to rest on a row before its preview opens.
		//
		// A quarter second, matching the reference. Long enough that scanning a
		// list opens nothing, short enough that pointing at a row feels like it
		// answered.
		constexpr double HOVER_DELAY_SECONDS = 0.25;

		// How wide the preview panel is.
		constexpr float PANEL_SIDE = 132.0f;

		// Room under the picture for the asset's name.
		constexpr float CAPTION_HEIGHT = 18.0f;

		// How far from the cursor it sits.
		constexpr float CURSOR_OFFSET = 18.0f;
	}

	void Editor::HoverPreview(const std::string &name, engine::assets::AssetKind kind) {
		if (!ImGui::IsItemHovered()) {
			// **Only the row under the cursor clears it, and only if it is the
			// row that set it.** A list clears the hover once, after the loop —
			// see `EndHoverPreview` — because every row calling this would have
			// each one cancelling the next.
			return;
		}

		HoverCandidate = name;
		HoverKind = kind;
		HoverSeen = true;
	}

	void Editor::EndHoverPreview(double frameSeconds) {
		if (!HoverSeen) {
			// Nothing under the cursor this frame.
			HoverCandidate.clear();
			HoverElapsed = 0.0;
			HoverShowing.clear();
			return;
		}
		HoverSeen = false;

		if (HoverCandidate != HoverPending) {
			// **A different row: the clock restarts and nothing is showing.**
			// This is the token from the reference, expressed as state rather
			// than as a deferred callback — the effect is the same and there is
			// no closure to outlive the row it was made for.
			HoverPending = HoverCandidate;
			HoverElapsed = 0.0;
			HoverShowing.clear();
			return;
		}

		if (!HoverShowing.empty()) {
			return;
		}

		HoverElapsed += frameSeconds;
		if (HoverElapsed >= HOVER_DELAY_SECONDS) {
			HoverShowing = HoverPending;
		}
	}

	void Editor::DrawHoverPreview() {
		if (HoverShowing.empty()) {
			return;
		}

		// **Drawn as a foreground overlay rather than as a tooltip.** An imgui
		// tooltip is owned by the item that produced it and closes the moment
		// the cursor moves inside it; this has to survive the pointer travelling
		// and has to be able to hold a live 3D viewport.
		const ImVec2 cursor = ImGui::GetMousePos();
		const ImGuiViewport *main = ImGui::GetMainViewport();

		const float side = engine::ui::Scaled(PANEL_SIDE);
		const float caption = engine::ui::Scaled(CAPTION_HEIGHT);
		const float offset = engine::ui::Scaled(CURSOR_OFFSET);
		const float width = side;
		const float height = side + caption;

		// Flip rather than overflow, so it stays readable near an edge — the
		// reference's rule, unchanged.
		const float right = main->WorkPos.x + main->WorkSize.x;
		const float bottom = main->WorkPos.y + main->WorkSize.y;

		const float x = cursor.x + offset + width > right ? cursor.x - offset - width : cursor.x + offset;
		const float y = cursor.y + offset + height > bottom ? cursor.y - offset - height : cursor.y + offset;

		ImGui::SetNextWindowPos(ImVec2(std::max(x, main->WorkPos.x), std::max(y, main->WorkPos.y)));
		ImGui::SetNextWindowSize(ImVec2(width, height));

		if (!ImGui::Begin(
				"##hoverpreview",
				nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
					ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
					ImGuiWindowFlags_AlwaysAutoResize
			)) {
			ImGui::End();
			return;
		}

		const float picture = side - ImGui::GetStyle().WindowPadding.y * 2.0f;

		PreviewState state = PreviewState::Pending;

		// **A material joins the mesh path rather than the thumbnail one**, and
		// for the same reason: a `.amat` is a reference and has no picture of its
		// own, so a thumbnail of one resolves to `Unavailable` and draws a dash
		// for ever. What it wants is the engine's sphere wearing it, which is a
		// render — the same slot, the same rotation, the same turntable.
		if (PreviewIsRendered(HoverKind)) {
			// **A mesh is a render and not a bitmap** — `MeshPreview.cpp`
			// carries why. Loading is idempotent and cached, so calling it every
			// frame the preview is open costs a hash lookup.
			state = LoadPreviewMesh(HoverShowing);

			if (state == PreviewState::Ready && DrawPreviewViewport(HoverShowing, picture)) {
				if (void *texture = Renderer.SceneTexture(PREVIEW_SLOT); texture != nullptr) {
					const engine::render::SceneExtent extent = Renderer.SceneTextureExtent(PREVIEW_SLOT);

					// **Sampled to its extent rather than whole**, for the
					// viewport panel's reason: the texture is rounded up to a
					// block and the world fills the corner, so drawing all of it
					// would show the unwritten border down two edges.
					ImGui::Image(
						reinterpret_cast<ImTextureID>(texture),
						ImVec2(picture, picture),
						ImVec2(0.0f, 0.0f),
						ImVec2(extent.U, extent.V)
					);
				} else {
					// The first frame after the preview was asked for: the
					// rotation has not had its turn yet.
					ImGui::Dummy(ImVec2(picture, picture));
				}
			}
		} else {
			void *handle = ThumbnailFor(HoverShowing, state);
			if (handle != nullptr) {
				ImGui::Image(reinterpret_cast<ImTextureID>(handle), ImVec2(picture, picture));
			}
		}

		if (state != PreviewState::Ready) {
			// **A sentence, and a different one per reason.** One blank square
			// for four situations is what this replaces — `Preview.hpp` opens
			// with the argument.
			const ImVec2 corner = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(picture, picture));
			ImGui::GetWindowDrawList()->AddRect(
				ImVec2(corner.x, corner.y),
				ImVec2(corner.x + picture, corner.y + picture),
				ImGui::GetColorU32(ImGuiCol_Border)
			);

			if (const char *why = DescribePreview(state, HoverKind); why != nullptr) {
				const ImVec2 text = ImGui::CalcTextSize(why, nullptr, false, picture);
				ImGui::GetWindowDrawList()->AddText(
					nullptr,
					0.0f,
					ImVec2(corner.x + 4.0f, corner.y + (picture - text.y) * 0.5f),
					ImGui::GetColorU32(ImGuiCol_TextDisabled),
					why,
					nullptr,
					picture - 8.0f
				);
			}
		}

		// The name, truncated by the window rather than wrapped: a hash is
		// sixty-four characters and wrapping one would be four lines of hex.
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::TextUnformatted(HoverShowing.c_str());
		ImGui::PopStyleColor();

		ImGui::End();
	}
}
