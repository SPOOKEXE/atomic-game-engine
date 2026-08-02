// A thin main over the cdn library, for the same reason the client's and the
// server's are thin: the test binary needs something to link, and a server
// serving its own assets links this library in-process rather than starting a
// second program. repo_layout.md §2, §11.
//
// It serves nothing yet. The origin's HTTP range layer is `Engine::net` and the
// manifest is `Engine::assets`, and neither exists — so this main mounts the
// root, reports what it would serve, and says plainly that it stopped there.

#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>

#include <cdn/ContentRoot.hpp>
#include <cstdio>
#include <filesystem>

int main(int argc, char **argv) {
	engine::core::Log::Initialise("cdn");

	engine::core::Arguments arguments("cdn", "atomic — serves a game's content.");

	arguments.Flag("verbose", "Log at trace level");
	arguments.Value("root", "DIR", "Directory to serve content from (default: beside the binary)");

	const auto parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::fprintf(stderr, "%s\n\n%s", parsed.Error.c_str(), arguments.Help().c_str());
		return 2;
	}
	if (parsed.HelpRequested) {
		std::fputs(arguments.Help().c_str(), stdout);
		return 0;
	}

	if (arguments.Has("verbose")) {
		engine::core::Log::SetLevel(engine::core::LogLevel::Trace);
	}

	// Beside the binary by default, because the staged cdn/ directory is
	// runnable as it stands and the self-hosted case is a directory that ships
	// with it. The working directory is whoever launched the process.
	std::filesystem::path root = engine::core::Paths::Assets();
	if (auto chosen = arguments.Get("root")) {
		root = std::filesystem::path(*chosen);
	}

	const auto mounted = cdn::ContentRoot::Mount(root);
	if (!mounted) {
		return 1;
	}

	ENGINE_INFO("cdn: content root {}", mounted->Directory().string());
	ENGINE_WARN(
		"cdn: nothing is served — the manifest is Engine::assets and the origin's HTTP "
		"layer is Engine::net, and neither has landed. ROADMAP.md v0.8."
	);
	return 0;
}
