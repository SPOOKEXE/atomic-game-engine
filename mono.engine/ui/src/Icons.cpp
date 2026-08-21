#include <engine/ui/Icons.hpp>

#include <imgui.h>
#include <imgui_internal.h>

namespace engine::ui {

	namespace {
		// The side of the square an icon is drawn inside, and the top-left it
		// starts at. Taken from the line height so the glyph matches whatever
		// text it sits beside at any interface scale.
		struct IconBox {
			ImVec2 Origin;
			float Side = 0.0f;
		};

		IconBox BoxAt(ImVec2 origin, float size) {
			IconBox box;
			box.Side = size > 0.0f ? size : ImGui::GetTextLineHeight();
			box.Origin = origin;
			return box;
		}

		// **A folder is a tab, a body, and the gap between them.** The gap is
		// what makes it read as a folder rather than as a rectangle with a bump:
		// without it the two shapes merge at small sizes into one blob, and the
		// button stops saying anything at all.
		void DrawFolder(ImDrawList *list, const IconBox &box, ImU32 colour) {
			const float side = box.Side;
			const float x = box.Origin.x;
			const float y = box.Origin.y;

			// Vertically centred in the line box, and inset a little so two
			// icons in a column do not touch.
			const float top = y + side * 0.22f;
			const float bottom = y + side * 0.82f;
			const float left = x + side * 0.06f;
			const float right = x + side * 0.94f;
			const float rounding = side * 0.08f;

			// The tab, on the back-left, drawn first so the body covers where
			// the two meet.
			list->AddRectFilled(
				ImVec2(left, top), ImVec2(left + (right - left) * 0.45f, top + side * 0.14f), colour, rounding
			);

			list->AddRectFilled(ImVec2(left, top + side * 0.10f), ImVec2(right, bottom), colour, rounding);
		}

		// A page with the top-right corner folded away, which is the shape that
		// reads as "a file" at sixteen pixels when a full document outline does
		// not.
		void DrawFile(ImDrawList *list, const IconBox &box, ImU32 colour) {
			const float side = box.Side;
			const float x = box.Origin.x;
			const float y = box.Origin.y;

			const float top = y + side * 0.16f;
			const float bottom = y + side * 0.86f;
			const float left = x + side * 0.20f;
			const float right = x + side * 0.80f;
			const float fold = side * 0.24f;

			const ImVec2 points[5] = {
				ImVec2(left, top),
				ImVec2(right - fold, top),
				ImVec2(right, top + fold),
				ImVec2(right, bottom),
				ImVec2(left, bottom),
			};
			list->AddConvexPolyFilled(points, 5, colour);

			// The fold itself, knocked back out of the page in the window's own
			// background so it reads as a corner turned over rather than as a
			// notch cut out of the silhouette.
			const ImVec2 corner[3] = {
				ImVec2(right - fold, top),
				ImVec2(right, top + fold),
				ImVec2(right - fold, top + fold),
			};
			list->AddConvexPolyFilled(corner, 3, ImGui::GetColorU32(ImGuiCol_WindowBg));
		}

		bool IconButton(const char *id, const char *tooltip, bool folder) {
			const float side = ImGui::GetTextLineHeight();
			const ImVec2 padding = ImGui::GetStyle().FramePadding;
			const ImVec2 size(side + padding.x * 2.0f, side + padding.y * 2.0f);

			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const bool clicked = ImGui::Button(id, size);

			// **After the button, so the glyph lands on top of its frame.**
			// imgui draws in submission order into one list, so an icon drawn
			// first would be painted over by the button's own background.
			const ImU32 colour = ImGui::GetColorU32(ImGuiCol_Text);
			const IconBox box = BoxAt(ImVec2(origin.x + padding.x, origin.y + padding.y), side);

			if (folder) {
				DrawFolder(ImGui::GetWindowDrawList(), box, colour);
			} else {
				DrawFile(ImGui::GetWindowDrawList(), box, colour);
			}

			if (tooltip != nullptr && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", tooltip);
			}
			return clicked;
		}
	}

	void FolderIcon(float size) {
		const IconBox box = BoxAt(ImGui::GetCursorScreenPos(), size);
		DrawFolder(ImGui::GetWindowDrawList(), box, ImGui::GetColorU32(ImGuiCol_Text));
		ImGui::Dummy(ImVec2(box.Side, box.Side));
	}

	void FileIcon(float size) {
		const IconBox box = BoxAt(ImGui::GetCursorScreenPos(), size);
		DrawFile(ImGui::GetWindowDrawList(), box, ImGui::GetColorU32(ImGuiCol_Text));
		ImGui::Dummy(ImVec2(box.Side, box.Side));
	}

	bool FolderButton(const char *id, const char *tooltip) {
		return IconButton(id, tooltip, true);
	}

	bool FileButton(const char *id, const char *tooltip) {
		return IconButton(id, tooltip, false);
	}
}
