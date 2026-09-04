#include <engine/assets/ContentHash.hpp>
#include <engine/delivery/Uploader.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.delivery.uploader")
TEST_DEPENDS("engine.assets.contenthash")
TEST_DEPENDS("engine.delivery.source")

using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::delivery::DeliverySettings;
using engine::delivery::MakeUploader;
using engine::delivery::Source;
using engine::delivery::SourceKind;
using engine::delivery::SourceRole;
using engine::delivery::Uploader;

namespace {
	namespace fs = std::filesystem;

	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> bytes(text.size());
		std::memcpy(bytes.data(), text.data(), text.size());
		return bytes;
	}

	void Write(const fs::path &path, const std::vector<std::byte> &bytes) {
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		REQUIRE(file);
		if (!bytes.empty()) {
			file.write(
				reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())
			);
		}
		REQUIRE(file.good());
	}

	std::vector<std::byte> Read(const fs::path &path) {
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		REQUIRE(file);
		const std::streamoff size = file.tellg();
		REQUIRE(size >= 0);
		std::vector<std::byte> bytes(static_cast<size_t>(size));
		file.seekg(0);
		file.read(reinterpret_cast<char *>(bytes.data()), size);
		REQUIRE(file.good());
		return bytes;
	}

	struct TemporaryDirectory {
		fs::path Root;

		explicit TemporaryDirectory(std::string_view name) {
			Root = fs::temp_directory_path() / ("atomic-uploader-" + std::string(name));
			std::error_code failure;
			fs::remove_all(Root, failure);
			REQUIRE_FALSE(failure);
			REQUIRE(fs::create_directories(Root, failure));
			REQUIRE_FALSE(failure);
		}

		~TemporaryDirectory() {
			std::error_code ignored;
			fs::remove_all(Root, ignored);
		}
	};

	Source Directory(std::string name, const fs::path &path) {
		return Source{
			.Name = std::move(name),
			.Kind = SourceKind::Directory,
			.Location = path.string(),
			.Enabled = true,
			.Role = SourceRole::Write,
		};
	}

	std::unique_ptr<Uploader> LocalUploader(std::vector<Source> sources) {
		DeliverySettings settings;
		settings.Sources = std::move(sources);
		return MakeUploader(settings);
	}
}

TEST_CASE("an uploader is absent when no source accepts writes", "[delivery][uploader]") {
	TemporaryDirectory directory("no-writes");
	Source source = Directory("read-only", directory.Root);
	source.Role = SourceRole::Read;

	CHECK_FALSE(LocalUploader({source}));
}

TEST_CASE("unreadable and empty uploads finish as local failures", "[delivery][uploader]") {
	TemporaryDirectory directory("local-failures");
	std::unique_ptr<Uploader> uploader = LocalUploader({Directory("local", directory.Root / "store")});
	REQUIRE(uploader);

	const fs::path missing = directory.Root / "missing.png";
	CHECK_FALSE(uploader->Add(missing));

	const fs::path empty = directory.Root / "empty.png";
	Write(empty, {});
	CHECK_FALSE(uploader->Add(empty));
	CHECK(uploader->Remaining() == 0);
	CHECK(uploader->Pump() == 0);
	CHECK(uploader->Counters().Failed == 2);

	const std::vector outcomes = uploader->Take();
	REQUIRE(outcomes.size() == 2);
	CHECK(outcomes[0].File == missing);
	CHECK(outcomes[0].Detail == "could not be read");
	CHECK_FALSE(outcomes[0].Delivered);
	CHECK(outcomes[1].File == empty);
	CHECK(outcomes[1].Detail == "empty");
	CHECK_FALSE(outcomes[1].Delivered);
	CHECK(uploader->Take().empty());
}

TEST_CASE("a local upload snapshots the file and deduplicates the same content", "[delivery][uploader]") {
	TemporaryDirectory directory("snapshot-and-deduplicate");
	const fs::path source = directory.Root / "terrain.PNG";
	const std::vector<std::byte> original = Bytes("the bytes queued for upload");
	Write(source, original);

	const fs::path destination = directory.Root / "store";
	std::unique_ptr<Uploader> uploader = LocalUploader({Directory("local", destination)});
	REQUIRE(uploader);
	REQUIRE(uploader->Add(source));

	// `Add` owns a snapshot. A save that changes while the editor waits for its
	// next pump must not change the content address or the bytes it publishes.
	Write(source, Bytes("changed after queueing"));
	REQUIRE(uploader->Pump() == 1);
	const ContentHash root = Hasher::Of(original);
	const fs::path stored = destination / (root.ToHex() + ".png");
	CHECK(Read(stored) == original);
	CHECK(uploader->Counters().Stored == 1);
	CHECK(uploader->Counters().SentBytes == original.size());

	const std::vector first = uploader->Take();
	REQUIRE(first.size() == 1);
	CHECK(first[0].Root == root);
	CHECK(first[0].Detail == "stored");
	CHECK(first[0].Delivered);

	Write(source, original);
	REQUIRE(uploader->Add(source));
	REQUIRE(uploader->Pump() == 1);
	CHECK(uploader->Counters().Stored == 1);
	CHECK(uploader->Counters().Skipped == 1);
	CHECK(uploader->Counters().SentBytes == original.size());

	const std::vector second = uploader->Take();
	REQUIRE(second.size() == 1);
	CHECK(second[0].Root == root);
	CHECK(second[0].Detail == "already there");
	CHECK(second[0].Delivered);
}

TEST_CASE("one local upload reaches every writable destination", "[delivery][uploader]") {
	TemporaryDirectory directory("fan-out");
	const fs::path source = directory.Root / "material.bin";
	const std::vector<std::byte> bytes = Bytes("replicate this file");
	Write(source, bytes);

	const fs::path first = directory.Root / "first";
	const fs::path second = directory.Root / "second";
	std::unique_ptr<Uploader> uploader =
		LocalUploader({Directory("first", first), Directory("second", second)});
	REQUIRE(uploader);
	REQUIRE(uploader->Add(source));
	CHECK(uploader->Remaining() == 2);
	REQUIRE(uploader->Pump() == 2);
	CHECK(uploader->Remaining() == 0);
	CHECK(uploader->Counters().Stored == 2);
	CHECK(uploader->Counters().SentBytes == bytes.size() * 2);

	const ContentHash root = Hasher::Of(bytes);
	CHECK(Read(first / (root.ToHex() + ".bin")) == bytes);
	CHECK(Read(second / (root.ToHex() + ".bin")) == bytes);

	const std::vector outcomes = uploader->Take();
	REQUIRE(outcomes.size() == 2);
	CHECK(outcomes[0].Destination == "first");
	CHECK(outcomes[0].Delivered);
	CHECK(outcomes[1].Destination == "second");
	CHECK(outcomes[1].Delivered);
}
