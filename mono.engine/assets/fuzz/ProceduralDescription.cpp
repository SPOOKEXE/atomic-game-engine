#include <engine/assets/ProceduralDescription.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>

namespace {
	using namespace engine::assets;

	void Require(bool condition) {
		if (!condition) std::abort();
	}

	ProceduralPlacement Seed(std::string id, std::string kind) {
		return {
			std::move(id),
			std::move(kind),
			"procedural/" + std::string(64, 'a') + ".amesh",
			{-1024, 2048},
			2,
			3,
			{1.5f, -2, 3},
			{4, 5, 6},
			{0.25f, 0.5f, 0.75f, 1}
		};
	}

	void Check(std::span<const std::byte> bytes) {
		std::vector<ProceduralPlacement> parsed{Seed("unchanged", "tree")};
		std::string failure;
		std::vector<std::byte> previous;
		Require(EncodeProceduralDescription(parsed, previous, failure));
		const bool accepted = DecodeProceduralDescription(bytes, parsed, failure);
		std::vector<std::byte> canonical;
		Require(EncodeProceduralDescription(parsed, canonical, failure));
		if (!accepted) {
			Require(canonical == previous);
			return;
		}
		Require(std::equal(canonical.begin(), canonical.end(), bytes.begin(), bytes.end()));
		std::vector<ProceduralPlacement> again;
		Require(DecodeProceduralDescription(canonical, again, failure));
		std::vector<std::byte> repeated;
		Require(EncodeProceduralDescription(again, repeated, failure) && repeated == canonical);
	}

	void
	Write(const std::filesystem::path &directory, std::string_view name, std::span<const std::byte> bytes) {
		std::ofstream file(directory / name, std::ios::binary);
		file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		Require(file.good());
	}

	void WriteSeeds(
		const std::filesystem::path &directory,
		std::string name,
		const std::vector<ProceduralPlacement> &placements
	) {
		std::string failure;
		std::vector<std::byte> bytes;
		Require(EncodeProceduralDescription(placements, bytes, failure));
		std::vector<ProceduralPlacement> decoded;
		Require(DecodeProceduralDescription(bytes, decoded, failure));
		Check(bytes);
		Write(directory, name + ".valid", bytes);
		auto malformed = bytes;
		malformed[0] ^= std::byte{0xff};
		Write(directory, name + ".magic", malformed);
		malformed = bytes;
		malformed.pop_back();
		Write(directory, name + ".truncated", malformed);
		malformed = bytes;
		malformed.push_back(std::byte{0xff});
		Write(directory, name + ".trailing", malformed);
		malformed = bytes;
		std::fill(malformed.begin() + 4, malformed.begin() + 8, std::byte{0xff});
		Write(directory, name + ".count", malformed);
		if (placements.empty()) return;
		malformed = bytes;
		std::fill(malformed.begin() + 8, malformed.begin() + 12, std::byte{0xff});
		Write(directory, name + ".name-length", malformed);
		const auto &first = placements.front();
		const size_t centre = 8 + 12 + first.Id.size() + first.Kind.size() + first.MeshName.size() + 20;
		for (const auto &[suffix, bits] :
			 {std::pair{".nonfinite", uint32_t{0x7f800000}},
			  std::pair{".negative-zero", uint32_t{0x80000000}}}) {
			malformed = bytes;
			for (size_t part = 0; part < 4; ++part)
				malformed[centre + part] = static_cast<std::byte>(bits >> (part * 8));
			Write(directory, name + suffix, malformed);
		}
	}
}

extern "C" int LLVMFuzzerInitialize(int *argumentCount, char ***arguments) {
	if (*argumentCount != 3 || std::string_view((*arguments)[1]) != "--write-seeds") return 0;
	const std::filesystem::path directory((*arguments)[2]);
	std::filesystem::create_directories(directory);
	WriteSeeds(directory, "empty", {});
	WriteSeeds(
		directory,
		"mixed",
		{Seed("a", "tree"),
		 Seed("b", "village-lot"),
		 Seed("c", "house"),
		 Seed("d", "entrance"),
		 Seed("e", "road")}
	);
	auto extreme = Seed(std::string(128, 'z'), "road");
	extreme.Origin = {-(int64_t{1} << 47), int64_t{1} << 47};
	WriteSeeds(directory, "bounds", {extreme});
	std::exit(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size) {
	if (size <= MAXIMUM_PROCEDURAL_DESCRIPTION_BYTES + 1) Check(std::as_bytes(std::span(input, size)));
	return 0;
}
