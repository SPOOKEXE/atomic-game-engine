#include "ExternalEditor.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

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

TEST_CASE("external writes reload until both editors diverge", "[studio][external-editor]") {
	Scratch scratch;
	const std::filesystem::path path = scratch.Root / "World" / "1-Script.luau";
	studio::ExternalDocument document;
	std::string error;
	REQUIRE(studio::StageExternalDocument(path, "first", document, error));

	Overwrite(path, "external");
	std::string reloaded;
	CHECK(
		studio::RefreshExternalDocument(document, "first", reloaded, error) ==
		studio::ExternalRefresh::Reloaded
	);
	CHECK(reloaded == "external");

	Overwrite(path, "second external");
	CHECK(
		studio::RefreshExternalDocument(document, "studio edit", reloaded, error) ==
		studio::ExternalRefresh::Conflict
	);
	CHECK(document.Conflict);

	std::string accepted = "studio edit";
	REQUIRE(studio::AcceptExternalDocument(document, accepted, error));
	CHECK(accepted == "second external");
	CHECK_FALSE(document.Conflict);

	Overwrite(path, "third external");
	CHECK(
		studio::RefreshExternalDocument(document, "studio again", reloaded, error) ==
		studio::ExternalRefresh::Conflict
	);
	REQUIRE(studio::KeepStudioDocument(document, "studio again", error));
	CHECK_FALSE(document.Conflict);
}
