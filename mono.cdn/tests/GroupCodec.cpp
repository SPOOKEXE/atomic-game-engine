#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cdn/GroupCodec.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("cdn.groupcodec")
TEST_DEPENDS("engine.assets.contenthash")
TEST_DEPENDS("engine.core.framegraph")
TEST_DEPENDS("engine.core.metrics")

using cdn::Dictionary;
using cdn::GroupCodec;
using engine::core::FrameGraph;
using engine::core::Metrics;

namespace {
	std::vector<std::byte> Bytes(std::string_view text) {
		std::vector<std::byte> data(text.size());
		std::memcpy(data.data(), text.data(), text.size());
		return data;
	}

	// Content shaped like a game's: many small records that share most of their
	// structure and differ in a few fields. That similarity is exactly what a
	// dictionary learns, and content without it would make the dictionary cases
	// below prove nothing.
	std::string Record(int index) {
		return "{\"asset\":\"mesh_" + std::to_string(index) +
			   "\",\"format\":\"cooked-v1\",\"vertices\":" + std::to_string(index * 37 % 5000) +
			   ",\"material\":\"standard-pbr\",\"lod\":0,\"flags\":[\"static\",\"shadowcast\"]}";
	}

	std::vector<std::byte> Payload(int records) {
		std::string text;
		for (int index = 0; index < records; ++index) {
			text += Record(index);
		}
		return Bytes(text);
	}

	// A dictionary trained on the same shape of content. Zstd refuses when it
	// has too little to learn from, so this uses a generous sample count.
	Dictionary Trained() {
		std::vector<std::vector<std::byte>> owned;
		owned.reserve(2048);
		for (int index = 0; index < 2048; ++index) {
			owned.push_back(Bytes(Record(index + 100'000)));
		}

		std::vector<std::span<const std::byte>> samples;
		samples.reserve(owned.size());
		for (const auto &sample : owned) {
			samples.emplace_back(sample);
		}

		// A small capacity on purpose. Zstd warns when the sample is under ten
		// times the dictionary size, and a test that trains a 110 KiB
		// dictionary on 190 KiB of samples is exercising the warning path rather
		// than the feature.
		auto dictionary = Dictionary::Train(samples, 8 * 1024);
		REQUIRE(dictionary.has_value());
		return std::move(*dictionary);
	}
}

TEST_CASE("a payload round-trips", "[cdn][groupcodec]") {
	const auto payload = Payload(200);

	const auto frame = GroupCodec::Compress(payload);
	REQUIRE(frame.has_value());

	const auto restored = GroupCodec::Decompress(*frame, payload.size());
	REQUIRE(restored.has_value());
	CHECK(*restored == payload);
}

TEST_CASE("compression actually compresses", "[cdn][groupcodec]") {
	const auto payload = Payload(500);

	const auto frame = GroupCodec::Compress(payload);
	REQUIRE(frame.has_value());

	INFO(payload.size() << " -> " << frame->size());
	CHECK(frame->size() < payload.size());
}

TEST_CASE("compression is deterministic", "[cdn][groupcodec]") {
	const auto payload = Payload(100);

	// A prepared group is cached by content. Two runs producing different frames
	// would mean the cache key does not identify the artefact.
	const auto first = GroupCodec::Compress(payload);
	const auto second = GroupCodec::Compress(payload);
	REQUIRE(first.has_value());
	REQUIRE(second.has_value());
	CHECK(*first == *second);
}

TEST_CASE("an empty payload round-trips as nothing", "[cdn][groupcodec]") {
	const auto frame = GroupCodec::Compress({});
	REQUIRE(frame.has_value());

	// Zero expected bytes is refused rather than answered with an empty buffer:
	// a group weighing nothing is not a group, and the manifest never records
	// one.
	CHECK_FALSE(GroupCodec::Decompress(*frame, 0).has_value());
}

TEST_CASE("a frame is refused when the size does not match the manifest", "[cdn][groupcodec]") {
	const auto payload = Payload(50);
	const auto frame = GroupCodec::Compress(payload);
	REQUIRE(frame.has_value());

	CHECK(GroupCodec::Decompress(*frame, payload.size()).has_value());

	// Exactly, not at most. Content that does not match what the manifest
	// describes is refused rather than truncated or padded to fit — padding it
	// would hand the hash check bytes the origin never sent.
	CHECK_FALSE(GroupCodec::Decompress(*frame, payload.size() - 1).has_value());
	CHECK_FALSE(GroupCodec::Decompress(*frame, payload.size() + 1).has_value());
}

TEST_CASE("a corrupt or empty frame is refused", "[cdn][groupcodec]") {
	const auto payload = Payload(50);
	const auto frame = GroupCodec::Compress(payload);
	REQUIRE(frame.has_value());

	CHECK_FALSE(GroupCodec::Decompress({}, payload.size()).has_value());

	auto truncated = *frame;
	truncated.resize(truncated.size() / 2);
	CHECK_FALSE(GroupCodec::Decompress(truncated, payload.size()).has_value());

	// A flipped byte in the middle of the frame. Caught by the content checksum
	// the codec turns on — Zstd leaves it off, and with it off this decompresses
	// cleanly to the right length and the wrong content. The chunk hashes would
	// still catch that downstream, but only after a whole group had been
	// transferred and expanded.
	auto edited = *frame;
	edited[edited.size() / 2] =
		static_cast<std::byte>(static_cast<uint8_t>(edited[edited.size() / 2]) ^ 0xFF);
	CHECK_FALSE(GroupCodec::Decompress(edited, payload.size()).has_value());

	auto garbage = Bytes("this is not a zstd frame at all, not even slightly");
	CHECK_FALSE(GroupCodec::Decompress(garbage, payload.size()).has_value());
}

TEST_CASE("an absurd expected size is refused before it is allocated", "[cdn][groupcodec]") {
	const auto frame = GroupCodec::Compress(Payload(10));
	REQUIRE(frame.has_value());

	// The backstop above the manifest's own bound. Sizing a buffer from what a
	// frame *claims* is the classic decompression bomb — a few kilobytes on the
	// wire declaring a multi-gigabyte payload — which is why the size comes from
	// the signed manifest and why even that is bounded.
	CHECK_FALSE(GroupCodec::Decompress(*frame, GroupCodec::MAXIMUM_PAYLOAD_BYTES + 1).has_value());
}

TEST_CASE("a dictionary improves the ratio on small similar payloads", "[cdn][groupcodec]") {
	const Dictionary dictionary = Trained();

	// One small record — the case a dictionary exists for. Without one there is
	// no history to compress against and a few hundred bytes stay a few hundred
	// bytes; the dictionary supplies the history.
	const auto payload = Bytes(Record(424'242));

	const auto plain = GroupCodec::Compress(payload);
	const auto withDictionary = GroupCodec::Compress(payload, dictionary);
	REQUIRE(plain.has_value());
	REQUIRE(withDictionary.has_value());

	INFO(
		payload.size() << " raw, " << plain->size() << " plain, " << withDictionary->size()
					   << " with dictionary"
	);
	CHECK(withDictionary->size() < plain->size());
}

TEST_CASE("a dictionary round-trips a payload", "[cdn][groupcodec]") {
	const Dictionary dictionary = Trained();
	const auto payload = Payload(120);

	const auto frame = GroupCodec::Compress(payload, dictionary);
	REQUIRE(frame.has_value());

	const auto restored = GroupCodec::Decompress(*frame, dictionary, payload.size());
	REQUIRE(restored.has_value());
	CHECK(*restored == payload);
}

TEST_CASE("a dictionary frame does not decode without its dictionary", "[cdn][groupcodec]") {
	const Dictionary dictionary = Trained();
	const auto payload = Payload(120);

	const auto frame = GroupCodec::Compress(payload, dictionary);
	REQUIRE(frame.has_value());

	// Which is why the dictionary hash is half a prepared group's cache key: a
	// group compressed against one dictionary is a different artefact from the
	// same group compressed against another, and serving the wrong one hands a
	// client bytes it cannot decode.
	CHECK_FALSE(GroupCodec::Decompress(*frame, payload.size()).has_value());
}

TEST_CASE("a dictionary is addressed by its content", "[cdn][groupcodec]") {
	const Dictionary dictionary = Trained();

	CHECK_FALSE(dictionary.Hash().IsZero());
	CHECK_FALSE(dictionary.Bytes().empty());

	// Loading the same bytes gives the same address, so a dictionary versions
	// like everything else instead of being an out-of-band file that can drift.
	const auto reloaded = Dictionary::Load(dictionary.Bytes());
	REQUIRE(reloaded.has_value());
	CHECK(reloaded->Hash() == dictionary.Hash());
}

TEST_CASE("bytes that are not a trained dictionary are refused", "[cdn][groupcodec]") {
	// Zstd would accept these as a "raw content" dictionary, which is legal and
	// nearly useless. A manifest shipped where a dictionary was expected would
	// then cost ratio on every group for the life of the deployment, silently.
	CHECK_FALSE(Dictionary::Load({}).has_value());
	CHECK_FALSE(Dictionary::Load(Bytes("not a dictionary")).has_value());
	CHECK_FALSE(Dictionary::Load(Payload(20)).has_value());
}

TEST_CASE("training refuses when there is too little to learn from", "[cdn][groupcodec]") {
	CHECK_FALSE(Dictionary::Train({}).has_value());

	const auto one = Bytes("a single short sample");
	const std::vector<std::span<const std::byte>> tooFew{one};
	CHECK_FALSE(Dictionary::Train(tooFew).has_value());

	// A dictionary trained on too little is worse than none: it ships to every
	// client and costs bytes on every fetch while buying nothing.
}

TEST_CASE(
	"the codec reports itself to the frame graph and the metrics sink", "[cdn][groupcodec][framegraph]"
) {
	const auto payload = Payload(100);

	Metrics::Clear();
	FrameGraph::SetEnabled(true);
	FrameGraph::BeginFrame();
	const auto frame = GroupCodec::Compress(payload);
	REQUIRE(frame.has_value());
	CHECK(GroupCodec::Decompress(*frame, payload.size()).has_value());
	CHECK_FALSE(GroupCodec::Decompress(*frame, payload.size() + 1).has_value());
	FrameGraph::EndFrame();
	const std::vector<engine::core::FrameSpan> spans(FrameGraph::Spans().begin(), FrameGraph::Spans().end());
	FrameGraph::SetEnabled(false);

	const auto named = [&spans](std::string_view name) {
		return std::any_of(spans.begin(), spans.end(), [name](const auto &span) {
			return span.Name == name;
		});
	};

	CHECK(named("GroupCodec::Compress"));
	CHECK(named("GroupCodec::Decompress"));

	const auto counters = Metrics::Drain();
	const auto total = [&counters](std::string_view name) {
		double sum = 0.0;
		for (const auto &counter : counters) {
			if (counter.Name == engine::core::Name(name)) {
				sum += counter.Value;
			}
		}
		return sum;
	};

	// In and out both counted, because the ratio is the number worth watching
	// and neither half says anything on its own.
	CHECK(total("cdn.codec.compressed.in") == static_cast<double>(payload.size()));
	CHECK(total("cdn.codec.compressed.out") == static_cast<double>(frame->size()));
	CHECK(total("cdn.codec.decompressed") == 1.0);
	CHECK(total("cdn.codec.decompress.refused") == 1.0);
}
