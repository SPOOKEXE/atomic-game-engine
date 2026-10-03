#include "ImageGraphCookAction.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>

TEST_SUITE_ID("studio.imagegraph.cook_action")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("engine.render.composersurface")

namespace {
	using namespace engine;
	using namespace imagegraph;
	struct Fixture {
		Document Doc;
		EvaluationSnapshot Inputs;
		Diagnostic Error;
		render::hlsl::CookedComposerLibrary Library;
		studio::detail::ImageGraphCookLibraries Libraries;
		core::Name Owner{"cook-test-owner"};
		studio::detail::ImageGraphCookCapture Captured{"hlsl", Owner, 7, 3};
		assets::LocalPaths Paths = assets::LocalPathsUnder(
			std::filesystem::temp_directory_path() /
			("atomic-composer-cook-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
		);
		Fixture() {
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"base",
				 "image.solid",
				 "",
				 {},
				 {{"width", int64_t(3)}, {"height", int64_t(2)}, {"colour", Colour{12, 34, 56, 255}}}},
				{"hlsl",
				 "pc.hlsl",
				 "",
				 {},
				 {{"main",
				   std::string(
					   "output.color=gm_BaseTextureObject.Sample(gm_"
					   "BaseTexture,input.uv)*gain;"
				   )},
				  {"vertex", std::string("ignored")},
				  {"global", std::string{}},
				  {"libraries", std::string{}}}}
			};
			Doc.Nodes.back().DynamicInputs = {
				{"argument_name_0", ValueType::Text, std::string("gain")},
				{"argument_type_0", ValueType::Enum, EnumValue{0}},
				{"argument_value_0", ValueType::Scalar, 0.5}
			};
			Doc.Links = {{"base", "image", "hlsl", "base_texture"}};
			Doc.Outputs = {{"out", "hlsl", "surface"}};
			Plan plan;
			REQUIRE(Compile(Doc, plan, Error) == Status::Ok);
			REQUIRE(EvaluateNodeInputs(Doc, plan, "hlsl", {}, Inputs, Error) == Status::Ok);
		}
		~Fixture() {
			std::error_code ignored;
			std::filesystem::remove_all(Paths.Root, ignored);
		}
		uint64_t Revision(core::Name owner, core::Name name) {
			uint64_t revision = 0;
			(void)Library.Find(owner, name, revision);
			return revision;
		}
		std::optional<std::string>
		Install(core::Name owner, core::Name name, const assets::ShaderData &artifact) {
			return Library.Install(owner, name, artifact);
		}
		bool Cook(
			studio::ImageGraphHistory &history,
			uint64_t budget = Limits::MaximumEvaluationBytes,
			std::string_view selected = "hlsl",
			uint64_t inputRevision = 3
		) {
			return studio::detail::ApplyImageGraphCookAction(
				Doc,
				history,
				Captured,
				selected,
				Owner,
				7,
				inputRevision,
				Inputs,
				Paths,
				Libraries.Views,
				render::hlsl::CookerIdentity(),
				[&](auto owner, auto name) { return Revision(owner, name); },
				[&](auto owner, auto name, const auto &artifact) { return Install(owner, name, artifact); },
				[&](auto owner, auto name, auto revision) { return Library.Remove(owner, name, revision); },
				Error,
				budget > Libraries.Bytes ? budget - Libraries.Bytes : 0
			);
		}
		core::Name Selector() const {
			const auto &values = Doc.Nodes.back().SourceProperties;
			const auto found = std::find_if(values.begin(), values.end(), [](const auto &value) {
				return value.Port == render::hlsl::COOKED_SELECTOR;
			});
			REQUIRE(found != values.end());
			return core::Name(std::get<std::string>(found->Data));
		}
		core::Name PersistedName() const {
			core::Name result;
			for (const auto &entry : std::filesystem::directory_iterator(Paths.Baked)) {
				const auto filename = entry.path().filename().string();
				CHECK(filename.size() == 72);
				REQUIRE_FALSE(result.IsValid());
				result = core::Name(filename);
			}
			REQUIRE(result.IsValid());
			return result;
		}
	};
} // namespace
TEST_CASE(
	"Composer cook publishes a real immutable artifact and one undoable "
	"selector",
	"[studio][composer_cook_action]"
) {
	Fixture f;
	studio::ImageGraphHistory history;
	const auto original = f.Doc;
	REQUIRE_FALSE(render::hlsl::CookerIdentity().empty());
	INFO(f.Error.Message);
	REQUIRE(f.Cook(history));
	const auto cooked = f.Doc;
	const auto name = f.Selector();
	CHECK(name == f.PersistedName());
	CHECK(f.Revision(f.Owner, name) == 1);
	assets::ShaderData artifact;
	REQUIRE_FALSE(render::hlsl::ReadArtifact(f.Paths.Baked, name, artifact));
	render::hlsl::CookedComposerLibrary reopened;
	REQUIRE_FALSE(reopened.Install(f.Owner, name, artifact));
	render::hlsl::SurfaceRequest request;
	REQUIRE_FALSE(
		render::hlsl::BuildSurfaceRequest(f.Doc.Nodes.back(), f.Inputs, reopened, f.Owner, false, request)
	);
	CHECK(request.Textures[0].Pixels[0] == 12);
	REQUIRE(history.Undo(f.Doc));
	CHECK(f.Doc == original);
	CHECK_FALSE(history.CanUndo());
	REQUIRE(history.Redo(f.Doc));
	CHECK(f.Doc == cooked);
	CHECK_FALSE(history.CanRedo());
	CHECK_FALSE(f.Cook(history));
	CHECK(f.Revision(f.Owner, name) == 1);
	CHECK(f.Error.Code == Status::Ok);
}
TEST_CASE(
	"Composer cook history refusal rolls back only its newly installed pair", "[studio][composer_cook_action]"
) {
	Fixture f;
	studio::ImageGraphHistory refused(128, 1);
	const auto original = f.Doc;
	CHECK_FALSE(f.Cook(refused));
	CHECK(f.Doc == original);
	CHECK_FALSE(refused.CanUndo());
	CHECK(f.Error.Code == Status::LimitExceeded);
	const auto name = f.PersistedName();
	CHECK(f.Revision(f.Owner, name) == 0);
	assets::ShaderData artifact;
	REQUIRE_FALSE(render::hlsl::ReadArtifact(f.Paths.Baked, name, artifact));
	REQUIRE_FALSE(f.Library.Install(f.Owner, name, artifact));
	const auto revision = f.Revision(f.Owner, name);
	CHECK_FALSE(f.Cook(refused));
	CHECK(f.Doc == original);
	CHECK(f.Revision(f.Owner, name) == revision);
	CHECK_FALSE(f.Library.Remove(f.Owner, name, revision + 1));
}
TEST_CASE(
	"Composer cook stale captures and residency refusal leave authoring "
	"and store untouched",
	"[studio][composer_cook_action]"
) {
	Fixture f;
	studio::ImageGraphHistory history;
	const auto original = f.Doc;
	SECTION("selection changed") {
		CHECK_FALSE(f.Cook(history, Limits::MaximumEvaluationBytes, "base"));
	}
	SECTION("resolved inputs changed") {
		CHECK_FALSE(f.Cook(history, Limits::MaximumEvaluationBytes, "hlsl", 4));
	}
	SECTION("owner changed") {
		f.Owner = core::Name("different-owner");
		CHECK_FALSE(f.Cook(history));
	}
	SECTION("live bytes refused") {
		CHECK_FALSE(f.Cook(history, 1));
	}
	CHECK(f.Doc == original);
	CHECK_FALSE(history.CanUndo());
	CHECK_FALSE(std::filesystem::exists(f.Paths.Root));
}
TEST_CASE(
	"Composer cook refuses corrupted immutable bytes without replacing them", "[studio][composer_cook_action]"
) {
	Fixture f;
	studio::ImageGraphHistory refused(128, 1);
	CHECK_FALSE(f.Cook(refused));
	const auto name = f.PersistedName();
	const auto file = f.Paths.Baked / std::string(name.Text());
	{
		std::ofstream output(file, std::ios::binary | std::ios::trunc);
		output << "opaque-existing-bytes";
	}
	studio::ImageGraphHistory history;
	const auto original = f.Doc;
	CHECK_FALSE(f.Cook(history));
	CHECK(f.Doc == original);
	CHECK_FALSE(history.CanUndo());
	CHECK(f.Revision(f.Owner, name) == 0);
	std::ifstream input(file, std::ios::binary);
	std::string bytes{std::istreambuf_iterator<char>(input), {}};
	CHECK(bytes == "opaque-existing-bytes");
}
TEST_CASE(
	"Composer cook consumes exact granted library source and refuses "
	"ambiguous bindings",
	"[studio][composer_cook_action]"
) {
	Fixture f;
	studio::ImageGraphHistory history;
	REQUIRE(assets::EnsureLocalStore(f.Paths));
	const auto source = f.Paths.Raw / "math.hlsl";
	{
		std::ofstream file(source);
		file << "float4 LibraryGain(float4 value) { return value * 0.5; }";
	}
	f.Doc.Nodes.back().Values[0].Data = std::string(
		"output.color=LibraryGain(gm_BaseTextureObject.Sample(gm_"
		"BaseTexture,input.uv));"
	);
	f.Doc.Nodes.back().Values[3].Data = std::string("using math;");
	Plan plan;
	REQUIRE(Compile(f.Doc, plan, f.Error) == Status::Ok);
	REQUIRE(EvaluateNodeInputs(f.Doc, plan, "hlsl", {}, f.Inputs, f.Error) == Status::Ok);
	const auto original = f.Doc;
	std::vector<imagegraphexport::GraphFileGrant> grants{{"hlsl", source, false, "math"}};
	std::string failure;
	REQUIRE(studio::detail::ReadImageGraphCookLibraries(grants, "hlsl", f.Libraries, failure));
	CHECK(f.Libraries.Views[0].Name == "math");
	SECTION("real compiled import and native save selector") {
		INFO(f.Error.Message);
		REQUIRE(f.Cook(history));
		CHECK(f.Doc != original);
		assets::ShaderData artifact;
		REQUIRE_FALSE(render::hlsl::ReadArtifact(f.Paths.Baked, f.Selector(), artifact));
	}
	SECTION("duplicate grant keeps previous captured sources") {
		grants.push_back(grants[0]);
		CHECK_FALSE(studio::detail::ReadImageGraphCookLibraries(grants, "hlsl", f.Libraries, failure));
		CHECK(f.Libraries.Sources.size() == 1);
		CHECK(f.Libraries.Views[0].Name == "math");
		CHECK(f.Doc == original);
		CHECK_FALSE(history.CanUndo());
	}
	SECTION("library capture admits prior output and grant names before reading") {
		const auto oldText = f.Libraries.Sources[0].Text;
		const auto oldName = f.Libraries.Sources[0].Name;
		const auto oldView = f.Libraries.Views[0].Source.data();
		const auto oldBytes = studio::detail::ImageGraphCookLibraryBytes(f.Libraries, UINT64_MAX);
		REQUIRE(oldBytes);
		CHECK_FALSE(
			studio::detail::ReadImageGraphCookLibraries(grants, "hlsl", f.Libraries, failure, *oldBytes)
		);
		CHECK(f.Libraries.Sources[0].Text == oldText);
		CHECK(f.Libraries.Sources[0].Name == oldName);
		CHECK(f.Libraries.Views[0].Source.data() == oldView);
		grants[0].Resource = std::string(1024, 'n');
		const uint64_t onlyStorage = *oldBytes + sizeof(studio::detail::ImageGraphCookLibraries) +
									 sizeof(studio::detail::ImageGraphCookLibraries::Source) +
									 sizeof(render::hlsl::Library);
		CHECK_FALSE(
			studio::detail::ReadImageGraphCookLibraries(grants, "hlsl", f.Libraries, failure, onlyStorage)
		);
		CHECK(f.Libraries.Sources[0].Text == oldText);
		CHECK(f.Libraries.Views[0].Name == oldName);
		CHECK(f.Doc == original);
		CHECK_FALSE(history.CanUndo());
	}
	SECTION("missing grant never follows authored library names to disk") {
		f.Libraries = {};
		CHECK_FALSE(f.Cook(history));
		CHECK(f.Doc == original);
		CHECK_FALSE(history.CanUndo());
	}
}
TEST_CASE(
	"Composer owner installation refusal retains every previous pair and "
	"authoring state",
	"[studio][composer_cook_action]"
) {
	Fixture f;
	studio::ImageGraphHistory refused(128, 1);
	REQUIRE_FALSE(f.Cook(refused));
	const auto name = f.PersistedName();
	assets::ShaderData artifact;
	REQUIRE_FALSE(render::hlsl::ReadArtifact(f.Paths.Baked, name, artifact));
	for (size_t index = 0; index < render::hlsl::MAXIMUM_OWNER_PROGRAMS; ++index)
		REQUIRE_FALSE(
			f.Library.Install(f.Owner, core::Name("retained-program-" + std::to_string(index)), artifact)
		);
	const auto before = f.Library.PayloadBytes(f.Owner);
	const auto original = f.Doc;
	studio::ImageGraphHistory history;
	REQUIRE_FALSE(f.Cook(history));
	CHECK(f.Error.Code == Status::LimitExceeded);
	CHECK(f.Doc == original);
	CHECK_FALSE(history.CanUndo());
	CHECK(f.Revision(f.Owner, name) == 0);
	CHECK(f.Library.PayloadBytes(f.Owner) == before);
	CHECK(f.Revision(f.Owner, core::Name("retained-program-0")) == 1);
}
