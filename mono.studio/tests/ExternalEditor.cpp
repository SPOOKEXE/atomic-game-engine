#include "ExternalEditor.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

TEST_SUITE_ID("studio.external-editor")

namespace {
	struct Scratch {
		Scratch() : Root(std::filesystem::temp_directory_path() / "atomic-studio-external-editor-test") {
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
		}

		~Scratch() {
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
		}

		std::filesystem::path Root;
	};

	void Overwrite(const std::filesystem::path &path, const std::string_view text) {
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		REQUIRE(output);
		output << text;
		output.close();
		REQUIRE(output.good());

		// Filesystems with coarse timestamps can otherwise report an unchanged
		// stamp for two writes in one test process.
		std::error_code failure;
		const auto written = std::filesystem::last_write_time(path, failure);
		REQUIRE_FALSE(failure);
		std::filesystem::last_write_time(path, written + std::chrono::seconds(2), failure);
		REQUIRE_FALSE(failure);
	}
}

TEST_CASE("external editor choices have stable readable spellings", "[studio][external-editor]") {
	for (size_t index = 0; index < studio::EXTERNAL_EDITOR_KIND_COUNT; index++) {
		const auto kind = static_cast<studio::ExternalEditorKind>(index);
		CHECK(studio::ExternalEditorKindOf(studio::Describe(kind)) == kind);
	}
	CHECK_FALSE(studio::ExternalEditorKindOf("unknown").has_value());

	studio::ExternalEditorSettings custom;
	custom.Kind = studio::ExternalEditorKind::Custom;
	std::string error;
	CHECK_FALSE(studio::LaunchExternalEditor(custom, "source.luau", error));
	CHECK_FALSE(error.empty());
}

TEST_CASE("external document paths contain no authored traversal", "[studio][external-editor]") {
	const std::filesystem::path path =
		studio::ExternalDocumentPath("root", "../World", 42, "../../Shader", "glsl");
	CHECK(path.parent_path() == std::filesystem::path("root") / "___World");
	CHECK(path.filename() == "42-______Shader.glsl");
}

TEST_CASE("external documents do not overwrite other drafts", "[studio][external-editor]") {
	Scratch scratch;
	const auto first = studio::ExternalDocumentPath(scratch.Root, "World A", 42, "Script", "luau");
	const auto second = studio::ExternalDocumentPath(scratch.Root, "World_A", 42, "Script", "luau");
	std::filesystem::create_directories(first.parent_path());
	Overwrite(first, "existing user file");
	studio::ExternalDocument a;
	studio::ExternalDocument b;
	studio::ExternalDocument detached;
	std::string error;
	REQUIRE(studio::StageExternalDocument(first, "first draft", a, error));
	REQUIRE(studio::StageExternalDocument(second, "second draft", b, error));
	REQUIRE(studio::StageExternalDocument(first, "detached draft", detached, error));
	CHECK(a.Path != b.Path);
	CHECK(a.Path != detached.Path);
	std::string text;
	REQUIRE(studio::AcceptExternalDocument(a, text, error));
	CHECK(text == "first draft");
	REQUIRE(studio::AcceptExternalDocument(b, text, error));
	CHECK(text == "second draft");
	std::ifstream existing(first);
	std::getline(existing, text);
	CHECK(text == "existing user file");
	const auto oldPath = a.Path;
	REQUIRE(studio::KeepStudioDocument(a, "updated", error));
	CHECK(a.Path == oldPath);
	REQUIRE(studio::AcceptExternalDocument(a, text, error));
	CHECK(text == "updated");

	const auto blocked = scratch.Root / "not-a-directory";
	Overwrite(blocked, "keep");
	studio::ExternalDocument failed;
	CHECK_FALSE(studio::StageExternalDocument(blocked / "source.luau", "ignored", failed, error));
	CHECK_FALSE(failed.Active());
	CHECK_FALSE(error.empty());

	const auto emptyName = scratch.Root / "missing-name" / "";
	CHECK_FALSE(studio::StageExternalDocument(emptyName, "ignored", failed, error));
	CHECK(std::filesystem::is_empty(emptyName.parent_path()));
}

TEST_CASE("external document reservations are atomic", "[studio][external-editor]") {
	Scratch scratch;
	const auto world = scratch.Root / "World";
	std::filesystem::create_directories(world);
	for (size_t index = 0; index < 32; ++index) {
		const auto slot = world / ("document-" + std::to_string(index));
		std::filesystem::create_directory(slot);
		Overwrite(slot / "sentinel", "keep");
	}
	constexpr size_t COUNT = 8;
	std::array<studio::ExternalDocument, COUNT> documents;
	std::array<std::string, COUNT> errors;
	std::array<bool, COUNT> staged{};
	std::array<std::thread, COUNT> threads;
	for (size_t index = 0; index < COUNT; ++index) {
		threads[index] = std::thread([&, index] {
			staged[index] = studio::StageExternalDocument(
				world / "source.luau", std::to_string(index), documents[index], errors[index]
			);
		});
	}
	for (auto &thread : threads)
		thread.join();
	for (size_t index = 0; index < COUNT; ++index) {
		REQUIRE(staged[index]);
		for (size_t other = 0; other < index; ++other)
			CHECK(documents[index].Path != documents[other].Path);
		std::string text;
		REQUIRE(studio::AcceptExternalDocument(documents[index], text, errors[index]));
		CHECK(text == std::to_string(index));
	}
	for (size_t index = 0; index < 32; ++index) {
		std::ifstream sentinel(world / ("document-" + std::to_string(index)) / "sentinel");
		std::string text;
		std::getline(sentinel, text);
		CHECK(text == "keep");
	}
}

TEST_CASE("external writes reload until both editors diverge", "[studio][external-editor]") {
	Scratch scratch;
	const std::filesystem::path path = scratch.Root / "World" / "1-Script.luau";
	studio::ExternalDocument document;
	std::string error;
	REQUIRE(studio::StageExternalDocument(path, "first", document, error));

	Overwrite(document.Path, "external");
	std::string reloaded;
	CHECK(
		studio::RefreshExternalDocument(document, "first", reloaded, error) ==
		studio::ExternalRefresh::Reloaded
	);
	CHECK(reloaded == "external");

	Overwrite(document.Path, "second external");
	CHECK(
		studio::RefreshExternalDocument(document, "studio edit", reloaded, error) ==
		studio::ExternalRefresh::Conflict
	);
	CHECK(document.Conflict);

	std::string accepted = "studio edit";
	REQUIRE(studio::AcceptExternalDocument(document, accepted, error));
	CHECK(accepted == "second external");
	CHECK_FALSE(document.Conflict);

	Overwrite(document.Path, "third external");
	CHECK(
		studio::RefreshExternalDocument(document, "studio again", reloaded, error) ==
		studio::ExternalRefresh::Conflict
	);
	REQUIRE(studio::KeepStudioDocument(document, "studio again", error));
	CHECK_FALSE(document.Conflict);
}
