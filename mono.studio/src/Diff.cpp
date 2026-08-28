// What has changed since the file on disk was written.
//
// **`Modified` is a bool, and a bool is not an answer.** It says something has
// changed and nothing about what, so the only way to find out before saving is
// to save and then read a version-control diff - which is the wrong end of the
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
// The diff is the half that can be silently wrong - a comparator that reports
// no changes looks exactly like a clean tree - and it is pure text in, text out.
// So it is a free function with its own suite, and the panel is a reader over
// what it returns.

#include <engine/game/Game.hpp>
#include <engine/game/Xml.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <iomanip>
#include <sstream>
#include <string>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {

	namespace {
		constexpr size_t SAVED_CHANGE_MAX_BYTES = 128u * 1024u * 1024u;

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

		bool ReadText(const std::filesystem::path &path, std::string &out, std::string &error) {
			std::error_code sizeError;
			const uintmax_t size = std::filesystem::file_size(path, sizeError);
			if (!sizeError && size > SAVED_CHANGE_MAX_BYTES) {
				error = "change record exceeds 128 MiB: " + path.string();
				return false;
			}

			std::ifstream in(path, std::ios::binary);
			if (!in) {
				error = "could not read " + path.string();
				return false;
			}

			std::ostringstream buffer;
			buffer << in.rdbuf();
			if (!in.eof() && in.fail()) {
				error = "could not finish reading " + path.string();
				return false;
			}
			out = buffer.str();
			return true;
		}

		struct SaveTimestamp {
			std::string Text;
			std::string Filename;
		};

		SaveTimestamp NowTimestamp() {
			const auto now = std::chrono::system_clock::now();
			const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
			const auto milliseconds =
				std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
			const std::time_t value = std::chrono::system_clock::to_time_t(now);

			std::tm utc{};
#if defined(_WIN32)
			gmtime_s(&utc, &value);
#else
			gmtime_r(&value, &utc);
#endif

			std::ostringstream text;
			text << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
				 << milliseconds << 'Z';

			std::ostringstream filename;
			filename << std::put_time(&utc, "%Y%m%dT%H%M%S") << std::setw(3) << std::setfill('0')
					 << milliseconds << 'Z';
			return SaveTimestamp{text.str(), filename.str()};
		}

		void SetParseError(std::string *error, std::string_view reason) {
			if (error != nullptr) {
				*error = reason;
			}
		}
	}

	std::filesystem::path SavedChangesDirectory(const std::filesystem::path &game) {
		return game.parent_path() / ".atomic-changes" / (game.filename().string() + ".history");
	}

	std::string SavedChangeXml(const SavedChange &change) {
		engine::game::XmlWriter writer;
		writer.Open("Changes");
		writer.Attribute("format", "1");
		writer.Attribute("savedAt", change.SavedAt);
		writer.Open("Before");
		writer.Verbatim(change.Before);
		writer.Close();
		writer.Open("After");
		writer.Verbatim(change.After);
		writer.Close();
		writer.Close();
		return writer.Finish();
	}

	bool ParseSavedChangeXml(std::string_view text, SavedChange &out, std::string *error) {
		engine::game::XmlLimits limits;
		limits.MaximumBytes = SAVED_CHANGE_MAX_BYTES;
		limits.MaximumDepth = 4;
		limits.MaximumElements = 3;
		limits.MaximumAttributes = 4;

		engine::game::XmlDocument document;
		const engine::game::XmlStatus status = engine::game::ParseXml(text, document, limits);
		if (status != engine::game::XmlStatus::Ok) {
			SetParseError(error, std::string("invalid XML: ") + engine::game::Describe(status));
			return false;
		}

		const engine::game::XmlElement *root = document.Root();
		if (root == nullptr || root->Name != "Changes" || root->Attribute("format") != "1") {
			SetParseError(error, "expected Changes format 1");
			return false;
		}

		SavedChange parsed;
		parsed.SavedAt = root->Attribute("savedAt");
		if (parsed.SavedAt.empty()) {
			SetParseError(error, "missing savedAt");
			return false;
		}

		bool foundBefore = false;
		bool foundAfter = false;
		for (const uint32_t index : root->Children) {
			const engine::game::XmlElement *child = document.At(index);
			if (child == nullptr) {
				SetParseError(error, "invalid child index");
				return false;
			}
			if (child->Name == "Before" && !foundBefore) {
				parsed.Before = child->Text;
				foundBefore = true;
			} else if (child->Name == "After" && !foundAfter) {
				parsed.After = child->Text;
				foundAfter = true;
			} else {
				SetParseError(error, "unexpected or duplicate child");
				return false;
			}
		}

		if (!foundBefore || !foundAfter) {
			SetParseError(error, "missing Before or After");
			return false;
		}

		out = std::move(parsed);
		if (error != nullptr) {
			error->clear();
		}
		return true;
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
		// comparison into one over the handful of lines somebody touched - which
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
		// "this block changed", which is still true and still useful - and the
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
			ImGui::TextDisabled("this game has never been saved - there is nothing to compare against");
			ImGui::End();
			return;
		}

		if (ImGui::Button("Refresh")) {
			RefreshDiff();
			RefreshSavedChanges();
		}
		if (!DiffLoaded) {
			RefreshDiff();
		}
		if (SavedChangesFor != GamePath) {
			RefreshSavedChanges();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%s", GamePath.string().c_str());

		ImGui::Separator();

		const auto drawRows = [](const char *id, const std::vector<DiffLine> &rows, const bool coarse) {
			size_t added = 0;
			size_t removed = 0;
			for (const DiffLine &line : rows) {
				added += line.Kind == DiffKind::Added ? 1 : 0;
				removed += line.Kind == DiffKind::Removed ? 1 : 0;
			}

			if (added == 0 && removed == 0) {
				ImGui::TextDisabled("no changes");
				return;
			}

			ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.45f, 1.0f), "+%zu", added);
			ImGui::SameLine();
			ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "-%zu", removed);
			if (coarse) {
				ImGui::SameLine();
				ImGui::TextColored(
					ImVec4(0.85f, 0.55f, 0.2f, 1.0f), "too large to align, showing the changed block"
				);
			}

			if (ImGui::BeginChild(id)) {
				for (const DiffLine &line : rows) {
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
		};

		if (ImGui::BeginTabBar("##change-tabs")) {
			if (ImGui::BeginTabItem("Unsaved")) {
				if (!DiffError.empty()) {
					ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.35f, 1.0f), "%s", DiffError.c_str());
				} else if (DiffRows.empty()) {
					ImGui::TextDisabled("the file matches what is open");
				} else {
					drawRows("##unsaved-diff", DiffRows, DiffCoarse);
				}
				ImGui::EndTabItem();
			}

			if (ImGui::BeginTabItem("Saved History")) {
				if (!SavedChangesError.empty()) {
					ImGui::TextColored(ImVec4(0.85f, 0.55f, 0.2f, 1.0f), "%s", SavedChangesError.c_str());
				}

				if (SavedChanges.empty()) {
					ImGui::TextDisabled("no previous saves have been recorded");
				} else {
					SavedChangeSelection =
						std::clamp(SavedChangeSelection, 0, static_cast<int>(SavedChanges.size()) - 1);
					const SavedChange &selected = SavedChanges[static_cast<size_t>(SavedChangeSelection)];

					ImGui::SetNextItemWidth(320.0f);
					if (ImGui::BeginCombo("Record", selected.SavedAt.c_str())) {
						for (size_t index = 0; index < SavedChanges.size(); index++) {
							const bool current = static_cast<int>(index) == SavedChangeSelection;
							if (ImGui::Selectable(SavedChanges[index].SavedAt.c_str(), current)) {
								SavedChangeSelection = static_cast<int>(index);
								SavedDiffSource.clear();
							}
							if (current) {
								ImGui::SetItemDefaultFocus();
							}
						}
						ImGui::EndCombo();
					}

					const SavedChange &shown = SavedChanges[static_cast<size_t>(SavedChangeSelection)];
					if (SavedDiffSource != shown.Source) {
						SavedDiffRows = DiffText(shown.Before, shown.After, &SavedDiffCoarse);
						SavedDiffSource = shown.Source;
					}
					ImGui::TextDisabled("%s", shown.Source.filename().string().c_str());
					drawRows("##saved-diff", SavedDiffRows, SavedDiffCoarse);
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		ImGui::End();
	}

	void Editor::RefreshDiff() {
		DiffRows.clear();
		DiffError.clear();
		DiffCoarse = false;
		DiffLoaded = true;

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
		const std::string live = engine::game::WriteGame(*Universe, GameName, RenderingProfiles);

		DiffRows = DiffText(saved, live, &DiffCoarse);
	}

	void Editor::RefreshSavedChanges() {
		SavedChanges.clear();
		SavedChangesError.clear();
		SavedChangesFor = GamePath;
		SavedChangeSelection = 0;
		SavedDiffRows.clear();
		SavedDiffSource.clear();
		SavedDiffCoarse = false;

		if (GamePath.empty()) {
			return;
		}

		const std::filesystem::path directory = SavedChangesDirectory(GamePath);
		std::error_code error;
		if (!std::filesystem::exists(directory, error)) {
			if (error) {
				SavedChangesError = "could not inspect " + directory.string() + ": " + error.message();
			}
			return;
		}

		std::vector<std::filesystem::path> files;
		std::filesystem::directory_iterator entries(directory, error);
		if (error) {
			SavedChangesError = "could not list " + directory.string() + ": " + error.message();
			return;
		}
		for (const std::filesystem::directory_entry &entry : entries) {
			if (!entry.is_regular_file(error)) {
				error.clear();
				continue;
			}
			const std::string filename = entry.path().filename().string();
			if (filename.ends_with("-changes.xml")) {
				files.push_back(entry.path());
			}
		}
		std::sort(files.begin(), files.end(), std::greater<>());

		size_t skipped = 0;
		std::string firstError;
		for (const std::filesystem::path &path : files) {
			std::string text;
			std::string readError;
			SavedChange change;
			if (!ReadText(path, text, readError) || !ParseSavedChangeXml(text, change, &readError)) {
				skipped++;
				if (firstError.empty()) {
					firstError = path.filename().string() + ": " + readError;
				}
				continue;
			}
			change.Source = path;
			SavedChanges.push_back(std::move(change));
		}

		if (skipped != 0) {
			SavedChangesError = std::to_string(skipped) + " change record(s) skipped; first: " + firstError;
		}
	}

	bool Editor::RecordSavedChange(
		const std::filesystem::path &game,
		const std::string_view before,
		const std::string_view after,
		std::string &error
	) {
		error.clear();
		if (before == after) {
			return true;
		}

		const SaveTimestamp timestamp = NowTimestamp();
		const std::filesystem::path directory = SavedChangesDirectory(game);
		std::error_code filesystemError;
		std::filesystem::create_directories(directory, filesystemError);
		if (filesystemError) {
			error = "could not create " + directory.string() + ": " + filesystemError.message();
			return false;
		}

		std::filesystem::path target;
		for (size_t suffix = 0;; suffix++) {
			const std::string extra = suffix == 0 ? std::string() : "-" + std::to_string(suffix);
			target = directory / (timestamp.Filename + extra + "-changes.xml");
			if (!std::filesystem::exists(target, filesystemError)) {
				break;
			}
			if (filesystemError) {
				error = "could not inspect " + target.string() + ": " + filesystemError.message();
				return false;
			}
		}

		SavedChange change;
		change.SavedAt = timestamp.Text;
		change.Before = before;
		change.After = after;
		const std::string xml = SavedChangeXml(change);
		if (xml.size() > SAVED_CHANGE_MAX_BYTES) {
			error = "change record exceeds 128 MiB";
			return false;
		}
		const std::filesystem::path temporary = target.string() + ".tmp";

		{
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			if (!out) {
				error = "could not write " + temporary.string();
				return false;
			}
			out.write(xml.data(), static_cast<std::streamsize>(xml.size()));
			out.flush();
			if (!out) {
				error = "could not finish writing " + temporary.string();
				return false;
			}
		}

		std::filesystem::rename(temporary, target, filesystemError);
		if (filesystemError) {
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			error = "could not publish " + target.string() + ": " + filesystemError.message();
			return false;
		}

		SavedChangesFor.clear();
		return true;
	}
}
