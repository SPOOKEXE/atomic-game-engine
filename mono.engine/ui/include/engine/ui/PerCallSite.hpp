#pragma once

// State that belongs to a call site rather than to the editor.
//
// **Three widgets grew the same fifteen lines independently**, and the third
// one's comment says so: *"Same shape as the class picker's per-id search
// state."* A widget drawn from two places needs two copies of whatever it is
// remembering - the toolbar's class filter and the tree's class filter are two
// lists with two queries, and one shared record would make typing in one filter
// the other. The lookup is by the id the caller already passes imgui.
//
// This is the one place that pattern is written, so a fourth widget that needs
// it gets the tested version rather than a fourth transcription.
//
// **Public here since v0.18, having been a private header of `mono.studio`.**
// `ui::FilePrompt` moved into this module for `Browse.hpp`'s reason and brought
// its per-title state with it, so the pattern had to become reachable. Nothing
// about it was ever editor-specific.
//
// **Deliberately not on any one program's state.** None of this is world state, none of it
// survives a frame boundary in any meaningful sense, and none of it is worth
// 120 more bytes on a class that already has too many members. It is the same
// judgement `ui::InstallThemeSettings` makes for one value.
//
// @tier L12 · client
//
// The table is a vector scanned linearly rather than a map: the number of call
// sites for any one widget is the number of places somebody wrote it in the
// source, which is three at the most, and a hash of a short string costs more
// than comparing three of them.

#include <string>
#include <utility>
#include <vector>

namespace engine::ui {

	// The state this call site has, creating it on first use.
	//
	// **The reference stays valid for the call and no longer.** The table is a
	// vector and the next new id reallocates it, so use the reference and let
	// it go - do not store one across frames.
	//
	// @tparam T   What this widget remembers. Default-constructed on first use.
	// @param  id  The caller's imgui id, which is what separates the copies.
	// @return The state for that id.
	template <class T> T &PerCallSite(const char *id) {
		// One table per `T`, which is what keeps two widgets' state apart
		// without either of them naming the other.
		static std::vector<std::pair<std::string, T>> table;

		for (auto &entry : table) {
			if (entry.first == id) {
				return entry.second;
			}
		}
		return table.emplace_back(id, T{}).second;
	}
}
