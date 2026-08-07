// The assets manager: what is in the local content store, and how to add to it.
//
// **A view over `cdn::LocalStore` and nothing of its own.** The folder is the
// index — `LocalStore.hpp` says so — so this panel reads the log to say where
// things came from, reads `raw/` to say what is actually there, and calls
// `ImportFile` and `PublishLocal`. It keeps no list, caches nothing between
// frames that it cannot rebuild, and has no opinion the store does not already
// have.
//
// **The upload "button" takes a typed path rather than opening a file dialog**,
// and that is a real limitation rather than a preference. There is no portable
// file picker in this stack: SDL3 has `SDL_ShowOpenFileDialog`, which is
// asynchronous and platform-backed, and wiring it means a callback crossing back
// into the frame loop and a dependency the studio does not have today. A text
// field plus drag-and-drop — which SDL *does* deliver as an event — covers the
// same ground, and the field is what a person uses when they have a path in the
// clipboard, which for content coming out of another tool is most of the time.

#include <cdn/LocalStore.hpp>
#include <engine/core/Log.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	namespace {
		// Seconds since the Unix epoch, for the log.
		//
		// Read here and passed down, because `cdn::LocalStore` holds no notion of
		// "now" of its own to drift — `assets::Grant`'s standing rule.
		uint64_t NowSeconds() {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
											 std::chrono::system_clock::now().time_since_epoch()
			)
											 .count());
		}

		// A byte count somebody can read at a glance.
		std::string Readable(uint64_t bytes) {
			static const char *UNITS[] = {"B", "KB", "MB", "GB"};

			double scaled = static_cast<double>(bytes);
			size_t unit = 0;
			while (scaled >= 1024.0 && unit + 1 < std::size(UNITS)) {
				scaled /= 1024.0;
				unit++;
			}

			char text[32];
			std::snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.1f %s", scaled, UNITS[unit]);
			return text;
		}
	}

	void Editor::DrawAssets() {
		if (!ShowAssets) {
			return;
		}

		if (!ImGui::Begin("Assets", &ShowAssets)) {
			ImGui::End();
			return;
		}

		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();

		ImGui::TextUnformatted(paths.Root.string().c_str());
		ImGui::Separator();

		// --- adding -----------------------------------------------------------

		ImGui::TextUnformatted("Add content");

		// **Wide enough for a real path**, because the thing a person pastes here
		// is forty characters more often than ten and a field they have to scroll
		// inside is one they cannot check before pressing the button.
		ImGui::SetNextItemWidth(-120.0f);
		ImGui::InputTextWithHint(
			"##import", "a file or a folder to import", AssetImportPath, sizeof(AssetImportPath)
		);

		ImGui::SameLine();

		// **Disabled rather than hidden when the field is empty**, so the button
		// is in the same place whether or not it can be pressed — a control that
		// moves is one somebody clicks by accident.
		const bool hasPath = AssetImportPath[0] != '\0';
		ImGui::BeginDisabled(!hasPath);
		if (ImGui::Button("Import", ImVec2(110.0f, 0.0f))) {
			ImportAssetPath(AssetImportPath);
		}
		ImGui::EndDisabled();

		if (!AssetStatus.empty()) {
			ImGui::TextUnformatted(AssetStatus.c_str());
		}

		ImGui::Separator();

		// --- publishing -------------------------------------------------------
		//
		// **The key is asked for and never stored**, which is `PublishLocal`'s own
		// rule surfacing: a manifest signs what a client will trust, and a key kept
		// in the editor's preferences file is a key that signs anything anybody
		// drops in the folder.
		ImGui::TextUnformatted("Publish raw/ into processed/");

		ImGui::SetNextItemWidth(-120.0f);
		ImGui::InputTextWithHint(
			"##key",
			"64 hex characters of signing seed",
			AssetSigningKey,
			sizeof(AssetSigningKey),
			ImGuiInputTextFlags_Password
		);

		ImGui::SameLine();
		ImGui::BeginDisabled(AssetSigningKey[0] == '\0');
		if (ImGui::Button("Publish", ImVec2(110.0f, 0.0f))) {
			PublishAssets(AssetSigningKey);
		}
		ImGui::EndDisabled();

		ImGui::Separator();

		// --- what is there ----------------------------------------------------
		//
		// **The log rather than a directory walk**, because the log is the only
		// thing that knows where a file came from — `raw/` holds hashes. Newest
		// first, which is the order somebody looking for what they just added
		// wants.
		const std::vector<cdn::LogEntry> entries = cdn::ReadLog(paths);

		ImGui::Text("%zu entr%s", entries.size(), entries.size() == 1 ? "y" : "ies");

		if (ImGui::BeginTable(
				"assets", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
			)) {
			ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthFixed, 110.0f);
			ImGui::TableSetupColumn("Came from");
			ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
			ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 90.0f);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();

			// **A clipper, because the roadmap's own seed import is 289 rows** and
			// a store somebody has used for a while is thousands. Drawing every
			// row of a scrolled table is the cost imgui's clipper exists to
			// remove, and at this count it is the difference between a panel that
			// opens and one that drops the frame rate while it is open.
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(entries.size()));

			while (clipper.Step()) {
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
					// Newest first.
					const cdn::LogEntry &entry = entries[entries.size() - 1 - static_cast<size_t>(row)];

					ImGui::TableNextRow();

					ImGui::TableNextColumn();
					ImGui::TextUnformatted(entry.Action.c_str());

					ImGui::TableNextColumn();
					ImGui::TextUnformatted(entry.Subject.c_str());

					// The whole path when it is elided, because that is the field
					// somebody is squinting at.
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("%s", entry.Subject.c_str());
					}

					ImGui::TableNextColumn();
					ImGui::TextUnformatted(Readable(entry.Bytes).c_str());

					ImGui::TableNextColumn();

					// The first eight characters, which is enough to recognise one
					// and short enough to fit. The whole hash is a click away.
					ImGui::TextUnformatted(entry.Hash.substr(0, 8).c_str());
					if (ImGui::IsItemHovered() && !entry.Hash.empty()) {
						ImGui::SetTooltip("%s", entry.Hash.c_str());
					}
					if (ImGui::IsItemClicked() && !entry.Hash.empty()) {
						ImGui::SetClipboardText(entry.Hash.c_str());
					}
				}
			}

			ImGui::EndTable();
		}

		ImGui::End();
	}

	void Editor::ImportAssetPath(const std::string &given) {
		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();
		if (!cdn::EnsureLocalStore(paths)) {
			AssetStatus = "could not create the content store";
			return;
		}

		const std::filesystem::path source(given);
		std::error_code failure;

		size_t imported = 0;
		size_t duplicates = 0;
		size_t failures = 0;
		const uint64_t now = NowSeconds();

		const auto take = [&](const std::filesystem::path &file) {
			const auto report = cdn::ImportFile(paths, file, now);
			if (!report.has_value()) {
				failures++;
			} else if (report->Duplicate) {
				duplicates++;
			} else {
				imported++;
			}
		};

		if (std::filesystem::is_directory(source, failure)) {
			// **A folder is walked**, because that is what a person drags. Skipping
			// what cannot be read rather than stopping, for `contentimport`'s
			// reason: a real content directory has a broken symlink in it
			// eventually.
			for (std::filesystem::recursive_directory_iterator walk(
					 source, std::filesystem::directory_options::skip_permission_denied, failure
				 );
				 walk != std::filesystem::recursive_directory_iterator();
				 walk.increment(failure)) {
				if (failure) {
					break;
				}
				if (walk->is_regular_file(failure)) {
					take(walk->path());
				}
			}
		} else if (std::filesystem::is_regular_file(source, failure)) {
			take(source);
		} else {
			AssetStatus = "not a file or a folder: " + given;
			return;
		}

		AssetStatus = std::to_string(imported) + " imported, " + std::to_string(duplicates) +
					  " already there, " + std::to_string(failures) + " failed";
		ENGINE_INFO("assets: {}", AssetStatus);

		// **Cleared on success so the next import starts empty**, and kept on
		// failure so somebody can fix a typo rather than retype the path.
		if (failures == 0) {
			AssetImportPath[0] = '\0';
		}
	}

	void Editor::PublishAssets(const std::string &hexSeed) {
		if (hexSeed.size() != 64) {
			AssetStatus = "the signing key is 64 hex characters";
			return;
		}

		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); index++) {
			const std::string byte = hexSeed.substr(index * 2, 2);
			char *end = nullptr;
			const long value = std::strtol(byte.c_str(), &end, 16);
			if (end != byte.c_str() + 2) {
				AssetStatus = "the signing key is not hex";
				return;
			}
			seed[index] = static_cast<std::byte>(value);
		}

		const auto signing = engine::assets::SigningKey::FromSeed(seed);
		if (!signing.has_value()) {
			AssetStatus = "that is not a usable signing key";
			return;
		}

		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();
		const auto report = cdn::PublishLocal(paths, *signing, NowSeconds());
		if (!report.has_value()) {
			AssetStatus = "the publish failed — see the output panel";
			return;
		}

		AssetStatus = std::to_string(report->Assets) + " asset(s) in " +
					  std::to_string(report->Bundles) + " bundle(s), root " +
					  report->Root.ToHex().substr(0, 8);
		ENGINE_INFO("assets: {}", AssetStatus);
	}
}
