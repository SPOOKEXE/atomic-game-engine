#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Manifest.hpp>
#include <engine/core/Log.hpp>

#include <algorithm>
#include <array>
#include <cdn/LocalStore.hpp>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
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
		// on Windows; a process with neither - a container, a service - gets the
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
		// spaces constantly - the roadmap's own seed list has several - and a
		// space-separated log would split one of those into two fields the first
		// time anybody read it back.
		constexpr char FIELD = '\t';
	}

	namespace {
		// Whether a directory holds no regular file, at any depth.
		//
		// **Missing counts as empty**, because the caller's question is "is there
		// anything to publish" and a store that has never been baked has no
		// folder rather than an empty one.
		bool IsEmptyDirectory(const std::filesystem::path &path) {
			std::error_code failure;
			for (const auto &entry : std::filesystem::recursive_directory_iterator(path, failure)) {
				if (failure) {
					break;
				}
				if (entry.is_regular_file(failure)) {
					return false;
				}
			}
			return true;
		}
	}

	LocalPaths DefaultLocalPaths() {
		return LocalPathsUnder(HomeDirectory() / "Documents" / "atomic-game-engine" / "cdn");
	}

	LocalPaths LocalPathsUnder(const std::filesystem::path &root) {
		LocalPaths paths;
		paths.Root = root;
		paths.Raw = root / "raw";
		paths.Baked = root / "baked";
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

		std::filesystem::create_directories(paths.Baked, failure);
		if (failure) {
			ENGINE_ERROR("content store: could not create {}: {}", paths.Baked.string(), failure.message());
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
		// rather than a document - see the header on why nothing parses it to make
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
		// what the chunker does - and it is right here for a reason the chunker
		// does not have: the bytes have to be *written out* as well as hashed, and
		// the name to write them under is not known until the hash is finished. A
		// streaming version would have to read the file twice.
		//
		// The ceiling is whatever a person drags in. A model or a texture is
		// megabytes; the roadmap's own seed list tops out at an hour of audio,
		// which is about sixty. That is a buffer, not a problem.
		const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};

		// **An empty file is refused here rather than three stages later.**
		// `Publish` already skips one - "cdn: skipped {} - empty" - but by then
		// it is in `raw/` for good, hash-named `af1349b9…` after BLAKE3's
		// empty-input digest, and it bakes to nothing. The result is a store
		// whose raw and baked counts differ by one for ever, with no line
		// anywhere naming the file: the only way to find it is to subtract two
		// numbers printed by two different tools and then diff the two folders
		// by hash. `DEFERRED.md` D00034 is that hunt written down.
		//
		// **Named, because a count cannot be subtracted from a count to get a
		// name.** A folder import sweeps whatever is in the folder - the one
		// this was found by was a `.lock` inside a Python virtualenv that came
		// along with a model - so the message has to say which file, or it is
		// the same silence one stage earlier.
		if (bytes.empty()) {
			ENGINE_WARN(
				"content store: refused {} - empty, and an empty file can never bake or publish",
				source.string()
			);
			return std::nullopt;
		}

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
			// Re-importing is what a person does, and the bytes are the identity -
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

		// **Refused rather than published as nothing.** This is exactly the
		// state a store is in the first time it is published after `baked/`
		// existed, and an empty manifest written over a working one reads as a
		// store somebody emptied. Whatever fills `baked/` has not run.
		if (IsEmptyDirectory(paths.Baked) && !IsEmptyDirectory(paths.Raw)) {
			ENGINE_ERROR(
				"content store: {} is empty and {} is not", paths.Baked.string(), paths.Raw.string()
			);
			ENGINE_ERROR("bake before publishing - `contentimport --publish` and the studio both do");
			return std::nullopt;
		}

		const std::optional<PublishReport> report = Publish(paths.Baked, paths.Processed, signing, settings);
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

	std::filesystem::path FindInStore(const LocalPaths &paths, std::string_view name) {
		if (name.empty()) {
			return {};
		}

		std::error_code failure;
		for (const std::filesystem::path &folder : {paths.Baked, paths.Raw}) {
			const std::filesystem::path candidate = folder / name;
			if (std::filesystem::is_regular_file(candidate, failure)) {
				return candidate;
			}
		}
		return {};
	}

	engine::assets::SigningKey DevelopmentSigningKey() {
		// **Spelled out rather than derived, so it is greppable and stable.**
		// A seed computed from a string would be a second thing to reimplement
		// the day another tool needs the same identity, and this is a constant
		// whose whole value is that everything agrees on it. `LocalStore.hpp`
		// carries what it is and is not for.
		constexpr std::array<uint8_t, engine::assets::SigningKey::SEED_BYTES> SEED{
			0xa7, 0x0e, 0x1c, 0x9d, 0x54, 0x33, 0x8b, 0x62, 0xf1, 0x40, 0x2a, 0xd6, 0x77, 0x18, 0xbc, 0x05,
			0x93, 0xee, 0x6f, 0x21, 0x4c, 0xb8, 0x0d, 0x7a, 0x35, 0xc2, 0x99, 0x86, 0x50, 0xfb, 0x13, 0x48,
		};

		std::array<std::byte, engine::assets::SigningKey::SEED_BYTES> seed{};
		for (size_t index = 0; index < seed.size(); index++) {
			seed[index] = static_cast<std::byte>(SEED[index]);
		}

		std::optional<engine::assets::SigningKey> key = engine::assets::SigningKey::FromSeed(seed);

		// `FromSeed` refuses only a wrong length, and the length is a
		// `static_assert` away from being a compile error.
		return std::move(*key);
	}

	engine::assets::PublicKey DevelopmentPublisher() {
		return DevelopmentSigningKey().Public();
	}

	std::function<bool(std::string_view model, std::string_view reference, std::string &out)>
	StoreTextureResolver(const LocalPaths &paths) {
		// Both directions of the log, taken once. **A snapshot rather than a live
		// read**, because this is called per submesh of every model in a bake and
		// re-reading a file with several thousand lines each time would be
		// quadratic in the store.
		auto originals = std::make_shared<std::unordered_map<std::string, std::string>>();
		auto hashes = std::make_shared<std::unordered_map<std::string, std::string>>();

		for (const LogEntry &entry : ReadLog(paths)) {
			if (entry.Hash.empty() || entry.Subject.empty()) {
				continue;
			}
			// Last writer wins in both maps, matching `RawContents`: a file
			// re-imported from a new place is most usefully known by the new one.
			(*originals)[entry.Hash] = entry.Subject;
			(*hashes)[entry.Subject] = entry.Hash;
		}

		// The extension a store file carries, so the answer names a real file.
		// `raw/` holds `<hash><extension>` and the log records neither half
		// separately, so the extension comes off the original path.
		return [originals, hashes](std::string_view model, std::string_view reference, std::string &out) {
			// The model's own original path, from its hash. A model that is not
			// in the log - dropped into `raw/` by hand - cannot be placed, and
			// that is a `false` rather than a guess.
			const std::filesystem::path modelPath(model);
			const auto origin = originals->find(modelPath.stem().string());
			if (origin == originals->end()) {
				return false;
			}

			// Where the reference points, in the tree the model was authored in.
			// **`lexically_normal`, so `../shared/skin.png` is a path the log can
			// match** - a model referring up out of its own folder is ordinary,
			// and a literal join would produce a string no import ever wrote.
			const std::filesystem::path wanted =
				(std::filesystem::path(origin->second).parent_path() / std::filesystem::path(reference))
					.lexically_normal();

			const auto found = hashes->find(wanted.generic_string());
			if (found == hashes->end()) {
				return false;
			}

			out = found->second + wanted.extension().string();
			return true;
		};
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

		// **Recursive, because `cdn::Publish` is.** The publisher names an asset
		// by its path relative to `raw/`, so a tree under there has always been
		// publishable - and v0.10's material import is the first thing that
		// writes one, because a material has to *name* its texture and a
		// hash-renamed flat import gives it no name to write. A non-recursive
		// listing showed a store of six thousand files as empty, which reads as a
		// broken panel rather than as a listing that stops at the top level.
		for (const auto &file : std::filesystem::recursive_directory_iterator(paths.Raw, failure)) {
			if (failure) {
				break;
			}
			if (!file.is_regular_file(failure)) {
				continue;
			}

			RawEntry entry;
			entry.Path = file.path();
			entry.Bytes = static_cast<uint64_t>(file.file_size(failure));

			// `<hash><extension>` for an import, so the stem is the hash. A file
			// written into a subdirectory by a tool has no log line and is
			// labelled by its path relative to `raw/` - which is also the name it
			// will be published under, and is the thing somebody is looking for.
			const auto found = named.find(file.path().stem().string());
			if (found != named.end()) {
				entry.Original = std::filesystem::path(found->second).filename().string();
			} else {
				std::error_code ignored;
				const std::filesystem::path relative =
					std::filesystem::relative(file.path(), paths.Raw, ignored);
				entry.Original = ignored ? file.path().filename().string() : relative.generic_string();
			}

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
			// resolution, which on some of them is a whole second - so a bulk
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
		// is - sorted here anyway rather than relying on it, because "the
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
