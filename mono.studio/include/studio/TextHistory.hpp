#pragma once

// Document-owned text undo, storing UTF-8 aligned change ranges.

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace studio {
	// Cursor and selection as UTF-8 byte offsets.
	struct TextSelection {
		std::size_t Cursor = 0; // Active caret.
		std::size_t Start = 0;	// Selection start.
		std::size_t End = 0;	// Selection end.
	};
	// Retains reversible text changes with a soft limit on change payloads.
	// Keeps the latest edit even above the limit.
	// The current witness, allocation capacity and temporary copies are not counted.
	class TextHistory {
		// One reversible replacement and its surrounding selections.
		struct TextChange {
			std::size_t Offset = 0;	  // Replacement byte offset.
			std::string Removed;	  // Text before the edit.
			std::string Inserted;	  // Text after the edit.
			TextSelection Before;	  // Selection before the edit.
			TextSelection After;	  // Selection after the edit.
			bool Coalescible = false; // Whether adjacent typing may join this edit.
			double LastWhen = 0;	  // ImGui time of the last joined edit.
		};

	  public:
		// Set the soft payload budget in bytes.
		explicit TextHistory(std::size_t budget = 8 * 1024 * 1024) : Budget(budget) {}

		// Bind to new text and discard prior undo and redo entries.
		void Reset(std::string_view text) {
			std::string next(text);
			UndoEntries.clear();
			RedoEntries.clear();
			Bytes = 0;
			Observed = std::move(next);
			Ready = true;
		}
		// Reset and return false if text changed outside this history.
		bool Observe(std::string_view text) {
			if (!Ready || Observed != text) {
				Reset(text);
				return false;
			}
			return true;
		}
		// Record the change from the observed text, optionally joining adjacent typing.
		bool Record(
			std::string_view after,
			TextSelection beforeSelection,
			TextSelection afterSelection,
			bool typing = false,
			double now = 0
		) {
			if (!Ready || Observed == after) return false;
			std::size_t start = 0;
			while (start < Observed.size() && start < after.size() && Observed[start] == after[start])
				++start;
			const auto continuation = [](std::string_view value, std::size_t offset) {
				return offset < value.size() && (static_cast<unsigned char>(value[offset]) & 0xc0) == 0x80;
			};
			while (start > 0 && (continuation(Observed, start) || continuation(after, start)))
				--start;
			std::size_t suffix = 0;
			while (suffix < Observed.size() - start && suffix < after.size() - start &&
				   Observed[Observed.size() - 1 - suffix] == after[after.size() - 1 - suffix])
				++suffix;
			while (suffix > 0 && (continuation(Observed, Observed.size() - suffix) ||
								  continuation(after, after.size() - suffix)))
				--suffix;
			TextChange change{
				start,
				Observed.substr(start, Observed.size() - start - suffix),
				std::string(after.substr(start, after.size() - start - suffix)),
				beforeSelection,
				afterSelection,
				typing,
				now
			};
			std::string next(after);
			const std::size_t added = change.Removed.size() + change.Inserted.size();
			const bool merge =
				typing && RedoEntries.empty() && !UndoEntries.empty() && change.Removed.empty() &&
				!change.Inserted.empty() && UndoEntries.back().Coalescible &&
				UndoEntries.back().Removed.empty() &&
				change.Offset == UndoEntries.back().Offset + UndoEntries.back().Inserted.size() &&
				beforeSelection.Start == beforeSelection.End &&
				beforeSelection.Cursor == UndoEntries.back().After.Cursor &&
				now >= UndoEntries.back().LastWhen && now - UndoEntries.back().LastWhen <= 0.5;
			if (merge) {
				UndoEntries.back().Inserted += change.Inserted;
				UndoEntries.back().After = afterSelection;
				UndoEntries.back().LastWhen = now;
			} else {
				UndoEntries.push_back(std::move(change));
			}
			for (const auto &entry : RedoEntries)
				Bytes -= entry.Removed.size() + entry.Inserted.size();
			RedoEntries.clear();
			Bytes += added;
			while (UndoEntries.size() > 1 && Bytes > Budget) {
				Bytes -= UndoEntries.front().Removed.size() + UndoEntries.front().Inserted.size();
				UndoEntries.erase(UndoEntries.begin());
			}
			Observed = std::move(next);
			return true;
		}
		// Prevent a later edit from joining the latest typing transaction.
		void BreakCoalescing() {
			if (!UndoEntries.empty()) UndoEntries.back().Coalescible = false;
		}
		// Undo a change, restoring text and selection when the witness matches.
		bool Undo(std::string &text, TextSelection &selection) {
			return Move(text, selection, UndoEntries, RedoEntries, true);
		}
		// Redo a change, restoring text and selection when the witness matches.
		bool Redo(std::string &text, TextSelection &selection) {
			return Move(text, selection, RedoEntries, UndoEntries, false);
		}

	  private:
		bool Move(
			std::string &text,
			TextSelection &selection,
			std::vector<TextChange> &from,
			std::vector<TextChange> &to,
			bool undo
		) {
			if (!Observe(text) || from.empty()) return false;
			const TextChange &change = from.back();
			const std::string &removed = undo ? change.Inserted : change.Removed;
			const std::string &inserted = undo ? change.Removed : change.Inserted;
			if (change.Offset > text.size() || text.compare(change.Offset, removed.size(), removed) != 0) {
				Reset(text);
				return false;
			}
			std::string next = text;
			next.replace(change.Offset, removed.size(), inserted);
			std::string witness = next;
			to.push_back(change);
			selection = undo ? change.Before : change.After;
			const auto clamp = [&](std::size_t offset) {
				offset = std::min(offset, next.size());
				while (offset > 0 && offset < next.size() &&
					   (static_cast<unsigned char>(next[offset]) & 0xc0) == 0x80)
					--offset;
				return offset;
			};
			selection.Cursor = clamp(selection.Cursor);
			selection.Start = clamp(selection.Start);
			selection.End = clamp(selection.End);
			from.pop_back();
			if (!to.empty()) to.back().Coalescible = false;
			if (!from.empty()) from.back().Coalescible = false;
			text = std::move(next);
			Observed = std::move(witness);
			return true;
		}
		std::size_t Budget;
		std::size_t Bytes = 0;
		bool Ready = false;
		std::string Observed;
		std::vector<TextChange> UndoEntries;
		std::vector<TextChange> RedoEntries;
	};
}
