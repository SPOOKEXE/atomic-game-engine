#include "ProceduralState.hpp"

#include <engine/ecs/Components.hpp>
#include <engine/procedural/Artifact.hpp>
#include <engine/script/BakedProceduralReference.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
	using namespace engine;
	using namespace engine::script;
	namespace state = engine::script::procedural_detail;
	constexpr size_t MAXIMUM_INPUT_BYTES = 1048576;

	void Require(bool condition) {
		if (!condition) std::abort();
	}

	std::vector<uint8_t> Bytes(const core::ByteWriter &writer) {
		const auto bytes = writer.Bytes();
		const auto *first = reinterpret_cast<const uint8_t *>(bytes.data());
		return {first, first + bytes.size()};
	}

	ProceduralAuthority AuthoritySeed() {
		ProceduralAuthority authority;
		authority.Owner = "fuzz/world";
		authority.Incarnation = 29;
		procedural::Diagnostic diagnostic;
		Require(
			procedural::EncodeRecipe(
				procedural::ForestVillageDefaults("fuzz/forest", 17), authority.RecipeBytes, diagnostic
			) == procedural::Status::Ok
		);
		authority.Sources = {
			{procedural::SourceKind::Village, "town", {0, 0}, ""},
			{procedural::SourceKind::Entrance, "west", {-4096, 0}, "town"}
		};
		authority.Splines = {{"bend", "west", {-2048, 1024}}};
		authority.Overrides = {
			{"house/1/1", procedural::OverrideAction::Replace, {1024, 1024}, 1, 0, 0, "town"},
			{"tree/-1/2", procedural::OverrideAction::Delete, {}, 0, 0, 0, ""}
		};
		authority.Regions = {{{-1, 0, 16}, 37}, {{0, 0, 16}, 41}};
		return authority;
	}

	std::vector<uint8_t> ArtifactSeed() {
		procedural::RegionResult region;
		region.KeyBytes = {'R', 'G', 'K', '2', 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 16, 0};
		region.Placements = {{"tree/-1/2", procedural::PlacementKind::Tree, "", {-1024, 2048}, 1, 0, 0}};
		region.Roads = {{"road/west", "west", "town", {{{-4096, 0}, {-2048, 1024}, {0, 0}}}}};
		std::vector<uint8_t> bytes;
		procedural::Diagnostic diagnostic;
		Require(procedural::EncodeRegionResult(region, bytes, diagnostic) == procedural::Status::Ok);
		return bytes;
	}

	ProceduralPublished PublishedSeed() {
		ProceduralPublished published;
		ProceduralPublication publication;
		publication.Scope = {-1, 0, 16};
		publication.ArtifactBytes = ArtifactSeed();
		procedural::RegionResult region;
		procedural::Diagnostic diagnostic;
		Require(
			procedural::DecodeRegionResult(publication.ArtifactBytes, region, diagnostic) ==
			procedural::Status::Ok
		);
		publication.GenerationKey = state::Hash(region.KeyBytes);
		publication.ArtifactDigest = state::Hash(publication.ArtifactBytes);
		publication.AcceptedRevision = 7;
		publication.ActivationTick = 123;
		published.Regions.push_back(std::move(publication));
		return published;
	}

	void CheckAuthority(std::span<const uint8_t> bytes) {
		static const auto sentinel = AuthoritySeed();
		auto parsed = sentinel;
		procedural::Diagnostic diagnostic;
		std::vector<uint8_t> previous;
		Require(state::WriteAuthority(parsed, previous, diagnostic));
		const bool accepted = state::ReadAuthority(bytes, parsed, diagnostic);
		std::vector<uint8_t> canonical;
		Require(state::WriteAuthority(parsed, canonical, diagnostic));
		if (!accepted) {
			Require(canonical == previous && parsed.Incarnation == sentinel.Incarnation);
			Require(parsed.Regions.size() == sentinel.Regions.size());
			for (size_t i = 0; i < parsed.Regions.size(); ++i)
				Require(parsed.Regions[i].SourceRevision == sentinel.Regions[i].SourceRevision);
			return;
		}
		Require(std::equal(canonical.begin(), canonical.end(), bytes.begin(), bytes.end()));
		Require(parsed.Incarnation != 0);
		for (const auto &region : parsed.Regions)
			Require(region.SourceRevision == 1);
		ProceduralAuthority again;
		Require(state::ReadAuthority(canonical, again, diagnostic));
		std::vector<uint8_t> repeated;
		Require(state::WriteAuthority(again, repeated, diagnostic) && repeated == canonical);
	}

	void CheckPublished(std::span<const uint8_t> bytes) {
		static const auto sentinel = PublishedSeed();
		auto parsed = sentinel;
		procedural::Diagnostic diagnostic;
		std::vector<uint8_t> previous;
		Require(state::WritePublished(parsed, previous, diagnostic));
		const bool accepted = state::ReadPublished(bytes, parsed, diagnostic);
		std::vector<uint8_t> canonical;
		Require(state::WritePublished(parsed, canonical, diagnostic));
		if (!accepted) {
			Require(canonical == previous);
			return;
		}
		Require(std::equal(canonical.begin(), canonical.end(), bytes.begin(), bytes.end()));
		ProceduralPublished again;
		Require(state::ReadPublished(canonical, again, diagnostic));
		std::vector<uint8_t> repeated;
		Require(state::WritePublished(again, repeated, diagnostic) && repeated == canonical);
	}

	template <class T> void CheckSnapshotRow(std::span<const uint8_t> bytes, const T &sentinel) {
		const auto &codec = ecs::Components::Describe(ecs::Components::Assigned<T>());
		Require(codec.Read && codec.Write);
		auto parsed = sentinel;
		core::ByteWriter previous;
		codec.Write(previous, &parsed, 1);
		core::ByteReader reader(std::as_bytes(bytes));
		codec.Read(reader, &parsed, 1);
		core::ByteWriter canonical;
		codec.Write(canonical, &parsed, 1);
		if (reader.Failed()) {
			Require(
				std::equal(
					canonical.Bytes().begin(),
					canonical.Bytes().end(),
					previous.Bytes().begin(),
					previous.Bytes().end()
				)
			);
			if constexpr (std::is_same_v<T, ProceduralAuthority>) {
				Require(
					parsed.Incarnation == sentinel.Incarnation &&
					parsed.Regions.size() == sentinel.Regions.size()
				);
				for (size_t i = 0; i < parsed.Regions.size(); ++i)
					Require(parsed.Regions[i].SourceRevision == sentinel.Regions[i].SourceRevision);
			}
			return;
		}
		// Snapshot rows are prefix readers because later rows share the stream.
		const auto consumed = std::as_bytes(bytes).first(bytes.size() - reader.Remaining());
		Require(
			std::equal(canonical.Bytes().begin(), canonical.Bytes().end(), consumed.begin(), consumed.end())
		);
		T again;
		core::ByteReader second(canonical.Bytes());
		codec.Read(second, &again, 1);
		Require(!second.Failed() && second.AtEnd());
		core::ByteWriter repeated;
		codec.Write(repeated, &again, 1);
		Require(
			std::equal(
				canonical.Bytes().begin(),
				canonical.Bytes().end(),
				repeated.Bytes().begin(),
				repeated.Bytes().end()
			)
		);
	}

	// Recompute the real envelope hashes so mutations reach RGA1 validation,
	// rather than always stopping at PWP1's outer artifact digest check.
	void CheckRefreshedPublication(std::span<const uint8_t> artifact) {
		if (artifact.size() < 8) return;
		const size_t keySize =
			static_cast<uint32_t>(artifact[4]) | (static_cast<uint32_t>(artifact[5]) << 8) |
			(static_cast<uint32_t>(artifact[6]) << 16) | (static_cast<uint32_t>(artifact[7]) << 24);
		if (keySize > 262144 || keySize > artifact.size() - 8) return;
		const auto key = artifact.subspan(8, keySize);
		procedural::RegionScope scope;
		if (!procedural::ReadRegionKeyScope(key, scope)) return;
		core::ByteWriter writer(100 + artifact.size(), 100 + MAXIMUM_INPUT_BYTES + 1);
		writer.WriteUInt32(0x31505750);
		writer.WriteUInt16(1);
		writer.WriteInt32(scope.X);
		writer.WriteInt32(scope.Y);
		writer.WriteUInt16(scope.CellsPerSide);
		const auto keyHash = state::Hash(key);
		const auto artifactHash = state::Hash(artifact);
		writer.WriteRaw(keyHash.Bytes.data(), keyHash.Bytes.size());
		writer.WriteRaw(artifactHash.Bytes.data(), artifactHash.Bytes.size());
		writer.WriteUInt64(1);
		writer.WriteUInt64(0);
		writer.WriteUInt32(static_cast<uint32_t>(artifact.size()));
		writer.WriteRaw(artifact.data(), artifact.size());
		CheckPublished(Bytes(writer));
	}

	BakedProceduralReference ReferenceSeed() {
		return {core::Name("procedural/fuzz.aproc"), {-1, 2, 16}, ecs::Entity(7), 11};
	}

	void Check(std::span<const uint8_t> bytes) {
		CheckAuthority(bytes);
		CheckPublished(bytes);
		static const auto authority = AuthoritySeed();
		static const auto reference = ReferenceSeed();
		CheckSnapshotRow(bytes, authority);
		CheckSnapshotRow(bytes, reference);
		CheckRefreshedPublication(bytes);
	}

	void
	Write(const std::filesystem::path &directory, std::string_view name, std::span<const uint8_t> bytes) {
		std::ofstream file(directory / name, std::ios::binary);
		file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		Require(file.good());
	}

	void
	WriteSeeds(const std::filesystem::path &directory, std::string name, const std::vector<uint8_t> &bytes) {
		Check(bytes);
		Write(directory, name + ".valid", bytes);
		if (bytes.empty()) return;
		auto malformed = bytes;
		malformed[0] ^= 0xff;
		Write(directory, name + ".magic", malformed);
		malformed = bytes;
		malformed.pop_back();
		Write(directory, name + ".truncated", malformed);
		malformed = bytes;
		malformed.push_back(0xff);
		Write(directory, name + ".trailing", malformed);
		if (bytes.size() >= 8) {
			malformed = bytes;
			std::fill(malformed.begin() + 4, malformed.begin() + 8, 0xff);
			Write(directory, name + ".length", malformed);
		}
	}

	template <class T> std::vector<uint8_t> SnapshotBytes(const T &value) {
		const auto &codec = ecs::Components::Describe(ecs::Components::Assigned<T>());
		core::ByteWriter writer;
		codec.Write(writer, &value, 1);
		T decoded;
		core::ByteReader reader(writer.Bytes());
		codec.Read(reader, &decoded, 1);
		Require(!reader.Failed() && reader.AtEnd());
		return Bytes(writer);
	}
}

extern "C" int LLVMFuzzerInitialize(int *argumentCount, char ***arguments) {
	RegisterProceduralComponents();
	if (*argumentCount != 3 || std::string_view((*arguments)[1]) != "--write-seeds") return 0;
	const std::filesystem::path directory((*arguments)[2]);
	std::filesystem::create_directories(directory);
	procedural::Diagnostic diagnostic;
	std::vector<uint8_t> bytes;
	const auto authority = AuthoritySeed();
	Require(state::WriteAuthority(authority, bytes, diagnostic));
	ProceduralAuthority decodedAuthority;
	Require(state::ReadAuthority(bytes, decodedAuthority, diagnostic));
	WriteSeeds(directory, "authority", bytes);
	WriteSeeds(directory, "authority-row", SnapshotBytes(authority));
	const ProceduralAuthority unconfigured;
	WriteSeeds(directory, "authority-default-row", SnapshotBytes(unconfigured));
	Require(!state::ReadAuthority({}, decodedAuthority, diagnostic));
	const auto published = PublishedSeed();
	Require(state::WritePublished(published, bytes, diagnostic));
	ProceduralPublished decodedPublished;
	Require(state::ReadPublished(bytes, decodedPublished, diagnostic));
	WriteSeeds(directory, "published", bytes);
	WriteSeeds(directory, "artifact-refresh", ArtifactSeed());
	WriteSeeds(directory, "reference-row", SnapshotBytes(ReferenceSeed()));
	WriteSeeds(directory, "reference-default-row", SnapshotBytes(BakedProceduralReference{}));
	// A default row with only one nondefault field is never a valid demand.
	for (const auto &partial :
		 {BakedProceduralReference{{}, {1, 0, 16}, {}, 0},
		  BakedProceduralReference{{}, {}, ecs::Entity(1), 0},
		  BakedProceduralReference{{}, {}, {}, 1}}) {
		const auto &codec = ecs::Components::Describe(ecs::Components::Assigned<BakedProceduralReference>());
		core::ByteWriter writer;
		codec.Write(writer, &partial, 1);
		auto lastGood = ReferenceSeed();
		core::ByteReader reader(writer.Bytes());
		codec.Read(reader, &lastGood, 1);
		Require(
			reader.Failed() && lastGood.Description == ReferenceSeed().Description && lastGood.Revision == 11
		);
		WriteSeeds(
			directory,
			"reference-partial-" + std::to_string(partial.Scope.X) + "-" + std::to_string(partial.Part.Id) +
				"-" + std::to_string(partial.Revision),
			Bytes(writer)
		);
	}
	std::exit(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size) {
	if (size <= MAXIMUM_INPUT_BYTES + 1) Check(std::span(input, size));
	return 0;
}
