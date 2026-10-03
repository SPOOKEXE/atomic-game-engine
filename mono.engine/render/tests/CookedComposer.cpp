#include <engine/render/CookedComposer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>

TEST_SUITE_ID("engine.render.cookedcomposer")
namespace {
	using namespace engine::render::hlsl;
	constexpr std::string_view MAIN =
		"output.color=gm_BaseTextureObject.Sample(gm_BaseTexture,input.uv)*gain+tint;";
	void Accepted(std::optional<std::string> error) {
		INFO(error.value_or("accepted"));
		REQUIRE_FALSE(error);
	}
	engine::assets::ShaderData Artifact(bool metal = false) {
		const std::array<Argument, 2> arguments{
			{{"gain", ArgumentKind::Float}, {"tint", ArgumentKind::Color}}
		};
		engine::assets::ShaderData data;
		Accepted(CookArtifact(
			{"ignored source vertex", MAIN, "", "", arguments},
			{},
			"repo-vendored-shaderc-hlsl",
			metal,
			data,
			metal ? "fixture-offline-translator" : ""
		));
		return data;
	}
}
TEST_CASE(
	"Composer cooked pair survives ASH1 and admits reflected numeric types", "[render][cooked-composer]"
) {
	using namespace engine::assets;
	auto data = Artifact();
	engine::core::ByteWriter bytes;
	REQUIRE(Shader::Write(bytes, data));
	engine::core::ByteReader reader(bytes.Bytes());
	ShaderData decoded;
	REQUIRE(Shader::Read(reader, decoded));
	CookedPair pair;
	Accepted(AdmitArtifact(decoded, pair));
	REQUIRE(pair.SpirV.Arguments.size() == 2);
	CHECK(pair.SpirV.Arguments[1].Kind == ArgumentKind::Color);
	CHECK(pair.PayloadBytes == (pair.SpirV.Vertex.SpirV.size() + pair.SpirV.Fragment.SpirV.size()) * 4);
}
TEST_CASE(
	"Composer artifact admission rejects tampered packing and undeclared pair identity",
	"[render][cooked-composer]"
) {
	auto data = Artifact();
	CookedPair pair;
	Accepted(AdmitArtifact(data, pair));
	const auto words = pair.SpirV.Fragment.SpirV;
	auto bad = data;
	bad.Variants[0].Parameters[0].Offset += 4;
	CHECK(AdmitArtifact(bad, pair));
	CHECK(pair.SpirV.Fragment.SpirV == words);
	bad = data;
	bad.Variants[0].Name = "other";
	CHECK(AdmitArtifact(bad, pair));
	bad = data;
	bad.Variants[0].Features[1].Value = "4:tint";
	CHECK(AdmitArtifact(bad, pair));
	bad = data;
	bad.CompilerOptions.clear();
	CHECK(AdmitArtifact(bad, pair));
	bad = data;
	bad.Variants.pop_back();
	CHECK(AdmitArtifact(bad, pair));
}
TEST_CASE(
	"Composer owner library keeps last good pair on failure and advances installed revision",
	"[render][cooked-composer]"
) {
	using engine::core::Name;
	const Name owner("world.a"), other("world.b"), name("cooked.composer");
	CookedComposerLibrary library;
	auto data = Artifact();
	Accepted(library.Install(owner, name, data));
	uint64_t revision = 0;
	REQUIRE(library.Find(owner, name, revision));
	CHECK(revision == 1);
	const auto bytes = library.PayloadBytes(owner);
	REQUIRE(bytes > 0);
	auto bad = data;
	bad.Variants.front().Payloads.front().Bytes[0] = std::byte{0};
	CHECK(library.Install(owner, name, bad));
	REQUIRE(library.Find(owner, name, revision));
	CHECK(revision == 1);
	CHECK(library.PayloadBytes(owner) == bytes);
	CHECK_FALSE(library.Find(other, name, revision));
	CHECK(revision == 0);
	Accepted(library.Install(owner, name, data));
	REQUIRE(library.Find(owner, name, revision));
	CHECK(revision == 2);
	library.RemoveOwner(owner);
	CHECK_FALSE(library.Find(owner, name, revision));
	CHECK(library.PayloadBytes(owner) == 0);
}
TEST_CASE(
	"Composer definition fingerprint separates field boundaries and declarations", "[render][cooked-composer]"
) {
	engine::assets::ContentHash first, second;
	Accepted(DefinitionFingerprint({"a", "bc", "", "", {}}, first));
	Accepted(DefinitionFingerprint({"ab", "c", "", "", {}}, second));
	CHECK(first != second);
	const std::array<Argument, 1> a{{{"n", ArgumentKind::Float}}}, b{{{"n", ArgumentKind::Int}}};
	Accepted(DefinitionFingerprint({"", "", "", "", a}, first));
	Accepted(DefinitionFingerprint({"", "", "", "", b}, second));
	CHECK(first != second);
	const std::string huge(MAXIMUM_SOURCE_BYTES + 1, 'a');
	const auto preserved = second;
	CHECK(DefinitionFingerprint({huge, "", "", "", {}}, second));
	CHECK(second == preserved);
}
TEST_CASE(
	"Composer offline MSL retains exact ordered source sampler declaration", "[render][cooked-composer]"
) {
	const std::array<Argument, 2> arguments{
		{{"unused", ArgumentKind::Sampler2D}, {"active", ArgumentKind::Sampler2D}}
	};
	engine::assets::ShaderData data;
	Accepted(CookArtifact(
		{"", "output.color=activeObject.Sample(active,input.uv);", "", "", arguments},
		{},
		"repo-vendored-shaderc-hlsl",
		true,
		data,
		"fixture-offline-translator"
	));
	CookedPair pair;
	Accepted(AdmitArtifact(data, pair));
	REQUIRE_FALSE(pair.VertexMsl.empty());
	REQUIRE_FALSE(pair.FragmentMsl.empty());
	std::vector<SamplerBinding> native, metal;
	Accepted(SamplerBindings(pair.SpirV, "spirv", native));
	Accepted(SamplerBindings(pair.SpirV, "msl", metal));
	REQUIRE(native.size() == 1);
	REQUIRE(metal.size() == 1);
	CHECK(native[0].SourceSlot == 2);
	CHECK(native[0].BackendSlot == 2);
	CHECK(metal[0].SourceSlot == 2);
	CHECK(metal[0].BackendSlot == 0);
	data.Variants.back().Payloads.erase(data.Variants.back().Payloads.begin());
	CHECK(AdmitArtifact(data, pair));
}
TEST_CASE("Composer rollback removes only the exact newly installed revision", "[render][cooked-composer]") {
	using engine::core::Name;
	CookedComposerLibrary library;
	const Name owner("rollback.owner"), asset("rollback.asset");
	auto shader = Artifact();
	Accepted(library.Install(owner, asset, shader));
	uint64_t revision = 0;
	REQUIRE(library.Find(owner, asset, revision));
	CHECK_FALSE(library.Remove(owner, asset, revision + 1));
	REQUIRE(library.Find(owner, asset, revision));
	Accepted(library.Install(owner, asset, shader));
	CHECK_FALSE(library.Remove(owner, asset, revision));
	REQUIRE(library.Find(owner, asset, revision));
	CHECK(revision == 2);
	CHECK(library.Remove(owner, asset, revision));
	CHECK_FALSE(library.Find(owner, asset, revision));
	CHECK(library.PayloadBytes(owner) == 0);
}
TEST_CASE(
	"Composer offline translation refuses missing provenance before replacing output",
	"[render][cooked-composer]"
) {
	auto shader = Artifact();
	const auto retained = shader.Variants;
	CHECK(
		CookArtifact({"", "output.color=float4(1,0,0,1);", "", "", {}}, {}, "fixture-compiler", true, shader)
	);
	CHECK(shader.Variants == retained);
}

TEST_CASE(
	"Composer content-addressed persisted bytes load and roll back owner cache atomically",
	"[render][cooked-composer]"
) {
	const auto root = std::filesystem::temp_directory_path() /
					  ("composer-cooked-host-" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	REQUIRE(std::filesystem::create_directory(root));
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}
	} cleanup{root};
	auto artifact = Artifact();
	engine::core::ByteWriter writer;
	REQUIRE(engine::assets::Shader::Write(writer, artifact));
	const engine::core::Name name(engine::assets::Hasher::Of(writer.Bytes()).ToHex() + ".ashader");
	const auto path = root / std::string(name.Text());
	{
		std::ofstream file(path, std::ios::binary);
		const auto bytes = writer.Bytes();
		file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
		REQUIRE(file.good());
	}
	engine::assets::ShaderData loaded;
	Accepted(ReadArtifact(root, name, loaded));
	const auto preserved = loaded;
	const auto decodedBytes = engine::assets::ShaderRetainedPayloadBytes(loaded);
	REQUIRE(decodedBytes);
	CHECK(ReadArtifact(root, name, loaded, *decodedBytes + writer.Bytes().size()));
	CHECK(loaded == preserved);
	CookedComposerLibrary library;
	const engine::core::Name owner("persisted.owner");
	Accepted(library.Install(owner, name, loaded));
	uint64_t revision = 0;
	REQUIRE(library.Find(owner, name, revision));
	// A rejected authoring publication discards its new cache entry; durable bytes remain loadable.
	CHECK(library.Remove(owner, name, revision));
	CHECK(std::filesystem::exists(path));
	Accepted(ReadArtifact(root, name, loaded));
	Accepted(library.Install(owner, name, loaded));
	const auto retained = loaded.Variants;
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		file << "corrupted";
	}
	CHECK(ReadArtifact(root, name, loaded));
	CHECK(loaded.Variants == retained);
	CHECK(ReadArtifact(root, engine::core::Name("../bad.ashader"), loaded));
}

TEST_CASE(
	"Composer immutable install validates encoded identity and preserves revision on reuse",
	"[render][cooked-composer]"
) {
	auto shader = Artifact();
	engine::core::ByteWriter bytes;
	REQUIRE(engine::assets::Shader::Write(bytes, shader));
	const engine::core::Name asset(engine::assets::Hasher::Of(bytes.Bytes()).ToHex() + ".ashader"),
		owner("immutable.owner");
	CookedComposerLibrary library;
	Accepted(library.Install(owner, asset, shader));
	uint64_t revision = 0;
	REQUIRE(library.Find(owner, asset, revision));
	CHECK(revision == 1);
	Accepted(library.Install(owner, asset, shader));
	REQUIRE(library.Find(owner, asset, revision));
	CHECK(revision == 1);
	shader.CompilerVersion = "different encoded provenance";
	CHECK(library.Install(owner, asset, shader));
	REQUIRE(library.Find(owner, asset, revision));
	CHECK(revision == 1);
}

TEST_CASE(
	"Composer operation admission retains previous pair under exact retained capacity refusal",
	"[render][cooked-composer]"
) {
	const Definition definition{
		"", "output.color=gm_BaseTextureObject.Sample(gm_BaseTexture,input.uv);", "", "", {}
	};
	engine::assets::ShaderData artifact;
	Accepted(CookArtifact(definition, {}, "fixture-provenance", false, artifact));
	CookedPair pair;
	Accepted(AdmitArtifact(artifact, pair));
	const auto accepted = pair;
	const auto inputBytes = engine::assets::ShaderRetainedPayloadBytes(artifact);
	const auto pairBytes = CookedPairRetainedBytes(pair);
	REQUIRE(inputBytes);
	REQUIRE(pairBytes);
	CHECK(AdmitArtifact(artifact, pair, *inputBytes + *pairBytes));
	CHECK(pair == accepted);
	artifact.CompilerVersion.reserve(1024 * 1024);
	CHECK(*engine::assets::ShaderRetainedPayloadBytes(artifact) > *inputBytes);
	CookedComposerLibrary library;
	const engine::core::Name owner("budget.owner"), name("budget.pair");
	CHECK(library.Install(owner, name, artifact, *inputBytes));
	uint64_t revision = 17;
	CHECK_FALSE(library.Find(owner, name, revision));
	CHECK(revision == 0);
	Accepted(library.Install(owner, name, artifact));
	REQUIRE(library.Find(owner, name, revision));
	const auto held = revision;
	CHECK(library.Install(owner, name, artifact, *inputBytes));
	REQUIRE(library.Find(owner, name, revision));
	CHECK(revision == held);
}

TEST_CASE(
	"Composer fixed paired vertex reflection uses source position and UV locations",
	"[render][cooked-composer]"
) {
	const auto artifact = Artifact();
	const auto vertex =
		std::find_if(artifact.Variants.begin(), artifact.Variants.end(), [](const auto &value) {
			return value.Stage == "vertex";
		});
	REQUIRE(vertex != artifact.Variants.end());
	REQUIRE(vertex->Inputs.size() == 2);
	CHECK(std::any_of(vertex->Inputs.begin(), vertex->Inputs.end(), [](const auto &input) {
		return input.Location == 0 && input.Type == "float3";
	}));
	CHECK(std::any_of(vertex->Inputs.begin(), vertex->Inputs.end(), [](const auto &input) {
		return input.Location == 2 && input.Type == "float2";
	}));
	CHECK_FALSE(std::any_of(vertex->Inputs.begin(), vertex->Inputs.end(), [](const auto &input) {
		return input.Location == 1;
	}));
}

TEST_CASE(
	"Composer reflected container refuses tight replacement budget before copying metadata",
	"[render][cooked-composer]"
) {
	const auto artifact = Artifact();
	CookedPair pair;
	Accepted(AdmitArtifact(artifact, pair));
	engine::assets::ShaderData output = artifact;
	const auto retained = engine::assets::ShaderRetainedPayloadBytes(output);
	REQUIRE(retained);
	CHECK(BuildContainer(pair.SpirV, "fixture-provenance", output, *retained));
	CHECK(output == artifact);
}

TEST_CASE(
	"Composer reflected descriptor names are bounded before metadata clones", "[render][cooked-composer]"
) {
	const auto artifact = Artifact();
	CookedPair pair;
	Accepted(AdmitArtifact(artifact, pair));
	const auto fragment =
		std::find_if(artifact.Variants.begin(), artifact.Variants.end(), [](const auto &variant) {
			return variant.Stage == "fragment";
		});
	REQUIRE(fragment != artifact.Variants.end());
	const auto block =
		std::find_if(fragment->Resources.begin(), fragment->Resources.end(), [](const auto &resource) {
			return resource.Kind == "uniform-buffer";
		});
	REQUIRE(block != fragment->Resources.end());
	const std::string tooLong(engine::assets::Shader::MAXIMUM_NAME + 1, 'x');
	const auto originalWords = pair.SpirV.Fragment.SpirV;
	std::vector<uint32_t> rewritten(originalWords.begin(), originalWords.begin() + 5);
	bool renamed = false;
	for (size_t at = 5; at < originalWords.size();) {
		const uint32_t count = originalWords[at] >> 16, opcode = originalWords[at] & 65535;
		REQUIRE(count != 0);
		REQUIRE(count <= originalWords.size() - at);
		std::string name;
		if (opcode == 5 && count >= 3) {
			for (size_t word = at + 2; word < at + count; ++word) {
				for (size_t byte = 0; byte < 4; ++byte) {
					const char character = char((originalWords[word] >> (8 * byte)) & 255);
					if (character == 0) break;
					name.push_back(character);
				}
			}
		}
		if (opcode == 5 && name == block->Name) {
			const size_t words = 2 + (tooLong.size() + 4) / 4;
			std::vector<uint32_t> instruction(words, 0);
			instruction[0] = (uint32_t(words) << 16) | 5;
			instruction[1] = originalWords[at + 1];
			for (size_t byte = 0; byte < tooLong.size(); ++byte)
				instruction[2 + byte / 4] |= uint32_t(uint8_t(tooLong[byte])) << (8 * (byte % 4));
			rewritten.insert(rewritten.end(), instruction.begin(), instruction.end());
			renamed = true;
		} else
			rewritten.insert(rewritten.end(), originalWords.begin() + at, originalWords.begin() + at + count);
		at += count;
	}
	REQUIRE(renamed);
	pair.SpirV.Fragment.SpirV = std::move(rewritten);
	Accepted(Admit(pair.SpirV));
	engine::assets::ShaderData retained = artifact;
	CHECK(BuildContainer(pair.SpirV, "fixture-provenance", retained));
	CHECK(retained == artifact);
}
TEST_CASE(
	"Composer artifact cooking honors caller allowance before compiler copies", "[render][cooked-composer]"
) {
	auto output = Artifact();
	const auto original = output;
	const auto retained = engine::assets::ShaderRetainedPayloadBytes(output);
	REQUIRE(retained);
	const auto failure = CookArtifact(
		{"", "invalid HLSL", "", "", {}}, {}, "fixture-provenance", false, output, {}, *retained
	);
	REQUIRE(failure);
	CHECK(failure->find("operation budget") != std::string::npos);
	CHECK(output == original);
	Source source;
	const auto assembly = Assemble({"", MAIN, "", "", {}}, {}, source, sizeof(Source));
	REQUIRE(assembly);
	CHECK(source.Vertex.empty());
	CHECK(source.Fragment.empty());
}
TEST_CASE(
	"Composer library admission includes other retained owners and entry growth", "[render][cooked-composer]"
) {
	const auto data = Artifact();
	// Determine this artifact's exact independent admission floor, including owned reflected metadata.
	uint64_t lower = 0, upper = MAXIMUM_COOKED_OPERATION_BYTES;
	while (lower < upper) {
		const uint64_t middle = lower + (upper - lower) / 2;
		CookedPair trial;
		if (AdmitArtifact(data, trial, middle))
			lower = middle + 1;
		else
			upper = middle;
	}
	CookedPair minimumPair;
	Accepted(AdmitArtifact(data, minimumPair, lower));
	CookedComposerLibrary library;
	const engine::core::Name firstOwner("retained.first"), secondOwner("retained.second"), first("first"),
		second("second");
	Accepted(library.Install(firstOwner, first, data));
	const auto held = library.RetainedBytes();
	REQUIRE(held);
	CHECK(library.Install(secondOwner, second, data, lower + *held - 1));
	uint64_t revision = 0;
	REQUIRE(library.Find(firstOwner, first, revision));
	CHECK(revision == 1);
	CHECK_FALSE(library.Find(secondOwner, second, revision));
	CHECK(revision == 0);
	CHECK(library.RetainedBytes() == held);
	Accepted(library.Install(secondOwner, second, data, lower + *held));
	REQUIRE(library.Find(secondOwner, second, revision));
	CHECK(revision == 1);
	const auto both = library.RetainedBytes();
	REQUIRE(both);
	CHECK(*both > *held);
	CHECK(library.RetainedBytes(*both) == both);
	CHECK_FALSE(library.RetainedBytes(*both - 1));
	library.RemoveOwner(firstOwner);
	CHECK(*library.RetainedBytes() < *both);
}
TEST_CASE(
	"Composer library bounds owner metadata independently of logical payload", "[render][cooked-composer]"
) {
	const auto data = Artifact();
	CookedComposerLibrary library;
	const engine::core::Name shader("shared.artifact");
	for (size_t index = 0; index < MAXIMUM_LIBRARY_OWNERS; ++index)
		Accepted(library.Install(engine::core::Name("library.owner." + std::to_string(index)), shader, data));
	const auto held = library.RetainedBytes();
	CHECK(library.Install(engine::core::Name("library.owner.excess"), shader, data));
	CHECK(library.RetainedBytes() == held);
	uint64_t revision = 0;
	CHECK_FALSE(library.Find(engine::core::Name("library.owner.excess"), shader, revision));
	CHECK(revision == 0);
}
