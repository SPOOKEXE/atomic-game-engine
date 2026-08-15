#include <studio/Browse.hpp>

#include <algorithm>
#include <cctype>
#include <system_error>

namespace studio {

	namespace {
		// Lowercases ASCII only, which is what a file suffix is.
		std::string Lowered(std::string_view text) {
			std::string out(text);
			std::transform(out.begin(), out.end(), out.begin(), [](unsigned char letter) {
				return static_cast<char>(std::tolower(letter));
			});
			return out;
		}
	}

	bool MatchesExtension(std::string_view name, const std::vector<std::string> &extensions) {
		if (extensions.empty()) {
			return true;
		}

		const std::string lowered = Lowered(name);

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

	Listing BrowseDirectory(
		const std::filesystem::path &directory, const std::vector<std::string> &extensions
	) {
		Listing listing;

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

		if (const std::filesystem::path parent = where.parent_path();
			!parent.empty() && parent != where) {
			listing.Parent = parent;
		}

		std::filesystem::directory_iterator walk(where, std::filesystem::directory_options::skip_permission_denied, code);
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

			listing.Entries.push_back(BrowseEntry{name, entry.path(), directoryEntry});
		}

		// Directories first, then by name. Case-insensitive, because a listing
		// that puts `Zebra` before `apple` is a listing sorted by an
		// implementation detail.
		std::sort(
			listing.Entries.begin(),
			listing.Entries.end(),
			[](const BrowseEntry &left, const BrowseEntry &right) {
				if (left.Directory != right.Directory) {
					return left.Directory;
				}
				return Lowered(left.Name) < Lowered(right.Name);
			}
		);

		return listing;
	}
}
