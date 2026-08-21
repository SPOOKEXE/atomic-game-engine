#pragma once

// The handful of glyphs a panel draws rather than writes.
//
// **Drawn with `ImDrawList` rather than set in a font, and the reason is that
// there is no font to set them in.** `mono.vendor/fonts/` holds Inter, JetBrains
// Mono, Roboto and Noto Sans; none of the four carries a folder or a page
// pictograph, and Noto Sans in particular is the *text* face rather than Noto
// Emoji, so a literal `U+1F4C1` in a label renders as a box. Vendoring a fifth
// font and a licence for two shapes is the wrong trade.
//
// What is drawn instead is two rectangles and a tab, in the caller's current
// text colour, sized off the current line height. That scales with
// `InterfaceSettings::Scale` for free, needs no atlas, and cannot go missing on
// a machine where a font failed to load.
//
// **Deliberately not an icon set.** This is not the beginning of a library of
// sixty pictographs - two shapes earn their place because a browse button that
// does not say whether it will ask for a file or a folder is a button somebody
// has to press to find out. A third icon needs an argument of its own.
//
// @tier L12 · client
// @since v0.18

namespace engine::ui {

	// Draws a folder at the cursor and advances past it.
	//
	// @param size The square's side in pixels, or 0 for the current line height.
	void FolderIcon(float size = 0.0f);

	// Draws a page with a folded corner at the cursor and advances past it.
	//
	// @param size The square's side in pixels, or 0 for the current line height.
	void FileIcon(float size = 0.0f);

	// A button whose face is a folder rather than a word.
	//
	// **`id` is an imgui id and never shows.** Two browse buttons on one page
	// with the same id are one button that both rows fight over, which is
	// imgui's rule rather than this function's - so it is a parameter rather
	// than a constant here.
	//
	// @param id      The imgui id, `##`-prefixed by the caller if it likes.
	// @param tooltip Shown on hover, because an icon alone is a guess.
	// @return `true` on the frame it was clicked.
	bool FolderButton(const char *id, const char *tooltip);

	// A button whose face is a page. See `FolderButton`.
	//
	// @param id      The imgui id.
	// @param tooltip Shown on hover.
	// @return `true` on the frame it was clicked.
	bool FileButton(const char *id, const char *tooltip);
}
