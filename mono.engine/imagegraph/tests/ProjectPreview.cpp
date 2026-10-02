#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.project_preview")
TEST_DEPENDS("engine.imagegraph.document")
using namespace engine::imagegraph;
namespace {
	Document PreviewProject(uint32_t version = 9) {
		Document document;
		document.FormatVersion = version;
		document.Nodes.push_back(
			{"solid",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{12, 34, 56, 255}}}}
		);
		document.Outputs.push_back({"out", "solid", "image"});
		if (version >= 7) document.Project = ProjectSettings{};
		return document;
	}
}
TEST_CASE(
	"Authored grid and ordered guides persist named axes and explicit native visibility",
	"[imagegraph][project_preview]"
) {
	Document document = PreviewProject();
	auto &project = *document.Project;
	project.PreviewGrid = {true, true, {0, 2.5}};
	project.PreviewRulers = {
		{PreviewRulerAxis::Vertical, -12.5},
		{PreviewRulerAxis::Horizontal, 8.0},
		{PreviewRulerAxis::Vertical, -11.0}
	};
	project.ShowPreviewRulers = true;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(ValidProjectPreviewSettings(project));
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	const auto text = Write(document);
	CHECK(text.find("preview_grid 1 1 0 2.5") != std::string::npos);
	CHECK(text.find("preview_rulers 1 3 vertical -12.5 horizontal 8 vertical -11") != std::string::npos);
	Document parsed;
	REQUIRE(Read(text, parsed, diagnostic) == Status::Ok);
	CHECK(parsed == document);
	REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
	CHECK(Write(parsed) == text);
	const auto record = text.substr(text.find("preview_rulers"));
	const auto withoutRecord = text.substr(0, text.find("preview_rulers"));
	REQUIRE(Read(record + withoutRecord, parsed, diagnostic) != Status::Ok); // Header remains required first.
	const auto headerEnd = withoutRecord.find('\n') + 1;
	REQUIRE(
		Read(
			withoutRecord.substr(0, headerEnd) + record + withoutRecord.substr(headerEnd), parsed, diagnostic
		) == Status::Ok
	);
	CHECK(parsed == document);
}
TEST_CASE(
	"Legacy formats retain default roundtrips and migrate to preview defaults",
	"[imagegraph][project_preview]"
) {
	for (uint32_t version = 1; version <= 8; ++version) {
		CAPTURE(version);
		Document document = PreviewProject(version);
		Diagnostic diagnostic;
		Plan plan;
		Document parsed;
		const auto text = Write(document);
		REQUIRE(Read(text, parsed, diagnostic) == Status::Ok);
		CHECK(parsed == document);
		REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
		CHECK(Write(parsed) == text);
		REQUIRE(Migrate(parsed, diagnostic) == Status::Ok);
		CHECK(parsed.FormatVersion == 9);
		if (parsed.Project) {
			CHECK(parsed.Project->PreviewGrid == PreviewGridSettings{});
			CHECK(parsed.Project->PreviewRulers.empty());
			CHECK_FALSE(parsed.Project->ShowPreviewRulers);
			CHECK(parsed.Project->Palette == document.Project->Palette);
		}
		REQUIRE(Compile(parsed, plan, diagnostic) == Status::Ok);
		const auto migrated = parsed;
		REQUIRE(Migrate(parsed, diagnostic) == Status::Ok);
		CHECK(parsed == migrated);
	}
	Document document = PreviewProject(8);
	document.Project->PreviewGrid.Snap = true;
	Diagnostic diagnostic;
	Plan plan;
	CHECK(Compile(document, plan, diagnostic) == Status::UnsupportedVersion);
	REQUIRE(Migrate(document, diagnostic) == Status::Ok);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
}
TEST_CASE(
	"Preview bounds refuse invalid axes nonfinite controls and oversized guides before publication",
	"[imagegraph][project_preview]"
) {
	ProjectSettings project;
	project.PreviewRulers.resize(Limits::MaximumArrayElements, {PreviewRulerAxis::Horizontal, -1});
	CHECK(ValidProjectPreviewSettings(project));
	project.PreviewRulers.push_back({PreviewRulerAxis::Vertical, 0});
	CHECK_FALSE(ValidProjectPreviewSettings(project));
	project.PreviewRulers.resize(1);
	project.PreviewRulers[0].Axis = static_cast<PreviewRulerAxis>(7);
	CHECK_FALSE(ValidProjectPreviewSettings(project));
	project.PreviewRulers[0] = {PreviewRulerAxis::Vertical, std::numeric_limits<double>::infinity()};
	CHECK_FALSE(ValidProjectPreviewSettings(project));
	project.PreviewRulers.clear();
	project.PreviewGrid.Size.X = -1;
	CHECK_FALSE(ValidProjectPreviewSettings(project));
	project.PreviewGrid.Size.X = std::numeric_limits<double>::quiet_NaN();
	CHECK_FALSE(ValidProjectPreviewSettings(project));
	Document sentinel = PreviewProject();
	Diagnostic diagnostic;
	for (const std::string &record :
		 {std::string("preview_grid 0 1 -1 16"),
		  std::string("preview_grid 2 0 16 16"),
		  std::string("preview_rulers 1 1 0 2"),
		  std::string("preview_rulers 1 1 diagonal 2"),
		  std::string("preview_rulers 1 1 horizontal nan"),
		  std::string("preview_rulers 1 1 vertical"),
		  std::string("preview_grid 0 0 16 16\npreview_grid 1 1 4 4"),
		  std::string("preview_rulers 0 0\npreview_rulers 0 0")}) {
		CAPTURE(record);
		Document output = sentinel;
		CHECK(
			Read("imagegraph 9\nproject 32 32 0 3 0\n" + record + "\n", output, diagnostic) ==
			Status::Malformed
		);
		CHECK(output == sentinel);
	}
	Document output = sentinel;
	CHECK(
		Read(
			"imagegraph 9\npreview_rulers 0 " + std::to_string(Limits::MaximumArrayElements + 1) + "\n",
			output,
			diagnostic
		) == Status::LimitExceeded
	);
	CHECK(output == sentinel);
	Document invalid = sentinel;
	invalid.Project->PreviewGrid.Size.Y = -1;
	Plan plan;
	CHECK(Compile(invalid, plan, diagnostic) == Status::InvalidValue);
}
