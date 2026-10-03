#include "ComposerCaptureIdentity.hpp"

#include <engine/render/ComposerSurface.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/SourceSkyboxGroup.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>

TEST_SUITE_ID("engine.render.composersurface")
namespace {
	using namespace engine::render::hlsl;
	using namespace engine::imagegraph;
	void Accepted(std::optional<std::string> error) {
		INFO(error.value_or("accepted"));
		REQUIRE_FALSE(error);
	}
	Document Graph() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
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
		document.Nodes.back().DynamicInputs = {
			{"argument_name_0", ValueType::Text, std::string("gain")},
			{"argument_type_0", ValueType::Enum, EnumValue{0}},
			{"argument_value_0", ValueType::Scalar, 0.5}
		};
		document.Links = {{"base", "image", "hlsl", "base_texture"}};
		document.Outputs = {{"out", "hlsl", "surface"}};
		return document;
	}
	std::vector<AuthoredValue> Controls(const EvaluationSnapshot &snapshot) {
		std::vector<AuthoredValue> values;
		for (const auto &value : snapshot.Values()) {
			if (const auto *text = std::get_if<std::string>(&value.Data))
				values.push_back({value.Port, *text});
			else if (const auto *number = std::get_if<double>(&value.Data))
				values.push_back({value.Port, *number});
			else if (const auto *enumeration = std::get_if<EnumValue>(&value.Data))
				values.push_back({value.Port, *enumeration});
		}
		return values;
	}
	struct Fixture {
		Document Doc = Graph();
		Plan Compiled;
		Diagnostic Error;
		EvaluationSnapshot Snapshot;
		engine::assets::ShaderData Shader;
		CookedComposerLibrary Library;
		SurfaceRequest Request;
		engine::core::Name Owner{"composer.owner"}, Asset{"composer.shader"};
		Fixture() {
			REQUIRE(Compile(Doc, Compiled, Error) == Status::Ok);
			REQUIRE(EvaluateNodeInputs(Doc, Compiled, "hlsl", {.Seed = 17}, Snapshot, Error) == Status::Ok);
			engine::imagegraph::Document cooked;
			Accepted(CookNode(
				Doc,
				Doc.Nodes.back(),
				Snapshot,
				{},
				"repo-vendored-shaderc-hlsl",
				false,
				Asset,
				cooked,
				Shader
			));
			Doc = std::move(cooked);
			Accepted(Library.Install(Owner, Asset, Shader));
			Accepted(BuildSurfaceRequest(Doc.Nodes.back(), Snapshot, Library, Owner, false, Request));
		}
	};
} // namespace
TEST_CASE(
	"Composer real frozen input adapter cooks a document copy and packs "
	"one evaluation",
	"[render][composer-surface]"
) {
	Fixture fixture;
	const auto original = Graph();
	CHECK(original.Nodes.back().SourceProperties.empty());
	REQUIRE(fixture.Doc.Nodes.back().SourceProperties.size() == 1);
	CHECK(fixture.Request.Textures[0].Width == 3);
	CHECK(fixture.Request.Textures[0].Height == 2);
	CHECK(fixture.Request.Textures[0].Pixels[0] == 12);
	CHECK(fixture.Request.Format == engine::assets::TextureFormat::RGBA8_LINEAR);
	REQUIRE(fixture.Request.Uniforms.size() >= 4);
	uint32_t word = 0;
	for (size_t b = 0; b < 4; ++b)
		word |= uint32_t(fixture.Request.Uniforms[b]) << (8 * b);
	CHECK(std::bit_cast<float>(word) == 0.5f);
	const auto retained = fixture.Request.Pair.SpirV.Fragment.SpirV;
	fixture.Library.RemoveOwner(fixture.Owner);
	CHECK(BuildSurfaceRequest(
		fixture.Doc.Nodes.back(), fixture.Snapshot, fixture.Library, fixture.Owner, false, fixture.Request
	));
	CHECK(fixture.Request.Pair.SpirV.Fragment.SpirV == retained);
}
TEST_CASE(
	"Composer source edits invalidate cook while numeric edits reuse "
	"installed stage pair",
	"[render][composer-surface]"
) {
	Fixture fixture;
	fixture.Doc.Nodes.back().DynamicInputs[2].Default = 0.75;
	REQUIRE(Compile(fixture.Doc, fixture.Compiled, fixture.Error) == Status::Ok);
	EvaluationSnapshot numeric;
	REQUIRE(
		EvaluateNodeInputs(fixture.Doc, fixture.Compiled, "hlsl", {}, numeric, fixture.Error) == Status::Ok
	);
	Accepted(BuildSurfaceRequest(
		fixture.Doc.Nodes.back(), numeric, fixture.Library, fixture.Owner, true, fixture.Request
	));
	CHECK(fixture.Request.ShaderRevision == 1);
	CHECK(fixture.Request.Format == engine::assets::TextureFormat::RGBA8);
	fixture.Doc.Nodes.back().Values[0].Data = std::string("output.color=float4(1,0,0,1);");
	REQUIRE(Compile(fixture.Doc, fixture.Compiled, fixture.Error) == Status::Ok);
	EvaluationSnapshot changed;
	REQUIRE(
		EvaluateNodeInputs(fixture.Doc, fixture.Compiled, "hlsl", {}, changed, fixture.Error) == Status::Ok
	);
	CHECK(BuildSurfaceRequest(
		fixture.Doc.Nodes.back(), changed, fixture.Library, fixture.Owner, false, fixture.Request
	));
}
TEST_CASE(
	"Composer job declares upload before raster and rejects invalid "
	"requests atomically",
	"[render][composer-surface]"
) {
	Fixture fixture;
	SurfaceJob job;
	Accepted(BuildSurfaceJob(fixture.Request, job));
	REQUIRE(job.Schedule.Shared.size() == 2);
	CHECK(job.Graph.Find(job.Schedule.Shared[0])->Kind == engine::core::Name("composer-upload"));
	CHECK(job.Graph.Find(job.Schedule.Shared[1])->Kind == engine::core::Name("composer-surface"));
	CHECK(job.Graph.FindResource(job.Output)->External);
	CHECK(job.Graph.Find(job.Schedule.Shared[0])->Writes == job.Graph.Find(job.Schedule.Shared[1])->Reads);
	const auto original = job.Graph.Count();
	fixture.Request.Textures[0].Pixels.pop_back();
	CHECK(BuildSurfaceJob(fixture.Request, job));
	CHECK(job.Graph.Count() == original);
}
TEST_CASE(
	"Composer fixed vertex block honors reflected major order and unit "
	"quad bounds",
	"[render][composer-surface]"
) {
	CookedPair pair;
	const auto columns = VertexMatrices(pair, 30, 20);
	CHECK(columns[0] == 30);
	CHECK(columns[5] == 20);
	CHECK(columns[32] == 2);
	CHECK(columns[37] == -2);
	CHECK(columns[35] == -1);
	CHECK(columns[39] == 1);
	pair.VertexRowMajor = true;
	const auto rows = VertexMatrices(pair, 30, 20);
	CHECK(rows[44] == -1);
	CHECK(rows[45] == 1);
	CHECK(rows[47] == 1);
}
TEST_CASE(
	"Composer display download follows raw raster and includes its "
	"physical scratch payload",
	"[render][composer-surface]"
) {
	Fixture fixture;
	const auto linearBytes = SurfaceScratchBytes(fixture.Request);
	fixture.Request.Format = engine::assets::TextureFormat::RGBA8;
	CHECK(SurfaceScratchBytes(fixture.Request) == linearBytes + 3 * 2 * 4 * 4);
	SurfaceJob job;
	Accepted(BuildSurfaceJob(fixture.Request, job));
	REQUIRE(job.Schedule.Shared.size() == 3);
	const auto *raster = job.Graph.Find(job.Schedule.Shared[1]);
	const auto *copy = job.Graph.Find(job.Schedule.Shared[2]);
	REQUIRE(raster);
	REQUIRE(copy);
	CHECK(copy->Kind == engine::core::Name("composer-display-download"));
	CHECK(raster->Writes == copy->Reads);
	CHECK(copy->Writes[0] == job.Output);
	CHECK_FALSE(job.Graph.FindResource(raster->Writes[0])->External);
	CHECK(job.Graph.FindResource(job.Output)->External);
	fixture.Request.Textures[0].Format = SurfaceFormat::R8Unorm;
	fixture.Request.Textures[0].Pixels.assign(6, 127);
	CHECK(SurfaceScratchBytes(fixture.Request) == 6 * 4 * 7 + 160);
}
TEST_CASE(
	"Composer request validates retained pair accounting before "
	"constructing a job",
	"[render][composer-surface]"
) {
	Fixture fixture;
	SurfaceJob job;
	Accepted(BuildSurfaceJob(fixture.Request, job));
	const auto retained = job.Graph.Count();
	fixture.Request.Pair.PayloadBytes = 1;
	CHECK(ValidateSurfaceRequest(fixture.Request));
	CHECK(BuildSurfaceJob(fixture.Request, job));
	CHECK(job.Graph.Count() == retained);
}
TEST_CASE(
	"Composer native save reload preserves derived selector and "
	"executable request",
	"[render][composer-surface]"
) {
	Fixture fixture;
	const auto saved = Write(fixture.Doc);
	Document loaded;
	Diagnostic diagnostic;
	Plan plan;
	REQUIRE(Read(saved, loaded, diagnostic) == Status::Ok);
	REQUIRE(Compile(loaded, plan, diagnostic) == Status::Ok);
	REQUIRE(loaded.Nodes.back().SourceProperties.size() == 1);
	CHECK(loaded.Nodes.back().SourceProperties[0] == fixture.Doc.Nodes.back().SourceProperties[0]);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(loaded, plan, "hlsl", {}, snapshot, diagnostic) == Status::Ok);
	SurfaceRequest request;
	Accepted(
		BuildSurfaceRequest(loaded.Nodes.back(), snapshot, fixture.Library, fixture.Owner, false, request)
	);
	CHECK(request.Shader == fixture.Asset);
	CHECK(request.Pair == fixture.Request.Pair);
}
TEST_CASE(
	"Composer host invocation consumes borrowed resolved values with its "
	"operation cap",
	"[render][composer-surface]"
) {
	Fixture fixture;
	std::vector<AuthoredValue> inputs;
	for (const auto &value : fixture.Snapshot.Values()) {
		if (const auto *text = std::get_if<std::string>(&value.Data))
			inputs.push_back({value.Port, *text});
		else if (const auto *number = std::get_if<double>(&value.Data))
			inputs.push_back({value.Port, *number});
		else if (const auto *enumeration = std::get_if<EnumValue>(&value.Data))
			inputs.push_back({value.Port, *enumeration});
	}
	std::vector<HostResolvedImage> images;
	for (const auto &image : fixture.Snapshot.Images())
		images.push_back({image.Port, &image.Data});
	EvaluationRequest request;
	HostNodeInvocation invocation{
		fixture.Doc.Nodes.back(), request, inputs, images, MAXIMUM_SURFACE_JOB_BYTES
	};
	SurfaceRequest result;
	Accepted(BuildSurfaceRequest(invocation, fixture.Library, fixture.Owner, false, result));
	CHECK(result.Textures == fixture.Request.Textures);
	CHECK(result.Uniforms == fixture.Request.Uniforms);
	invocation.MaximumOperationBytes = SurfaceSourceBytes(result) - 1;
	const auto retained = result.Uniforms;
	CHECK(BuildSurfaceRequest(invocation, fixture.Library, fixture.Owner, false, result));
	CHECK(result.Uniforms == retained);
}

TEST_CASE(
	"Composer completed display bytes have an explicit second-stage graph", "[render][composer-surface]"
) {
	SurfaceJob job;
	Accepted(BuildSurfaceDisplayUploadJob(3, 2, 24, job));
	REQUIRE(job.Schedule.Shared.size() == 1);
	const auto *upload = job.Graph.Find(job.Schedule.Shared[0]);
	REQUIRE(upload);
	CHECK(upload->Kind == engine::core::Name("composer-display-upload"));
	CHECK(job.Graph.FindResource(upload->Reads[0])->Format == engine::graph::ResourceFormat::RGBA8);
	CHECK(job.Graph.FindResource(job.Output)->Format == engine::graph::ResourceFormat::RGBA8_SRGB);
	CHECK(job.Graph.FindResource(upload->Reads[0])->External);
	CHECK(job.Graph.FindResource(upload->Reads[0])->Kind == engine::graph::ResourceKind::Buffer);
	CHECK(job.Graph.FindResource(job.Output)->External);
	CHECK(BuildSurfaceDisplayUploadJob(3, 2, 23, job));
	CHECK(job.Schedule.Shared.size() == 1);
}

TEST_CASE(
	"Composer request quota includes retained metadata and replacement "
	"residency",
	"[render][composer-surface]"
) {
	Fixture fixture;
	const auto baseline = SurfaceSourceBytes(fixture.Request);
	fixture.Request.Pair.SpirV.Arguments[0].Name.reserve(1024 * 1024);
	CHECK(SurfaceSourceBytes(fixture.Request) >= baseline + 1024 * 1024 - 128);
	const auto held = fixture.Request.Pair.SpirV.Arguments[0].Name.capacity();
	std::vector<HostResolvedImage> images;
	for (const auto &image : fixture.Snapshot.Images())
		images.push_back({image.Port, &image.Data});
	EvaluationRequest request;
	auto inputs = Controls(fixture.Snapshot);
	HostNodeInvocation invocation{fixture.Doc.Nodes.back(), request, inputs, images};
	invocation.MaximumOperationBytes = SurfaceSourceBytes(fixture.Request);
	CHECK(BuildSurfaceRequest(invocation, fixture.Library, fixture.Owner, false, fixture.Request));
	CHECK(fixture.Request.Pair.SpirV.Arguments[0].Name.capacity() == held);
}

TEST_CASE(
	"Composer cooked libraries require no runtime source assembly or "
	"library access",
	"[render][composer-surface]"
) {
	Fixture fixture;
	for (auto &value : fixture.Doc.Nodes.back().Values) {
		if (value.Port == "libraries") value.Data = std::string("math");
		if (value.Port == "main")
			value.Data = std::string(
				"output.color=gm_BaseTextureObject.Sample(gm_"
				"BaseTexture,input.uv)*helper(gain);"
			);
	}
	REQUIRE(Compile(fixture.Doc, fixture.Compiled, fixture.Error) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(
		EvaluateNodeInputs(fixture.Doc, fixture.Compiled, "hlsl", {}, snapshot, fixture.Error) == Status::Ok
	);
	const std::array<Library, 1> libraries{{{"math", "float helper(float x){return x*2;}"}}};
	Document cooked;
	Accepted(CookNode(
		fixture.Doc,
		fixture.Doc.Nodes.back(),
		snapshot,
		libraries,
		"fixture-provenance",
		false,
		fixture.Asset,
		cooked,
		fixture.Shader
	));
	Accepted(fixture.Library.Install(fixture.Owner, fixture.Asset, fixture.Shader));
	Accepted(BuildSurfaceRequest(
		cooked.Nodes.back(), snapshot, fixture.Library, fixture.Owner, false, fixture.Request
	));
}

TEST_CASE("Composer packed numeric color uses source RGBA byte extraction", "[render][composer-surface]") {
	Fixture fixture;
	fixture.Doc.Nodes.back().DynamicInputs[0].Default = std::string("tint");
	fixture.Doc.Nodes.back().DynamicInputs[1].Default = EnumValue{8};
	fixture.Doc.Nodes.back().DynamicInputs[2].Type = ValueType::Colour;
	// The source Float-to-Color setter retains this scalar unchanged.
	fixture.Doc.Nodes.back().DynamicInputs[2].Default = double(0x80402010u);
	for (auto &value : fixture.Doc.Nodes.back().Values)
		if (value.Port == "main")
			value.Data = std::string(
				"output.color=gm_BaseTextureObject.Sample(gm_"
				"BaseTexture,input.uv)*tint;"
			);
	REQUIRE(Compile(fixture.Doc, fixture.Compiled, fixture.Error) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(
		EvaluateNodeInputs(fixture.Doc, fixture.Compiled, "hlsl", {}, snapshot, fixture.Error) == Status::Ok
	);
	Document cooked;
	Accepted(CookNode(
		fixture.Doc,
		fixture.Doc.Nodes.back(),
		snapshot,
		{},
		"fixture-provenance",
		false,
		fixture.Asset,
		cooked,
		fixture.Shader
	));
	Accepted(fixture.Library.Install(fixture.Owner, fixture.Asset, fixture.Shader));
	std::vector<HostResolvedImage> images;
	for (const auto &image : snapshot.Images())
		images.push_back({image.Port, &image.Data});
	EvaluationRequest clock;
	auto inputs = Controls(snapshot);
	HostNodeInvocation invocation{cooked.Nodes.back(), clock, inputs, images, MAXIMUM_SURFACE_JOB_BYTES};
	Accepted(BuildSurfaceRequest(invocation, fixture.Library, fixture.Owner, false, fixture.Request));
	REQUIRE(fixture.Request.Uniforms.size() >= 16);
	for (size_t channel = 0; channel < 4; ++channel) {
		uint32_t word = 0;
		for (size_t byte = 0; byte < 4; ++byte)
			word |= uint32_t(fixture.Request.Uniforms[channel * 4 + byte]) << (byte * 8);
		CHECK(std::bit_cast<float>(word) == float((0x80402010u >> (channel * 8)) & 255) / 255.f);
	}
	for (const auto &[integer, packed] :
		 std::array<std::pair<int64_t, uint32_t>, 2>{{{INT64_MAX, UINT32_MAX}, {INT64_MIN, 0}}}) {
		for (auto &input : inputs)
			if (input.Port == "argument_value_0") input.Data = integer;
		Accepted(BuildSurfaceRequest(invocation, fixture.Library, fixture.Owner, false, fixture.Request));
		for (size_t channel = 0; channel < 4; ++channel) {
			uint32_t word = 0;
			for (size_t byte = 0; byte < 4; ++byte)
				word |= uint32_t(fixture.Request.Uniforms[channel * 4 + byte]) << (byte * 8);
			CHECK(std::bit_cast<float>(word) == float((packed >> (channel * 8)) & 255) / 255.f);
		}
	}
}

TEST_CASE(
	"Composer preview identity tracks exact frozen controls and input images", "[render][composer-surface]"
) {
	Fixture fixture;
	std::vector<HostResolvedImage> images;
	for (const auto &image : fixture.Snapshot.Images())
		images.push_back({image.Port, &image.Data});
	EvaluationRequest clock;
	auto inputs = Controls(fixture.Snapshot);
	HostNodeInvocation invocation{fixture.Doc.Nodes.back(), clock, inputs, images, MAXIMUM_SURFACE_JOB_BYTES};
	HostNodeCapture original;
	Diagnostic diagnostic;
	uint64_t bytes = 0;
	REQUIRE(
		PrepareResolvedHostCapture(invocation, MAXIMUM_SURFACE_JOB_BYTES, original, bytes, diagnostic) ==
		Status::Ok
	);
	auto requested = original;
	CHECK(engine::render::detail::SameComposerCaptureInputs(original, requested, 1, 1));
	requested.Images.push_back({"surface", Image{1, 1, {1, 2, 3, 4}, 0}});
	CHECK(engine::render::detail::SameComposerCaptureInputs(original, requested, 1, 1));
	CHECK_FALSE(engine::render::detail::SameComposerCaptureInputs(original, requested, 1, 2));
	requested.Tick = 1;
	CHECK_FALSE(engine::render::detail::SameComposerCaptureInputs(original, requested, 1, 1));
	requested = original;
	requested.InputImages[0].Hash ^= 1;
	CHECK_FALSE(engine::render::detail::SameComposerCaptureInputs(original, requested, 1, 1));
	requested = original;
	requested.Authored.SourceProperties[0].Data = std::string("changed");
	CHECK_FALSE(engine::render::detail::SameComposerCaptureInputs(original, requested, 1, 1));
	SurfaceJob job;
	Accepted(BuildSurfaceJob(fixture.Request, job, true));
	REQUIRE(job.Schedule.Shared.size() == 3);
	CHECK(job.Graph.Find(job.Schedule.Shared[2])->Kind == engine::core::Name("composer-display-download"));
	CHECK(job.Graph.FindResource(job.Output)->Format == engine::graph::ResourceFormat::RGBA8);
}

TEST_CASE(
	"Composer unbound source sampler preserves source slot and refuses "
	"active ambiguity",
	"[render][composer-surface]"
) {
	Fixture fixture;
	auto authored = fixture.Doc.Nodes.back();
	authored.DynamicInputs[0].Default = std::string("extra");
	authored.DynamicInputs[1].Default = EnumValue{7};
	authored.DynamicInputs[2].Type = ValueType::Image;
	authored.DynamicInputs[2].Default = int64_t(-4);
	auto inputs = Controls(fixture.Snapshot);
	for (auto &input : inputs) {
		if (input.Port == "argument_name_0") input.Data = std::string("extra");
		if (input.Port == "argument_type_0") input.Data = EnumValue{7};
		if (input.Port == "argument_value_0") input.Data = int64_t(-4);
	}
	std::vector<HostResolvedImage> images;
	for (const auto &image : fixture.Snapshot.Images())
		images.push_back({image.Port, &image.Data});
	EvaluationRequest clock;
	const std::array<Argument, 1> arguments{{{"extra", ArgumentKind::Sampler2D}}};
	const std::string main = "output.color=gm_BaseTextureObject.Sample(gm_"
							 "BaseTexture,input.uv)+extraObject."
							 "Sample(extra,input.uv);";
	for (auto &input : inputs)
		if (input.Port == "main") input.Data = main;
	Accepted(
		CookArtifact({"ignored", main, "", "", arguments}, {}, "fixture-provenance", false, fixture.Shader)
	);
	Accepted(fixture.Library.Install(fixture.Owner, fixture.Asset, fixture.Shader));
	HostNodeInvocation invocation{authored, clock, inputs, images, MAXIMUM_SURFACE_JOB_BYTES};
	const auto previous = fixture.Request.ShaderRevision;
	CHECK(BuildSurfaceRequest(invocation, fixture.Library, fixture.Owner, false, fixture.Request));
	CHECK(fixture.Request.ShaderRevision == previous);
	const std::string inactiveMain = "output.color=gm_BaseTextureObject.Sample(gm_BaseTexture,input.uv);";
	for (auto &input : inputs)
		if (input.Port == "main") input.Data = inactiveMain;
	Accepted(CookArtifact(
		{"ignored", inactiveMain, "", "", arguments}, {}, "fixture-provenance", false, fixture.Shader
	));
	Accepted(fixture.Library.Install(fixture.Owner, fixture.Asset, fixture.Shader));
	Accepted(BuildSurfaceRequest(invocation, fixture.Library, fixture.Owner, false, fixture.Request));
	REQUIRE(fixture.Request.Textures.size() == 2);
	CHECK(fixture.Request.Textures[1].Width == 1);
	CHECK(fixture.Request.Textures[1].Pixels == std::vector<uint8_t>(4, 0));
}

TEST_CASE(
	"Composer raw integer argument truncates and wraps source signed32 bytes", "[render][composer-surface]"
) {
	Fixture fixture;
	auto authored = fixture.Doc.Nodes.back();
	authored.DynamicInputs[1].Default = EnumValue{1};
	authored.DynamicInputs[2].Type = ValueType::Integer;
	const std::array<Argument, 1> arguments{{{"gain", ArgumentKind::Int}}};
	const std::string main = "output.color=gm_BaseTextureObject.Sample(gm_BaseTexture,input.uv)*gain;";
	Accepted(
		CookArtifact({"ignored", main, "", "", arguments}, {}, "fixture-provenance", false, fixture.Shader)
	);
	Accepted(fixture.Library.Install(fixture.Owner, fixture.Asset, fixture.Shader));
	auto inputs = Controls(fixture.Snapshot);
	for (auto &input : inputs)
		if (input.Port == "argument_type_0") input.Data = EnumValue{1};
	std::vector<HostResolvedImage> images;
	for (const auto &image : fixture.Snapshot.Images())
		images.push_back({image.Port, &image.Data});
	EvaluationRequest clock;
	HostNodeInvocation invocation{authored, clock, inputs, images, MAXIMUM_SURFACE_JOB_BYTES};
	const std::array<std::pair<double, uint32_t>, 6> cases{
		{{2.5, 2},
		 {-2.5, 0xfffffffeu},
		 {4294967297.5, 1},
		 {-4294967297.5, 0xffffffffu},
		 {2147483648.5, 0x80000000u},
		 {-2147483649.5, 0x7fffffffu}}
	};
	for (const auto &[raw, expected] : cases) {
		for (auto &input : inputs)
			if (input.Port == "argument_value_0") input.Data = raw;
		Accepted(BuildSurfaceRequest(invocation, fixture.Library, fixture.Owner, false, fixture.Request));
		uint32_t word = 0;
		for (size_t byte = 0; byte < 4; ++byte)
			word |= uint32_t(fixture.Request.Uniforms[byte]) << (8 * byte);
		CHECK(word == expected);
	}
	for (auto &input : inputs)
		if (input.Port == "argument_value_0") input.Data = int64_t(0x20000000000001ll);
	Accepted(BuildSurfaceRequest(invocation, fixture.Library, fixture.Owner, false, fixture.Request));
	CHECK(fixture.Request.Uniforms[0] == std::byte(1));
}

TEST_CASE(
	"Composer capture vector growth admits overlapping backing before "
	"replacement",
	"[render][composer-surface]"
) {
	using engine::render::hlsl::detail::AdmitComposerVectorGrowth;
	const auto exact = AdmitComposerVectorGrowth(32, 64, 100, 20, 184);
	REQUIRE(exact);
	CHECK(*exact == 132);
	CHECK_FALSE(AdmitComposerVectorGrowth(32, 64, 100, 20, 183));
	CHECK_FALSE(AdmitComposerVectorGrowth(101, 64, 100, 20, 184));
	CHECK_FALSE(AdmitComposerVectorGrowth(32, UINT64_MAX, 100, 20, UINT64_MAX));
}
TEST_CASE(
	"Composer node cooking admits prior output and document candidate before publication",
	"[render][composer-surface]"
) {
	auto document = Graph();
	Plan plan;
	Diagnostic error;
	EvaluationSnapshot snapshot;
	REQUIRE(Compile(document, plan, error) == Status::Ok);
	REQUIRE(EvaluateNodeInputs(document, plan, "hlsl", {}, snapshot, error) == Status::Ok);
	Document previous = document;
	previous.Nodes.reserve(128);
	const auto originalNodes = previous.Nodes;
	engine::assets::ShaderData shader;
	const auto previousBytes = DocumentRetainedPayloadBytes(previous);
	const auto shaderBytes = engine::assets::ShaderRetainedPayloadBytes(shader);
	REQUIRE(previousBytes);
	REQUIRE(shaderBytes);
	const auto failure = CookNode(
		document,
		document.Nodes.back(),
		snapshot,
		{},
		"fixture-provenance",
		false,
		engine::core::Name("candidate.ashader"),
		previous,
		shader,
		{},
		*previousBytes + *shaderBytes
	);
	REQUIRE(failure);
	CHECK(failure->find("document candidate") != std::string::npos);
	CHECK(previous.Nodes == originalNodes);
	CHECK(shader.Variants.empty());
}

TEST_CASE(
	"Composer fixed HLSL vertex matrices cover the unit surface under reflected SPIR-V layout",
	"[render][composer-surface]"
) {
	Fixture fixture;
	const auto matrices = VertexMatrices(fixture.Request.Pair, 3, 2);
	const auto project = [&](float x, float y) {
		const std::array<float, 4> position{x, y, 0, 1};
		std::array<float, 4> clip{};
		for (size_t column = 0; column < 4; ++column)
			for (size_t row = 0; row < 4; ++row) {
				const auto index =
					32 + (fixture.Request.Pair.VertexRowMajor ? row * 4 + column : column * 4 + row);
				clip[column] += position[row] * matrices[index];
			}
		return clip;
	};
	CHECK(project(0, 0) == std::array<float, 4>{-1, 1, 0, 1});
	CHECK(project(1, 0) == std::array<float, 4>{1, 1, 0, 1});
	CHECK(project(0, 1) == std::array<float, 4>{-1, -1, 0, 1});
	CHECK(project(1, 1) == std::array<float, 4>{1, -1, 0, 1});
}
TEST_CASE(
	"Composer world teardown retires queued source work with no device and preserves other owners",
	"[render][composer-surface]"
) {
	Fixture fixture;
	engine::render::Renderer renderer;
	const engine::core::Name other("composer.retained.other"), name("composer.retired.output");
	Accepted(renderer.InstallComposerShader(fixture.Owner, fixture.Asset, fixture.Shader));
	Accepted(renderer.InstallComposerShader(other, fixture.Asset, fixture.Shader));
	using Queue = engine::render::imagegraph::TransformImage3DQueueResult;
	REQUIRE(renderer.QueueComposerSurface({fixture.Owner, name, 1, fixture.Request}) == Queue::Queued);
	REQUIRE(renderer.QueueComposerSurface({other, name, 1, fixture.Request}) == Queue::Queued);
	renderer.ForgetWorld(7, fixture.Owner);
	CHECK(renderer.SourceOutputStatus(fixture.Owner, name, 1) == engine::render::SourceTextureStatus::Absent);
	CHECK(renderer.SourceOutputStatus(other, name, 1) == engine::render::SourceTextureStatus::Pending);
	CHECK(renderer.ComposerShaderRevision(fixture.Owner, fixture.Asset) == 0);
	CHECK(renderer.ComposerShaderRevision(other, fixture.Asset) == 1);
}
