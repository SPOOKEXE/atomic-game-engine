#include <engine/procedural/Artifact.hpp>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>

namespace {

	using namespace engine::procedural;
	constexpr size_t MAXIMUM_INPUT_BYTES = 1048576;

	void Require(bool condition) {
		if (!condition) std::abort();
	}

	Recipe RecipeSeed() {
		Recipe recipe;
		recipe.Name = "fuzz/recipe";
		recipe.Generator = "fuzz/generator";
		recipe.SchemaVersion = 1;
		recipe.RootSeed = std::numeric_limits<uint64_t>::max();
		recipe.Parameters = {
			{"bool", true},
			{"signed", std::numeric_limits<int64_t>::min()},
			{"unsigned", std::numeric_limits<uint64_t>::max()},
			{"real", 1.25},
			{"two", Vector2{{1.5, -2.5}}},
			{"three", Vector3{{1, 2, 3}}},
			{"four", Vector4{{-1, -2, -3, -4}}},
			{"text", StableText{"forest/\xc3\xa9"}},
			{"bytes", ByteBuffer{{0, 255, 1, 128}}}
		};
		recipe.Nodes = {{"a", "source", {{"value", int64_t{7}}}}, {"b", "sink", {}}};
		recipe.Links = {{"a", "out", "b", "in"}};
		return recipe;
	}

	RegionResult ArtifactSeed() {
		RegionResult result;
		result.KeyBytes = {'R', 'G', 'K', '2', 0xff, 0xff, 0xff, 0xff, 2, 0, 0, 0, 16, 0};
		result.Placements = {
			{"tree/a", PlacementKind::Tree, "", {-12, 25}, 3, 2, 1},
			{"lot/b", PlacementKind::VillageLot, "town", {8, -9}, 1, 0, 1},
			{"house/c", PlacementKind::House, "town", {10, 12}, 2, 3, 4},
			{"entrance/d", PlacementKind::Entrance, "west", {1, 2}, 0, 0, 0}
		};
		result.Roads = {{"road/west", "west", "town", {{{1, 2}, {3, -4}, {5, 6}}}}};
		result.Facts = {3, 4, 5, 6};
		return result;
	}

	void CheckRecipe(std::span<const uint8_t> bytes) {
		Recipe parsed = RecipeSeed();
		Diagnostic diagnostic;
		std::vector<uint8_t> previous;
		Require(EncodeRecipe(parsed, previous, diagnostic) == Status::Ok);
		const bool accepted = DecodeRecipe(bytes, parsed, diagnostic) == Status::Ok;
		std::vector<uint8_t> canonical;
		Require(EncodeRecipe(parsed, canonical, diagnostic) == Status::Ok);
		if (!accepted) {
			Require(canonical == previous);
			return;
		}
		Require(std::equal(canonical.begin(), canonical.end(), bytes.begin(), bytes.end()));
		Recipe again;
		Require(DecodeRecipe(canonical, again, diagnostic) == Status::Ok);
		std::vector<uint8_t> repeated;
		Require(EncodeRecipe(again, repeated, diagnostic) == Status::Ok && repeated == canonical);
	}

	void CheckArtifact(std::span<const uint8_t> bytes) {
		RegionResult parsed = ArtifactSeed();
		Diagnostic diagnostic;
		std::vector<uint8_t> previous;
		Require(EncodeRegionResult(parsed, previous, diagnostic) == Status::Ok);
		const bool accepted = DecodeRegionResult(bytes, parsed, diagnostic) == Status::Ok;
		std::vector<uint8_t> canonical;
		Require(EncodeRegionResult(parsed, canonical, diagnostic) == Status::Ok);
		if (!accepted) {
			Require(canonical == previous);
			Require(
				parsed.Facts.CandidateCells == 3 && parsed.Facts.WorkUnits == 4 &&
				parsed.Facts.SpatialTests == 5 && parsed.Facts.OutputBytes == 6
			);
			return;
		}
		Require(std::equal(canonical.begin(), canonical.end(), bytes.begin(), bytes.end()));
		Require(
			parsed.Facts.CandidateCells == 0 && parsed.Facts.WorkUnits == 0 &&
			parsed.Facts.SpatialTests == 0 && parsed.Facts.OutputBytes == 0
		);
		RegionResult again;
		Require(DecodeRegionResult(canonical, again, diagnostic, parsed.KeyBytes) == Status::Ok);
		std::vector<uint8_t> repeated;
		Require(EncodeRegionResult(again, repeated, diagnostic) == Status::Ok && repeated == canonical);
		// Expected-key mismatches must not replace an already accepted artifact.
		auto staleKey = parsed.KeyBytes;
		if (staleKey.size() == 4)
			staleKey.push_back(1);
		else
			staleKey.back() ^= 1;
		Require(DecodeRegionResult(canonical, again, diagnostic, staleKey) == Status::StaleArtifact);
		Require(EncodeRegionResult(again, repeated, diagnostic) == Status::Ok && repeated == canonical);
	}

	void CheckAuthored(std::span<const uint8_t> bytes) {
		Diagnostic diagnostic;
		AuthoredSpline spline{"bend", "west", {-1024, 2048}};
		std::vector<uint8_t> previous;
		Require(EncodeAuthoredSpline(spline, previous, diagnostic) == Status::Ok);
		const bool splineAccepted = DecodeAuthoredSpline(bytes, spline, diagnostic) == Status::Ok;
		std::vector<uint8_t> canonical;
		Require(EncodeAuthoredSpline(spline, canonical, diagnostic) == Status::Ok);
		Require(
			splineAccepted ? std::equal(canonical.begin(), canonical.end(), bytes.begin(), bytes.end())
						   : canonical == previous
		);
		if (splineAccepted) {
			AuthoredSpline again;
			Require(DecodeAuthoredSpline(canonical, again, diagnostic) == Status::Ok);
			Require(EncodeAuthoredSpline(again, previous, diagnostic) == Status::Ok && previous == canonical);
		}
		PlacementOverride edit{"tree/-1/2", OverrideAction::Replace, {-512, 1024}, 3, 2, 1, ""};
		Require(EncodePlacementOverride(edit, previous, diagnostic) == Status::Ok);
		const bool editAccepted = DecodePlacementOverride(bytes, edit, diagnostic) == Status::Ok;
		Require(EncodePlacementOverride(edit, canonical, diagnostic) == Status::Ok);
		Require(
			editAccepted ? std::equal(canonical.begin(), canonical.end(), bytes.begin(), bytes.end())
						 : canonical == previous
		);
		if (editAccepted) {
			PlacementOverride again;
			Require(DecodePlacementOverride(canonical, again, diagnostic) == Status::Ok);
			Require(
				EncodePlacementOverride(again, previous, diagnostic) == Status::Ok && previous == canonical
			);
		}
	}

	void CheckScope(std::span<const uint8_t> bytes) {
		RegionScope scope{-123, 456, 32};
		if (!ReadRegionKeyScope(bytes, scope)) {
			Require(scope.X == -123 && scope.Y == 456 && scope.CellsPerSide == 32);
			return;
		}
		// RGK2 is a scope-header reader, not a decoder for the opaque witnesses.
		Require(bytes.size() >= 14 && bytes.size() <= 262144);
		Require(scope.CellsPerSide > 0 && scope.CellsPerSide <= 64);
		const auto read32 = [&](size_t offset) {
			return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
				   (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
				   (static_cast<uint32_t>(bytes[offset + 3]) << 24);
		};
		Require(scope.X == std::bit_cast<int32_t>(read32(4)) && scope.Y == std::bit_cast<int32_t>(read32(8)));
		Require(
			scope.CellsPerSide == (static_cast<uint16_t>(bytes[12]) | (static_cast<uint16_t>(bytes[13]) << 8))
		);
	}

	void Check(std::span<const uint8_t> bytes) {
		CheckRecipe(bytes);
		CheckArtifact(bytes);
		CheckAuthored(bytes);
		CheckScope(bytes);
	}

	void WriteSeed(const std::filesystem::path &directory, std::string name, std::vector<uint8_t> bytes) {
		Diagnostic diagnostic;
		Require(bytes.size() >= 4);
		const std::string_view magic(reinterpret_cast<const char *>(bytes.data()), 4);
		if (magic == "REC1") {
			Recipe decoded;
			Require(DecodeRecipe(bytes, decoded, diagnostic) == Status::Ok);
		} else if (magic == "RGA1") {
			RegionResult decoded;
			Require(DecodeRegionResult(bytes, decoded, diagnostic) == Status::Ok);
		} else if (magic == "SPL1") {
			AuthoredSpline decoded;
			Require(DecodeAuthoredSpline(bytes, decoded, diagnostic) == Status::Ok);
		} else if (magic == "OVR1") {
			PlacementOverride decoded;
			Require(DecodePlacementOverride(bytes, decoded, diagnostic) == Status::Ok);
		} else {
			RegionScope decoded;
			Require(magic == "RGK2" && ReadRegionKeyScope(bytes, decoded));
		}
		Check(bytes);
		const auto write = [&](std::string_view suffix, const auto &payload) {
			std::ofstream file(directory / (name + std::string(suffix)), std::ios::binary);
			file.write(
				reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size())
			);
			Require(file.good());
		};
		write(".valid", bytes);
		if (bytes.empty()) return;
		auto malformed = bytes;
		malformed[0] ^= 0xff;
		write(".magic", malformed);
		malformed = bytes;
		malformed.pop_back();
		write(".truncated", malformed);
		malformed = bytes;
		malformed.push_back(0xff);
		write(".trailing", malformed);
		if (bytes.size() >= 8) {
			malformed = bytes;
			std::fill(malformed.begin() + 4, malformed.begin() + 8, 0xff);
			write(".length", malformed);
		}
		if (magic == "REC1") {
			const std::array<uint8_t, 5> realTag{3, 0, 'f', '6', '4'};
			const auto found = std::search(bytes.begin(), bytes.end(), realTag.begin(), realTag.end());
			if (found != bytes.end()) {
				const size_t tag = static_cast<size_t>(found - bytes.begin());
				Require(tag + realTag.size() + 8 <= bytes.size());
				malformed = bytes;
				malformed[tag + 2] = 'x';
				write(".unknown-type", malformed);
				for (const auto &[suffix, bits] :
					 {std::pair{".nonfinite", uint64_t{0x7ff0000000000000}},
					  std::pair{".negative-zero", uint64_t{0x8000000000000000}}}) {
					malformed = bytes;
					for (size_t part = 0; part < 8; ++part)
						malformed[tag + realTag.size() + part] = static_cast<uint8_t>(bits >> (part * 8));
					write(suffix, malformed);
				}
			}
		}
	}
}

extern "C" int LLVMFuzzerInitialize(int *argumentCount, char ***arguments) {
	if (*argumentCount != 3 || std::string_view((*arguments)[1]) != "--write-seeds") return 0;
	const std::filesystem::path directory((*arguments)[2]);
	std::filesystem::create_directories(directory);
	Diagnostic diagnostic;
	std::vector<uint8_t> bytes;
	Require(EncodeRecipe(RecipeSeed(), bytes, diagnostic) == Status::Ok);
	WriteSeed(directory, "recipe-all-types", bytes);
	Require(EncodeRecipe(ForestVillageDefaults("fuzz/forest", 17), bytes, diagnostic) == Status::Ok);
	WriteSeed(directory, "recipe-forest", bytes);
	const auto artifact = ArtifactSeed();
	Require(EncodeRegionResult(artifact, bytes, diagnostic) == Status::Ok);
	WriteSeed(directory, "artifact-mixed", bytes);
	RegionResult empty;
	empty.KeyBytes = artifact.KeyBytes;
	Require(EncodeRegionResult(empty, bytes, diagnostic) == Status::Ok);
	WriteSeed(directory, "artifact-empty", bytes);
	WriteSeed(directory, "scope", artifact.KeyBytes);
	Require(EncodeAuthoredSpline({"bend", "west", {-1024, 2048}}, bytes, diagnostic) == Status::Ok);
	WriteSeed(directory, "spline", bytes);
	for (const auto &edit : std::vector<PlacementOverride>{
			 {"tree/-1/2", OverrideAction::Delete, {}, 0, 0, 0, ""},
			 {"tree/-1/2", OverrideAction::Replace, {-512, 1024}, 3, 2, 1, ""},
			 {"house/1/-2", OverrideAction::Replace, {1024, -2048}, 2, 1, 0, "town"}
		 }) {
		Require(EncodePlacementOverride(edit, bytes, diagnostic) == Status::Ok);
		WriteSeed(
			directory,
			"override-" + std::to_string(static_cast<unsigned>(edit.Action)) +
				(edit.SourceName.empty() ? "-tree" : "-house"),
			bytes
		);
	}
	std::exit(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size) {
	if (size <= MAXIMUM_INPUT_BYTES + 1) Check(std::span(input, size));
	return 0;
}
