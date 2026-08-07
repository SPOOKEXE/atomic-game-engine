#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/core/Log.hpp>

#include <algorithm>
#include <cdn/LocalStore.hpp>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace cdn {

	namespace {
		// Where a person's home is, or the current directory.
		//
		// **The environment and not a platform API**, which is the portable half
		// of the two and the one a test can override. `HOME` on Unix, `USERPROFILE`
		// on Windows; a process with neither — a container, a service — gets the
		// current directory rather than `/`, so the store lands somewhere writable
		// instead of failing at the first import.
		std::filesystem::path HomeDirectory() {
			if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0') {
				return std::filesystem::path(home);
			}
			if (const char *profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
				return std::filesystem::path(profile);
			}
			return std::filesystem::current_path();
		}

		// One log line's separator.
		//
		// **A tab, so a name with a space in it survives.** Content filenames have
		// spaces constantly — the roadmap's own seed list has several — and a
		// space-separated log would split one of those into two fields the first
		// time anybody read it back.
		constexpr char FIELD = '\t';
	}

	LocalPaths DefaultLocalPaths() {
		return LocalPathsUnder(HomeDirectory() / "Documents" / "atomic-game-engine" / "cdn");
	}

	LocalPaths LocalPathsUnder(const std::filesystem::path &root) {
		LocalPaths paths;
		paths.Root = root;
		paths.Raw = root / "raw";
		paths.Processed = root / "processed";
		paths.Log = root / "content.log";
		return paths;
	}

	bool EnsureLocalStore(const LocalPaths &paths) {
		std::error_code failure;

		// `create_directories` is already idempotent and reports "existed" as
		// `false` with no error, which is why the error code rather than the
		// return value is what decides.
		std::filesystem::create_directories(paths.Raw, failure);
		if (failure) {
			ENGINE_ERROR("content store: could not create {}: {}", paths.Raw.string(), failure.message());
			return false;
		}

		std::filesystem::create_directories(paths.Processed, failure);
		if (failure) {
			ENGINE_ERROR(
				"content store: could not create {}: {}", paths.Processed.string(), failure.message()
			);
			return false;
		}

		return true;
	}

	bool AppendLog(const LocalPaths &paths, const LogEntry &entry) {
		std::ofstream file(paths.Log, std::ios::app);
		if (!file) {
			return false;
		}

		// **One line, tab-separated, no header.** A format a person can read in a
		// terminal and `cut` at, which is the whole point of it being a text log
		// rather than a document — see the header on why nothing parses it to make
		// a decision.
		file << entry.Seconds << FIELD << entry.Action << FIELD << entry.Subject << FIELD << entry.Hash
			 << FIELD << entry.Bytes << '\n';
		return file.good();
	}

	std::vector<LogEntry> ReadLog(const LocalPaths &paths) {
		std::vector<LogEntry> entries;

		std::ifstream file(paths.Log);
		if (!file) {
			return entries;
		}

		std::string line;
		while (std::getline(file, line)) {
			std::istringstream fields(line);
			LogEntry entry;

			std::string seconds;
			std::string bytes;
			if (!std::getline(fields, seconds, FIELD) || !std::getline(fields, entry.Action, FIELD) ||
				!std::getline(fields, entry.Subject, FIELD) || !std::getline(fields, entry.Hash, FIELD) ||
				!std::getline(fields, bytes)) {
				// **A short line is skipped rather than refused.** A log is
				// appended to by several processes and the last line of one
				// truncated by a crash is an ordinary thing to find; refusing the
				// whole file over it would lose every line before it.
				continue;
			}

			// The numbers are parsed defensively for the same reason. A line
			// whose timestamp is not a number is a line something else wrote.
			entry.Seconds = std::strtoull(seconds.c_str(), nullptr, 10);
			entry.Bytes = std::strtoull(bytes.c_str(), nullptr, 10);
			entries.push_back(std::move(entry));
		}

		return entries;
	}

	std::optional<ImportReport>
	ImportFile(const LocalPaths &paths, const std::filesystem::path &source, uint64_t seconds) {
		if (!EnsureLocalStore(paths)) {
			return std::nullopt;
		}

		std::ifstream file(source, std::ios::binary);
		if (!file) {
			ENGINE_ERROR("content store: could not read {}", source.string());
			return std::nullopt;
		}

		// **Read whole rather than streamed into the hasher.** `assets::Hasher`
		// has a streaming form and this does not use it, which is the opposite of
		// what the chunker does — and it is right here for a reason the chunker
		// does not have: the bytes have to be *written out* as well as hashed, and
		// the name to write them under is not known until the hash is finished. A
		// streaming version would have to read the file twice.
		//
		// The ceiling is whatever a person drags in. A model or a texture is
		// megabytes; the roadmap's own seed list tops out at an hour of audio,
		// which is about sixty. That is a buffer, not a problem.
		const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};

		ImportReport report;
		report.Bytes = bytes.size();
		report.Hash = engine::assets::Hasher::Of(
						  std::span(reinterpret_cast<const std::byte *>(bytes.data()), bytes.size())
		)
						  .ToHex();

		// `<hash><extension>`. The extension is kept so the folder stays readable
		// and a publisher can tell what it is looking at without opening it; the
		// header carries why the original *name* is not kept on disk.
		report.Stored = paths.Raw / (report.Hash + source.extension().string());

		std::error_code failure;
		if (std::filesystem::exists(report.Stored, failure)) {
			// **Already there, and that is a success rather than an error.**
			// Re-importing is what a person does, and the bytes are the identity —
			// so the right answer is "it is already there" and not a second copy
			// under a suffixed name.
			report.Duplicate = true;
		} else {
			std::ofstream out(report.Stored, std::ios::binary);
			if (!out) {
				ENGINE_ERROR("content store: could not write {}", report.Stored.string());
				return std::nullopt;
			}
			out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			if (!out.good()) {
				return std::nullopt;
			}
		}

		LogEntry entry;
		entry.Seconds = seconds;
		entry.Action = report.Duplicate ? "import-duplicate" : "import";

		// **The original path, which is the whole reason the log exists.** The
		// folder answers "what is here"; only this answers "where did it come
		// from", and a hash-named flat folder cannot answer it at all.
		entry.Subject = source.string();
		entry.Hash = report.Hash;
		entry.Bytes = report.Bytes;
		(void)AppendLog(paths, entry);

		return report;
	}

	std::optional<PublishReport> PublishLocal(
		const LocalPaths &paths,
		const engine::assets::SigningKey &signing,
		uint64_t seconds,
		const PublishSettings &settings
	) {
		if (!EnsureLocalStore(paths)) {
			return std::nullopt;
		}

		const std::optional<PublishReport> report = Publish(paths.Raw, paths.Processed, signing, settings);
		if (!report.has_value()) {
			return std::nullopt;
		}

		LogEntry entry;
		entry.Seconds = seconds;
		entry.Action = "publish";
		entry.Subject = std::to_string(report->Assets) + " asset(s)";
		entry.Hash = report->Root.ToHex();
		entry.Bytes = report->StoredBytes;
		(void)AppendLog(paths, entry);

		return report;
	}

	std::vector<RawEntry> RawContents(const LocalPaths &paths) {
		std::vector<RawEntry> entries;

		std::error_code failure;
		if (!std::filesystem::is_directory(paths.Raw, failure)) {
			return entries;
		}

		// **The log read once into a map, not once per file.** A store with the
		// roadmap's own seed import in it is 289 files and the log is at least
		// that many lines; a lookup per file would be a quadratic walk every
		// time somebody opened the assets panel.
		std::unordered_map<std::string, std::string> named;
		for (const LogEntry &entry : ReadLog(paths)) {
			if (!entry.Hash.empty() && !entry.Subject.empty()) {
				// Last writer wins, which is right: a file re-imported from a
				// new place is most usefully labelled with the new one.
				named[entry.Hash] = entry.Subject;
			}
		}

		for (const auto &file : std::filesystem::directory_iterator(paths.Raw, failure)) {
			if (failure) {
				break;
			}
			if (!file.is_regular_file(failure)) {
				continue;
			}

			RawEntry entry;
			entry.Path = file.path();
			entry.Bytes = static_cast<uint64_t>(file.file_size(failure));

			// `<hash><extension>`, so the stem is the hash.
			const auto found = named.find(file.path().stem().string());
			entry.Original = found == named.end() ? file.path().filename().string()
												  : std::filesystem::path(found->second).filename().string();

			entries.push_back(std::move(entry));
		}

		// **Newest first, by the clock on the file.** Somebody looking at this
		// has just added something, and the thing they added should not be
		// wherever the hash happens to sort.
		std::sort(entries.begin(), entries.end(), [&](const RawEntry &left, const RawEntry &right) {
			std::error_code ignored;
			const auto leftTime = std::filesystem::last_write_time(left.Path, ignored);
			const auto rightTime = std::filesystem::last_write_time(right.Path, ignored);
			if (leftTime != rightTime) {
				return leftTime > rightTime;
			}
			// A tiebreak that does not depend on the filesystem's timestamp
			// resolution, which on some of them is a whole second — so a bulk
			// import would otherwise come back in an arbitrary order.
			return left.Original < right.Original;
		});

		return entries;
	}

	std::vector<PublishedEntry> PublishedContents(const LocalPaths &paths) {
		std::vector<PublishedEntry> entries;

		std::optional<engine::assets::ChunkStore> store =
			engine::assets::ChunkStore::Open(paths.Processed, false);
		if (!store) {
			return entries;
		}

		engine::assets::SignatureBytes signature;
		const std::optional<engine::assets::Manifest> manifest = store->ReadManifest(signature);
		if (!manifest) {
			return entries;
		}

		entries.reserve(manifest->Assets().size());
		for (const engine::assets::AssetEntry &asset : manifest->Assets()) {
			entries.push_back(PublishedEntry{.Name = asset.Name, .Kind = asset.Kind, .Root = asset.Root});
		}

		// Name order, which is what a picker wants and what a manifest already
		// is — sorted here anyway rather than relying on it, because "the
		// manifest happens to be sorted" is a property of the publisher rather
		// than of the format.
		std::sort(
			entries.begin(), entries.end(), [](const PublishedEntry &left, const PublishedEntry &right) {
				return left.Name < right.Name;
			}
		);

		return entries;
	}
}
