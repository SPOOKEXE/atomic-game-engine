// What has changed since the file on disk was written.
//
// **`Modified` is a bool, and a bool is not an answer.** It says something has
// changed and nothing about what, so the only way to find out before saving is
// to save and then read a version-control diff — which is the wrong end of the
// operation to discover you moved something you did not mean to.
//
// The whole feature rests on a property this engine already has: **a game is
// text.** `game::WriteGame` produces the same document `SaveGame` writes, so
// "what would be saved" is a string, "what is saved" is a string, and the
// question is a diff. A binary scene format could not answer this at all
// without a bespoke comparator per record type.
//
// ## Why the algorithm is here and not in the panel
//
// The diff is the half that can be silently wrong — a comparator that reports
// no changes looks exactly like a clean tree — and it is pure text in, text out.
// So it is a free function with its own suite, and the panel is a reader over
// what it returns.

#include <algorithm>
#include <fstream>
#include <imgui.h>
#include <sstream>
#include <string>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <vector>

#include <engine/game/Game.hpp>
#include <engine/ui/Theme.hpp>

namespace studio {

	namespace {
		std::vector<std::string> SplitLines(std::string_view text) {
			std::vector<std::string> lines;
			size_t start = 0;

			while (start <= text.size()) {
				const size_t end = text.find('\n', start);
				if (end == std::string_view::npos) {
					// A trailing newline ends the text rather than starting an
					// empty last line, which is what every diff tool does and
					// what stops a file gaining a phantom row on every save.
					if (start < text.size()) {
						lines.emplace_back(text.substr(start));
					}
					break;
				}
				lines.emplace_back(text.substr(start, end - start));
				start = end + 1;
			}
			return lines;
		}
	}

	std::vector<DiffLine> DiffText(std::string_view before, std::string_view after, bool *coarse) {
		const std::vector<std::string> left = SplitLines(before);
		const std::vector<std::string> right = SplitLines(after);

		std::vector<DiffLine> out;
		if (coarse != nullptr) {
			*coarse = false;
		}

		// **Common prefix and suffix first.** Two saves of one game are almost
		// entirely identical, so trimming the ends turns a whole-document
		// comparison into one over the handful of lines somebody touched — which
		// is what keeps the table below affordable.
		size_t head = 0;
		while (head < left.size() && head < right.size() && left[head] == right[head]) {
			head++;
		}

		size_t tail = 0;
		while (tail < left.size() - head && tail < right.size() - head &&
			   left[left.size() - 1 - tail] == right[right.size() - 1 - tail]) {
			tail++;
		}

		const size_t leftCount = left.size() - head - tail;
		const size_t rightCount = right.size() - head - tail;

		if (leftCount == 0 && rightCount == 0) {
			return out;
		}

		// **Bounded, and it says so when it gives up.** The table below is
		// O(n·m); on a scene where somebody rewrote everything that is a
		// multi-second stall in a frame. Past the cap the answer degrades to
		// "this block changed", which is still true and still useful — and the
		// panel reports that it degraded rather than presenting a coarse diff as
		// a fine one.
		if (leftCount * rightCount > DIFF_CELL_LIMIT) {
			if (coarse != nullptr) {
				*coarse = true;
			}
			for (size_t index = 0; index < leftCount; index++) {
				out.push_back(DiffLine{DiffKind::Removed, left[head + index]});
			}
			for (size_t index = 0; index < rightCount; index++) {
				out.push_back(DiffLine{DiffKind::Added, right[head + index]});
			}
			return out;
		}

		// Longest common subsequence over the middle. The table is (n+1)·(m+1)
		// of lengths; the walk back out of it is what produces the run of
		// same/removed/added rows.
		std::vector<std::vector<uint32_t>> lengths(leftCount + 1, std::vector<uint32_t>(rightCount + 1, 0));

		for (size_t row = leftCount; row-- > 0;) {
			for (size_t column = rightCount; column-- > 0;) {
				if (left[head + row] == right[head + column]) {
					lengths[row][column] = lengths[row + 1][column + 1] + 1;
				} else {
					lengths[row][column] = std::max(lengths[row + 1][column], lengths[row][column + 1]);
				}
			}
		}

		size_t row = 0;
		size_t column = 0;
		while (row < leftCount && column < rightCount) {
			if (left[head + row] == right[head + column]) {
				out.push_back(DiffLine{DiffKind::Same, left[head + row]});
				row++;
				column++;
			} else if (lengths[row + 1][column] >= lengths[row][column + 1]) {
				out.push_back(DiffLine{DiffKind::Removed, left[head + row]});
				row++;
			} else {
				out.push_back(DiffLine{DiffKind::Added, right[head + column]});
				column++;
			}
		}

		for (; row < leftCount; row++) {
			out.push_back(DiffLine{DiffKind::Removed, left[head + row]});
		}
		for (; column < rightCount; column++) {
			out.push_back(DiffLine{DiffKind::Added, right[head + column]});
		}

		return out;
	}

	void Editor::DrawDiff() {
		if (!ShowDiff) {
			return;
		}

		if (!ImGui::Begin("Changes", &ShowDiff)) {
			ImGui::End();
			return;
		}

		if (GamePath.empty()) {
			// **Not "no changes".** A game that has never been saved has
			// everything to save, and reporting that as a clean tree is the one
			// wrong answer this panel could give.
			ImGui::TextDisabled("this game has never been saved — there is nothing to compare against");
			ImGui::End();
			return;
		}

		if (ImGui::Button("Refresh") || DiffRows.empty()) {
			RefreshDiff();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%s", GamePath.string().c_str());

		ImGui::Separator();

		if (!DiffError.empty()) {
			ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "%s", DiffError.c_str());
			ImGui::End();
			return;
		}

		size_t added = 0;
		size_t removed = 0;
		for (const DiffLine &line : DiffRows) {
			added += line.Kind == DiffKind::Added ? 1 : 0;
			removed += line.Kind == DiffKind::Removed ? 1 : 0;
		}

		if (added == 0 && removed == 0) {
			ImGui::TextDisabled("no changes — the file matches what is open");
			ImGui::End();
			return;
		}

		ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f), "+%zu", added);
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "-%zu", removed);
		if (DiffCoarse) {
			ImGui::SameLine();
			ImGui::TextColored(
				ImVec4(0.85f, 0.55f, 0.2f, 1.0f), "· too large to align, showing the block"
			);
		}

		if (ImGui::BeginChild("##diff-rows")) {
			for (const DiffLine &line : DiffRows) {
				switch (line.Kind) {
					case DiffKind::Added:
						ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f), "+ %s", line.Text.c_str());
						break;
					case DiffKind::Removed:
						ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "- %s", line.Text.c_str());
						break;
					case DiffKind::Same:
						ImGui::TextDisabled("  %s", line.Text.c_str());
						break;
				}
			}
		}
		ImGui::EndChild();

		ImGui::End();
	}

	void Editor::RefreshDiff() {
		DiffRows.clear();
		DiffError.clear();
		DiffCoarse = false;

		if (Universe == nullptr || GamePath.empty()) {
			return;
		}

		std::ifstream in(GamePath, std::ios::binary);
		if (!in) {
			DiffError = "could not read " + GamePath.string();
			return;
		}

		std::ostringstream buffer;
		buffer << in.rdbuf();
		const std::string saved = buffer.str();

		// **The same writer `SaveGame` uses**, which is what makes this a
		// preview of the save rather than an approximation of one. A second
		// serialiser here would be a second answer to what a game is, and the
		// two would disagree the first time one was changed.
		const std::string live = engine::game::WriteGame(*Universe, GameName);

		DiffRows = DiffText(saved, live, &DiffCoarse);
	}
}
