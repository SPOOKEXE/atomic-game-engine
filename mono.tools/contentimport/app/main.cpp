// Brings files and folders into the local content store.
//
// **The repeatable form of "put this in the cdn".** `ROADMAP.md` v0.10 asks for
// a named list of files to be added to the store, and a shell loop that did it
// once would leave nothing anybody could run again — on another machine, or after
// the store was cleared. This is that loop with a name.
//
//     contentimport ~/Music/album ~/art/fox.png
//     contentimport --root /tmp/store --publish --key HEX ~/art
//
// **Directories are walked and files are taken as they are.** A person dragging
// content in thinks in folders, and requiring them to expand one is asking them
// to do what a program is for.
//
// @tier shared

#include <engine/assets/Signature.hpp>
#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>

#include <array>
#include <cdn/LocalStore.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {
	// Seconds since the Unix epoch.
	//
	// **Read here and passed down**, because `cdn::LocalStore` holds no notion of
	// "now" — `assets::Grant`'s standing rule, and what lets a suite pin a
	// timestamp.
	uint64_t NowSeconds() {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
										 std::chrono::system_clock::now().time_since_epoch()
		)
										 .count());
	}

	// A signing seed from sixty-four hex characters.
	//
	// **Spelled here and again in `mono.cdn/app/main.cpp`**, which is two copies
	// of one parser and is worth naming rather than leaving to be noticed. The
	// alternative is exporting it from `assets`, and the reason not to is that
	// `SigningKey::FromSeed` takes bytes on purpose: a *hex* constructor invites
	// a key in a config file, and this engine wants one on a command line or in
	// an environment variable. Two eighteen-line copies in two tools is the
	// cheaper of the two mistakes.
	std::optional<engine::assets::SigningKey> SeedFromHex(std::string_view text) {
		const std::string hex(text);
		if (hex.size() != 64) {
			return std::nullopt;
		}

		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); index++) {
			const std::string byte = hex.substr(index * 2, 2);
			char *end = nullptr;
			const long value = std::strtol(byte.c_str(), &end, 16);
			if (end != byte.c_str() + 2) {
				return std::nullopt;
			}
			seed[index] = static_cast<std::byte>(value);
		}
		return engine::assets::SigningKey::FromSeed(seed);
	}

	// Every file under a path, or the path itself when it is a file.
	//
	// **Skips anything unreadable rather than stopping.** A folder somebody points
	// at may hold a socket, a broken symlink or a file they cannot read, and
	// refusing the whole import over one of those would make the tool useless on
	// exactly the messy directories it exists for.
	std::vector<std::filesystem::path> Expand(const std::filesystem::path &root) {
		std::vector<std::filesystem::path> files;
		std::error_code failure;

		if (std::filesystem::is_regular_file(root, failure)) {
			files.push_back(root);
			return files;
		}

		if (!std::filesystem::is_directory(root, failure)) {
			ENGINE_ERROR("not a file or a directory: {}", root.string());
			return files;
		}

		for (std::filesystem::recursive_directory_iterator walk(
				 root, std::filesystem::directory_options::skip_permission_denied, failure
			 );
			 walk != std::filesystem::recursive_directory_iterator();
			 walk.increment(failure)) {
			if (failure) {
				break;
			}
			if (walk->is_regular_file(failure)) {
				files.push_back(walk->path());
			}
		}

		return files;
	}
}

int main(int argc, char **argv) {
	engine::core::Arguments arguments(
		"contentimport", "Brings files and folders into the local content store."
	);

	arguments.Value("root", "PATH", "The store. Defaults to ~/Documents/atomic-game-engine/cdn");
	arguments.Flag("publish", "Publish raw/ into processed/ once the imports are done");
	arguments.Value("key", "HEX", "64 hex characters of Ed25519 seed. Needed by --publish");

	const engine::core::Arguments::Result parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::fprintf(stderr, "%s\n", parsed.Error.c_str());
		return 2;
	}
	if (parsed.HelpRequested) {
		std::printf("%s", arguments.Help().c_str());
		return 0;
	}

	const auto root = arguments.Get("root");
	const cdn::LocalPaths paths =
		root.has_value() ? cdn::LocalPathsUnder(std::filesystem::path(*root)) : cdn::DefaultLocalPaths();

	if (!cdn::EnsureLocalStore(paths)) {
		return 1;
	}
	ENGINE_INFO("content store at {}", paths.Root.string());

	const uint64_t now = NowSeconds();
	size_t imported = 0;
	size_t duplicates = 0;
	size_t failures = 0;
	uint64_t bytes = 0;

	for (const std::string_view given : arguments.Positional()) {
		for (const std::filesystem::path &file : Expand(std::filesystem::path(given))) {
			const auto report = cdn::ImportFile(paths, file, now);
			if (!report.has_value()) {
				failures++;
				continue;
			}
			if (report->Duplicate) {
				duplicates++;
			} else {
				imported++;
				bytes += report->Bytes;
			}
		}
	}

	ENGINE_INFO(
		"imported {} file(s), {} already present, {} failed, {} byte(s) added",
		imported,
		duplicates,
		failures,
		bytes
	);

	if (!arguments.Has("publish")) {
		return failures == 0 ? 0 : 1;
	}

	// **The key is asked for rather than invented.** A publish signs a manifest,
	// and a tool that generated its own key would produce a store no client
	// trusts and no message saying why.
	const auto hex = arguments.Get("key");
	if (!hex.has_value()) {
		ENGINE_ERROR("--publish needs --key: a manifest nobody signed is one no client will take");
		return 2;
	}

	const auto signing = SeedFromHex(*hex);
	if (!signing.has_value()) {
		ENGINE_ERROR("--key must be 64 lowercase hex characters");
		return 2;
	}

	const auto published = cdn::PublishLocal(paths, *signing, now);
	if (!published.has_value()) {
		return 1;
	}

	ENGINE_INFO(
		"published {} asset(s) in {} bundle(s), root {}",
		published->Assets,
		published->Bundles,
		published->Root.ToHex()
	);
	return 0;
}
