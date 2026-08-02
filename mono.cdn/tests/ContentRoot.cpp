#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cdn/ContentRoot.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

TEST_SUITE_ID("cdn.contentroot")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.core.metrics")

using engine::core::FrameGraph;
using engine::core::Metrics;

namespace {
	namespace fs = std::filesystem;

	// A tree on disk, removed whatever the test does. Every case here is about
	// what the resolver does with a real filesystem underneath it — canonical(),
	// weakly_canonical() and symlink following have no in-memory equivalent to
	// test against, and a fake one would be testing the fake.
	struct Tree {
		fs::path Root;
		fs::path Outside;

		Tree() {
			// The counter keeps two cases in one binary from sharing a
			// directory. Catch2 runs them in one process, and a leftover file
			// from the previous case reads as a bug in this one.
			static int serial = 0;
			const fs::path base = fs::temp_directory_path() / ("atomic-cdn-" + std::to_string(++serial));

			std::error_code failure;
			fs::remove_all(base, failure);

			Root = base / "content";
			Outside = base / "outside";
			fs::create_directories(Root / "meshes");
			fs::create_directories(Outside);

			Write(Root / "manifest.bin", "manifest");
			Write(Root / "meshes" / "rock.mesh", "rock");
			Write(Outside / "secret.txt", "secret");
		}

		~Tree() {
			std::error_code failure;
			fs::remove_all(Root.parent_path(), failure);
		}

		static void Write(const fs::path &path, std::string_view contents) {
			std::ofstream file(path, std::ios::binary);
			file << contents;
		}

		cdn::ContentRoot Mount() const {
			auto mounted = cdn::ContentRoot::Mount(Root);
			REQUIRE(mounted.has_value());
			return *mounted;
		}
	};
}

TEST_CASE("a content root mounts a directory that exists", "[cdn][contentroot]") {
	Tree tree;

	const auto mounted = cdn::ContentRoot::Mount(tree.Root);
	REQUIRE(mounted.has_value());
	CHECK(mounted->Directory() == fs::canonical(tree.Root));
}

TEST_CASE("a content root canonicalises what it was given", "[cdn][contentroot]") {
	Tree tree;

	// The same directory, spelled the long way round. Two roots that compare
	// unequal would make containment depend on how the deployment spelled its
	// own configuration.
	const auto mounted = cdn::ContentRoot::Mount(tree.Root / "meshes" / ".." / ".");
	REQUIRE(mounted.has_value());
	CHECK(mounted->Directory() == fs::canonical(tree.Root));
}

TEST_CASE("a missing or non-directory root is refused at mount", "[cdn][contentroot]") {
	Tree tree;

	CHECK_FALSE(cdn::ContentRoot::Mount(tree.Root / "no-such-directory").has_value());
	CHECK_FALSE(cdn::ContentRoot::Mount(tree.Root / "manifest.bin").has_value());
	CHECK_FALSE(cdn::ContentRoot::Mount({}).has_value());
}

TEST_CASE("a name inside the root resolves", "[cdn][contentroot]") {
	Tree tree;
	const cdn::ContentRoot root = tree.Mount();

	const auto manifest = root.Resolve("manifest.bin");
	REQUIRE(manifest.has_value());
	CHECK(*manifest == fs::canonical(tree.Root / "manifest.bin"));

	const auto mesh = root.Resolve("meshes/rock.mesh");
	REQUIRE(mesh.has_value());
	CHECK(*mesh == fs::canonical(tree.Root / "meshes" / "rock.mesh"));
}

TEST_CASE("a name that does not exist yet still resolves", "[cdn][contentroot]") {
	Tree tree;
	const cdn::ContentRoot root = tree.Mount();

	// May-it-be-served and is-it-there are different questions. Answering the
	// first with the second would make an upload racy against its own manifest.
	CHECK(root.Resolve("meshes/not-cooked-yet.mesh").has_value());
	CHECK_FALSE(root.Exists("meshes/not-cooked-yet.mesh"));
}

TEST_CASE("a traversing name is refused", "[cdn][contentroot]") {
	Tree tree;
	const cdn::ContentRoot root = tree.Mount();

	CHECK_FALSE(root.Resolve("../outside/secret.txt").has_value());
	CHECK_FALSE(root.Resolve("meshes/../../outside/secret.txt").has_value());
	CHECK_FALSE(root.Resolve("..").has_value());

	// Refused even though it lands back inside. One file with two names is two
	// manifest keys for one piece of content.
	CHECK_FALSE(root.Resolve("meshes/../manifest.bin").has_value());
	CHECK_FALSE(root.Resolve("./manifest.bin").has_value());
}

TEST_CASE("an absolute name is refused", "[cdn][contentroot]") {
	Tree tree;
	const cdn::ContentRoot root = tree.Mount();

	CHECK_FALSE(root.Resolve(tree.Root.string() + "/manifest.bin").has_value());
	CHECK_FALSE(root.Resolve((tree.Outside / "secret.txt").string()).has_value());
	CHECK_FALSE(root.Resolve("/etc/passwd").has_value());
}

TEST_CASE("an empty name is refused", "[cdn][contentroot]") {
	Tree tree;
	const cdn::ContentRoot root = tree.Mount();

	CHECK_FALSE(root.Resolve("").has_value());
	CHECK_FALSE(root.Exists(""));
}

TEST_CASE("the root itself is not content", "[cdn][contentroot]") {
	Tree tree;
	const cdn::ContentRoot root = tree.Mount();

	CHECK_FALSE(root.Resolve(".").has_value());
	CHECK_FALSE(root.Exists("meshes"));
}

TEST_CASE("a symlink out of the root is refused", "[cdn][contentroot]") {
	Tree tree;

	// The case the component check cannot see: every component of the name is
	// ordinary, and the escape is on disk rather than in the string. Skipped
	// rather than failed where the platform will not make one — Windows needs a
	// privilege for it, and a test that fails on a permission says nothing
	// about the resolver.
	std::error_code failure;
	fs::create_directory_symlink(tree.Outside, tree.Root / "escape", failure);
	if (failure) {
		SUCCEED("symlinks unavailable on this platform");
		return;
	}

	const cdn::ContentRoot root = tree.Mount();
	CHECK_FALSE(root.Resolve("escape/secret.txt").has_value());
	CHECK_FALSE(root.Exists("escape/secret.txt"));
}

TEST_CASE("a symlink staying inside the root is served", "[cdn][contentroot]") {
	Tree tree;

	std::error_code failure;
	fs::create_symlink(tree.Root / "manifest.bin", tree.Root / "current.bin", failure);
	if (failure) {
		SUCCEED("symlinks unavailable on this platform");
		return;
	}

	// The containment check is about where a name lands, not about whether a
	// link was involved. Refusing every link would break the one deployment
	// pattern — an atomically swapped `current` — that content delivery is for.
	const cdn::ContentRoot root = tree.Mount();
	const auto current = root.Resolve("current.bin");
	REQUIRE(current.has_value());
	CHECK(*current == fs::canonical(tree.Root / "manifest.bin"));
	CHECK(root.Exists("current.bin"));
}

TEST_CASE("Exists answers for regular files only", "[cdn][contentroot]") {
	Tree tree;
	const cdn::ContentRoot root = tree.Mount();

	CHECK(root.Exists("manifest.bin"));
	CHECK(root.Exists("meshes/rock.mesh"));
	CHECK_FALSE(root.Exists("meshes"));
	CHECK_FALSE(root.Exists("missing.bin"));
	CHECK_FALSE(root.Exists("../outside/secret.txt"));
}

// ---------------------------------------------------------------------------
// The frame graph and the metrics sink
// ---------------------------------------------------------------------------
//
// An origin's cost is almost entirely waiting on a filesystem, so the spans are
// what separate "the disk is slow" from "we resolved the same name four
// thousand times". These cases exist so that a span deleted during a refactor
// fails here rather than being noticed as a hole in the overlay months later.

namespace {
	// The graph is a process-wide collector and Catch2 runs every case in one
	// process. Leaving it enabled would let one case publish into another's
	// frame, so collection is turned on and off around each measurement rather
	// than for the run.
	struct Collected {
		std::vector<engine::core::FrameSpan> Spans;

		bool Named(std::string_view name) const {
			return std::any_of(Spans.begin(), Spans.end(), [name](const auto &span) {
				return span.Name == name;
			});
		}

		const engine::core::FrameSpan *Find(std::string_view name) const {
			const auto found = std::find_if(Spans.begin(), Spans.end(), [name](const auto &span) {
				return span.Name == name;
			});
			return found == Spans.end() ? nullptr : &*found;
		}
	};

	template <typename Work> Collected Collect(Work &&work) {
		FrameGraph::SetEnabled(true);
		FrameGraph::BeginFrame();
		work();
		FrameGraph::EndFrame();

		// Copied out before disabling: the reference Spans() hands back is only
		// stable until the next EndFrame or SetEnabled(false).
		Collected collected{FrameGraph::Spans()};
		FrameGraph::SetEnabled(false);
		return collected;
	}
}

TEST_CASE("resolution reports itself to the frame graph", "[cdn][contentroot][framegraph]") {
	Tree tree;
	const cdn::ContentRoot root = tree.Mount();

	const Collected collected = Collect([&root] { (void)root.Resolve("manifest.bin"); });

	REQUIRE(collected.Named("ContentRoot::Resolve"));
}

TEST_CASE("a mount reports itself to the frame graph", "[cdn][contentroot][framegraph]") {
	Tree tree;

	const Collected collected = Collect([&tree] { (void)cdn::ContentRoot::Mount(tree.Root); });

	REQUIRE(collected.Named("ContentRoot::Mount"));
}

TEST_CASE("Exists nests its resolve inside itself", "[cdn][contentroot][framegraph]") {
	Tree tree;
	const cdn::ContentRoot root = tree.Mount();

	const Collected collected = Collect([&root] { (void)root.Exists("manifest.bin"); });

	const auto *outer = collected.Find("ContentRoot::Exists");
	const auto *inner = collected.Find("ContentRoot::Resolve");
	REQUIRE(outer != nullptr);
	REQUIRE(inner != nullptr);

	// The nesting is the point rather than an accident of where the macros
	// happen to sit. A flat pair would read on the overlay as two independent
	// costs and double-count the resolve against the frame.
	CHECK(inner->Depth == outer->Depth + 1);
	CHECK(inner->Milliseconds <= outer->Milliseconds);
}

TEST_CASE("a refused name is profiled like a served one", "[cdn][contentroot][framegraph]") {
	Tree tree;
	const cdn::ContentRoot root = tree.Mount();

	// A refusal that costs nothing to measure is a refusal nobody can see
	// costing something. Somebody walking the origin should show up as work.
	const Collected collected = Collect([&root] { (void)root.Resolve("../outside/secret.txt"); });

	REQUIRE(collected.Named("ContentRoot::Resolve"));
}

TEST_CASE("served and refused names are counted apart", "[cdn][contentroot][metrics]") {
	Tree tree;
	const cdn::ContentRoot root = tree.Mount();

	Metrics::Clear();
	(void)root.Resolve("manifest.bin");
	(void)root.Resolve("meshes/rock.mesh");
	(void)root.Resolve("../outside/secret.txt");

	const auto counters = Metrics::Drain();
	const auto total = [&counters](std::string_view name) {
		double sum = 0.0;
		for (const auto &counter : counters) {
			if (counter.Name == engine::core::Name(name)) {
				sum += counter.Value;
			}
		}
		return sum;
	};

	CHECK(total("cdn.resolve.served") == 2.0);
	CHECK(total("cdn.resolve.refused") == 1.0);
}
