#include "ImageGraphFilePublish.hpp"

#include "ImageGraphFileSetPublish.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <studio/ImageGraph.hpp>

#if defined(__linux__)
#include <csignal>
#include <sys/resource.h>
#endif

TEST_SUITE_ID("studio.imagegraph.file_publish")
TEST_DEPENDS("engine.imagegraph.document")

namespace {
	using namespace engine::imagegraph;
	struct Directory {
		std::filesystem::path Path =
			std::filesystem::temp_directory_path() /
			("atomic-graph-publish-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		Directory() {
			REQUIRE(std::filesystem::create_directory(Path));
		}
		~Directory() {
			std::error_code error;
			std::filesystem::remove_all(Path, error);
		}
	};
	std::string ReadFile(const std::filesystem::path &path) {
		std::ifstream stream(path, std::ios::binary);
		REQUIRE(stream.is_open());
		return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	}
	bool Publish(
		const std::filesystem::path &path,
		std::string_view text,
		Diagnostic &error,
		uint64_t maximum = studio::IMAGE_GRAPH_FILE_MAXIMUM_BYTES
	) {
		return studio::detail::PublishImageGraphFile(
			path, std::as_bytes(std::span(text.data(), text.size())), error, maximum
		);
	}
	size_t Entries(const Directory &directory) {
		return static_cast<size_t>(std::distance(
			std::filesystem::directory_iterator(directory.Path), std::filesystem::directory_iterator{}
		));
	}
}

TEST_CASE(
	"native graph replacement reloads authored keys and leaves only the destination",
	"[studio][imagegraph][save]"
) {
	Directory directory;
	const auto path = directory.Path / "modified.graph";
	Document original;
	original.FormatVersion = 9;
	original.Nodes = {{"number", "pc.number", "", {12, -8}, {{"value", 3.0}}}};
	original.Outputs = {{"out", "number", "value"}};
	Diagnostic error;
	REQUIRE(Migrate(original, error) == Status::Ok);
	REQUIRE(Publish(path, Write(original), error));
	CHECK(error.Code == Status::Ok);
	Document edited = original;
	edited.Nodes.front().Position = {30, 50};
	edited.Keyframes = {{"number", "value", 2, 7.0, "linear"}};
	edited.Keyframes.front().Subframe = 0.25;
	const auto text = Write(edited);
	REQUIRE_FALSE(text.empty());
	REQUIRE(Publish(path, text, error, text.size()));
	CHECK(ReadFile(path) == text);
	Document loaded;
	REQUIRE(Read(ReadFile(path), loaded, error) == Status::Ok);
	CHECK(loaded == edited);
	CHECK(Entries(directory) == 1);
	CHECK_FALSE(Publish(path, text, error, text.size() - 1));
	CHECK(error.Code == Status::LimitExceeded);
	CHECK(ReadFile(path) == text);
	CHECK(Entries(directory) == 1);
}

TEST_CASE(
	"graph path and rename refusals preserve existing files without temporary debris",
	"[studio][imagegraph][save]"
) {
	Directory directory;
	Diagnostic error;
	const auto path = directory.Path / "blocked.graph";
	REQUIRE(std::filesystem::create_directory(path));
	const auto marker = path / "previous";
	{
		std::ofstream stream(marker);
		stream << "keep";
	}
	CHECK_FALSE(Publish(path, "new", error));
	CHECK(error.Code == Status::InvalidValue);
	CHECK(error.Port == "path");
	CHECK(ReadFile(marker) == "keep");
	CHECK(Entries(directory) == 1);
	CHECK_FALSE(Publish(directory.Path / "missing" / "new.graph", "new", error));
	CHECK_FALSE(Publish({}, "new", error));
	CHECK_FALSE(Publish(directory.Path / "new.graph", "new", error, 0));
	CHECK(Entries(directory) == 1);
}

#if defined(__linux__)
TEST_CASE(
	"a real short graph write preserves the previous save and cleans its sibling",
	"[studio][imagegraph][save]"
) {
	Directory directory;
	const auto path = directory.Path / "existing.graph";
	const std::string original = "previous valid saved document";
	Diagnostic error;
	REQUIRE(Publish(path, original, error));
	struct FileLimit {
		rlimit Previous{};
		using Handler = void (*)(int);
		Handler PreviousSignal = SIG_DFL;
		bool Active = false;
		bool Begin() {
			if (getrlimit(RLIMIT_FSIZE, &Previous)) return false;
			PreviousSignal = std::signal(SIGXFSZ, SIG_IGN);
			if (PreviousSignal == SIG_ERR) return false;
			const rlimit restricted{8, Previous.rlim_max};
			if (setrlimit(RLIMIT_FSIZE, &restricted)) {
				std::signal(SIGXFSZ, PreviousSignal);
				return false;
			}
			Active = true;
			return true;
		}
		bool Restore() {
			if (!Active) return true;
			const bool restored = setrlimit(RLIMIT_FSIZE, &Previous) == 0;
			std::signal(SIGXFSZ, PreviousSignal);
			Active = false;
			return restored;
		}
		~FileLimit() {
			Restore();
		}
	} limit;
	REQUIRE(limit.Begin());
	const bool saved = Publish(path, "replacement longer than eight bytes", error);
	const bool restored = limit.Restore();
	REQUIRE(restored);
	CHECK_FALSE(saved);
	CHECK(error.Code == Status::InvalidValue);
	CHECK(error.Message == "could not write complete graph temporary file");
	CHECK(ReadFile(path) == original);
	CHECK(Entries(directory) == 1);
	REQUIRE(Publish(path, "retry", error));
	CHECK(ReadFile(path) == "retry");
	CHECK(Entries(directory) == 1);
}
#endif

TEST_CASE(
	"collection file commit restores earlier replacements when a later destination refuses",
	"[studio][imagegraph][save]"
) {
	Directory directory;
	Diagnostic diagnostic;
	const auto first = directory.Path / "collection.pxcc";
	const auto second = directory.Path / "collection.meta";
	const auto third = directory.Path / "collection.png";
	bool preview = false;
	SECTION("metadata replacement refuses") {}
	SECTION("preview replacement refuses") {
		preview = true;
	}
	REQUIRE(Publish(first, "old collection", diagnostic));
	REQUIRE(Publish(second, "old metadata", diagnostic));
	if (preview) REQUIRE(Publish(third, "old preview", diagnostic));
	const auto staging = directory.Path / "staging";
	REQUIRE(std::filesystem::create_directory(staging));
	{
		studio::detail::ImageGraphFileSetStage stage;
		stage.Directory = staging;
		stage.Count = preview ? 3 : 2;
		stage.Files[0] = {first, staging / "0.new", staging / "0.old"};
		stage.Files[1] = {second, staging / "missing.new", staging / "1.old"};
		REQUIRE(Publish(stage.Files[0].Temporary, "new collection", diagnostic));
		if (preview) {
			stage.Files[1].Temporary = staging / "1.new";
			stage.Files[2] = {third, staging / "missing.new", staging / "2.old"};
			REQUIRE(Publish(stage.Files[1].Temporary, "new metadata", diagnostic));
		}
		CHECK_FALSE(stage.Commit());
		CHECK(ReadFile(first) == "new collection");
		CHECK(std::filesystem::exists(stage.Files[0].Backup));
		REQUIRE(stage.Rollback());
		CHECK(ReadFile(first) == "old collection");
		CHECK(ReadFile(second) == "old metadata");
		if (preview) CHECK(ReadFile(third) == "old preview");
	}
	CHECK_FALSE(std::filesystem::exists(staging));
	CHECK(Entries(directory) == (preview ? 3 : 2));
}

TEST_CASE("collection file set rejects repeated later destinations", "[studio][imagegraph][save]") {
	Directory directory;
	Diagnostic diagnostic;
	const auto graph = directory.Path / "collection.pxcc";
	const auto sidecar = directory.Path / "collection.meta";
	REQUIRE(Publish(graph, "old graph", diagnostic));
	REQUIRE(Publish(sidecar, "old metadata", diagnostic));
	const std::string replacement = "new";
	const auto bytes = std::as_bytes(std::span(replacement.data(), replacement.size()));
	const std::array<studio::detail::ImageGraphFilePublication, 3> files{
		{{graph, bytes}, {sidecar, bytes}, {sidecar, bytes}}
	};
	CHECK_FALSE(studio::detail::PublishImageGraphFileSet(files, diagnostic, Limits::MaximumEvaluationBytes));
	CHECK(ReadFile(graph) == "old graph");
	CHECK(ReadFile(sidecar) == "old metadata");
	CHECK(Entries(directory) == 2);
}

TEST_CASE(
	"failed collection restore retains old backup and the reported recovery directory",
	"[studio][imagegraph][save]"
) {
	Directory directory;
	Diagnostic diagnostic;
	const auto first = directory.Path / "collection.pxcc";
	const auto second = directory.Path / "collection.meta";
	REQUIRE(Publish(first, "old collection", diagnostic));
	REQUIRE(Publish(second, "old metadata", diagnostic));
	const auto staging = directory.Path / "staging";
	REQUIRE(std::filesystem::create_directory(staging));
	{
		studio::detail::ImageGraphFileSetStage stage;
		stage.Directory = staging;
		stage.Count = 2;
		stage.Files[0] = {first, staging / "0.new", staging / "0.old"};
		stage.Files[1] = {second, staging / "missing.new", staging / "1.old"};
		REQUIRE(Publish(stage.Files[0].Temporary, "new collection", diagnostic));
		REQUIRE_FALSE(stage.Commit());
		REQUIRE(std::filesystem::remove(first));
		REQUIRE(std::filesystem::create_directory(first));
		REQUIRE(Publish(first / "keep", "other writer", diagnostic));
		REQUIRE_FALSE(stage.Rollback());
		CHECK(ReadFile(stage.Files[0].Backup) == "old collection");
		// grug clear the obstruction, but destructor must leave the reported backup alone.
		REQUIRE(std::filesystem::remove_all(first) == 2);
	}
	CHECK(ReadFile(staging / "0.old") == "old collection");
	CHECK_FALSE(std::filesystem::exists(first));
	CHECK(ReadFile(second) == "old metadata");
}
