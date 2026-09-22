#include <engine/ui/Browse.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

namespace engine::ui {

	namespace {
		// File suffixes and sort keys are ASCII. Keeping this explicit avoids a
		// locale-dependent directory order.
		std::string LoweredAscii(std::string_view text) {
			std::string out(text);
			for (char &letter : out) {
				if (letter >= 'A' && letter <= 'Z') {
					letter = static_cast<char>(letter - 'A' + 'a');
				}
			}
			return out;
		}

		// The key is kept beside the entry only while building a listing. A
		// comparator can then compare strings without remaking either key.
		struct SortableEntry {
			BrowseEntry Entry;
			std::string SortKey;
		};
	}

	bool MatchesExtension(std::string_view name, const std::vector<std::string> &extensions) {
		if (extensions.empty()) {
			return true;
		}

		const std::string lowered = LoweredAscii(name);

		for (const std::string &suffix : extensions) {
			if (suffix.empty() || lowered.size() < suffix.size()) {
				continue;
			}
			if (lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) == 0) {
				return true;
			}
		}

		return false;
	}

	Listing
	BrowseDirectory(const std::filesystem::path &directory, const std::vector<std::string> &extensions) {
		Listing listing;
		std::vector<SortableEntry> entries;

		std::error_code code;

		// **Every filesystem call takes an `error_code`.** A browser is pointed
		// at whatever somebody types, which includes paths that do not exist,
		// paths on a device that has gone away and paths they may not read - and
		// the throwing overloads turn each of those into an exception through a
		// draw call.
		std::filesystem::path where = directory;

		if (where.empty()) {
			where = std::filesystem::current_path(code);
			if (code) {
				listing.Error = "cannot tell where we are: " + code.message();
				return listing;
			}
		}

		// A path that names a file lists the folder it is in. See the
		// declaration: opening the dialog on a game's own path should show that
		// game's folder rather than refuse.
		if (!std::filesystem::is_directory(where, code)) {
			if (where.has_parent_path()) {
				where = where.parent_path();
			}
		}

		where = std::filesystem::weakly_canonical(where, code);
		if (code) {
			// Not fatal - an uncanonicalisable path is still listable, and
			// refusing here would make a symlinked folder unbrowsable.
			code.clear();
			where = directory;
		}

		listing.Directory = where;

		if (const std::filesystem::path parent = where.parent_path(); !parent.empty() && parent != where) {
			listing.Parent = parent;
		}

		std::filesystem::directory_iterator walk(
			where, std::filesystem::directory_options::skip_permission_denied, code
		);
		if (code) {
			listing.Error = "cannot read this folder: " + code.message();
			return listing;
		}

		for (const std::filesystem::directory_entry &entry : walk) {
			std::error_code each;

			const std::string name = entry.path().filename().string();
			if (name.empty() || name.front() == '.') {
				continue;
			}

			const bool directoryEntry = entry.is_directory(each);
			if (each) {
				// Skipped rather than fatal. See the declaration.
				continue;
			}

			if (!directoryEntry && !MatchesExtension(name, extensions)) {
				continue;
			}

			entries.push_back(
				SortableEntry{
					.Entry = BrowseEntry{name, entry.path(), directoryEntry},
					.SortKey = LoweredAscii(name),
				}
			);
		}

		// Directories first, then by one lowercase key per row. The displayed name
		// resolves a case-only tie so every filesystem yields the same listing.
		std::sort(entries.begin(), entries.end(), [](const SortableEntry &left, const SortableEntry &right) {
			if (left.Entry.Directory != right.Entry.Directory) {
				return left.Entry.Directory;
			}
			if (left.SortKey != right.SortKey) {
				return left.SortKey < right.SortKey;
			}
			return left.Entry.Name < right.Entry.Name;
		});

		listing.Entries.reserve(entries.size());
		for (SortableEntry &entry : entries) {
			listing.Entries.push_back(std::move(entry.Entry));
		}

		return listing;
	}
}
