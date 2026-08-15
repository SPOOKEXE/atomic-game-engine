// What compression costs an origin and what it saves a client.
//
// **This suite exists to answer two questions the headers admit are open.**
// `GroupCodec::DEFAULT_LEVEL` says of its value of 9: "Chosen rather than
// derived, and CDN.md §9 carries it as an open question - there is no
// measurement behind it yet and saying so is better than implying there is."
// `Dictionary::DEFAULT_TRAINED_BYTES` says the same of 110 KiB. Neither number
// can be argued about without a ratio-against-time curve, and this is where
// that curve comes from.
//
// **So read the ratio alongside the time, not instead of it.** Every
// compression row prints its output size through `Consume`, and the useful
// output of this suite is the pair: level 9 is only the right default if the
// bytes it saves over level 3 are worth the seconds it costs, and "worth" here
// means bytes served to every client for the life of a build against CPU spent
// once at publish. The trade is enormously in favour of the slow side - which
// is the reasoning the header already gives - but a *bounded* enormously, and
// this says where the bound is.
//
// The dictionary rows are the second question. A trained dictionary ships to
// every client once and then improves every group forever, so it is the
// cheapest large thing in the format - provided it actually improves the ratio.
// A dictionary that buys two percent is a dictionary that costs bytes on every
// fetch and buys nothing, which is exactly what `Dictionary::Load`'s comment
// warns about; these rows are what tell the two cases apart.
//
// **Decompression is the row that runs on a player's machine**, once per group
// fetched, while they wait at a loading screen. It is the only figure here a
// user experiences directly, and it should be very nearly independent of the
// level the origin chose - if it is not, a slow origin is also a slow client
// and the trade above stops being one-sided.

#include <engine/assets/ContentHash.hpp>
#include <engine/delivery/GroupCodec.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.delivery.bench.compression")

using engine::delivery::Dictionary;
using engine::delivery::GroupCodec;
using engine::testing::Consume;

namespace compression_bench {

	// One group. A bundle of small assets concatenated, which is the unit the
	// codec is handed.
	constexpr size_t GROUP_BYTES = 4 * 1024 * 1024;

	// How many samples the dictionary trains over, and how big each is.
	//
	// Zstd needs a reasonable number of reasonably similar samples and refuses
	// when it has too little to learn from. A hundred is comfortably above that
	// floor without making the training row take minutes.
	constexpr size_t SAMPLES = 100;
	constexpr size_t SAMPLE_BYTES = 8 * 1024;

	// Content shaped like a game's, which is the only kind worth measuring.
	//
	// **Random bytes would be the wrong test in the most misleading direction.**
	// Incompressible input makes every level produce the same size in about the
	// same time, so the level ladder would come out flat and the honest
	// conclusion - "level 9 buys nothing" - would be an artefact of the data
	// rather than a fact about the codec. Real content is structured and
	// repetitive: this is a synthetic stand-in with a repeating vocabulary and
	// scattered noise, which is what a folder of models, shaders and scene
	// descriptions looks like to a compressor.
	std::vector<std::byte> ContentOf(size_t bytes, uint32_t seed) {
		static const std::vector<std::string> vocabulary = {
			"<Part name=\"",
			"\" class=\"BasePart\">",
			"<Vector3 x=\"",
			"\" y=\"",
			"\" z=\"",
			"\"/>",
			"</Part>",
			"<Material>Plastic</",
			"<CFrame>",
			"1.000000",
			"0.000000",
			"-1.000000",
			"<Transparency>",
			"<Color3 r=\"",
			"<Children>",
			"</Children>",
		};

		std::vector<std::byte> made;
		made.reserve(bytes);

		uint32_t state = seed | 1u;
		while (made.size() < bytes) {
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;

			// Mostly vocabulary, which is what gives a compressor something to
			// find, plus a run of digits that differs every time - the numeric
			// fields a real scene file is full of and which no dictionary can
			// predict.
			const std::string &token = vocabulary[state % vocabulary.size()];
			for (const char character : token) {
				if (made.size() < bytes) {
					made.push_back(static_cast<std::byte>(character));
				}
			}

			for (int digit = 0; digit < 4 && made.size() < bytes; digit++) {
				made.push_back(static_cast<std::byte>('0' + ((state >> (digit * 4)) & 7u)));
			}
		}
		return made;
	}

	const std::vector<std::byte> &Group() {
		static const std::vector<std::byte> group = ContentOf(GROUP_BYTES, 0x1234'5678u);
		return group;
	}

	// A group of a size a dictionary can actually help with.
	//
	// Four kibibytes, which is a handful of small assets concatenated. Distinct
	// per index so two hundred of them are two hundred different payloads rather
	// than one payload compressed two hundred times - the second would let the
	// allocator and the cache flatter every level equally.
	const std::vector<std::byte> &SmallGroup(size_t index) {
		static std::vector<std::vector<std::byte>> built;
		if (built.empty()) {
			built.reserve(200);
			for (uint32_t made = 0; made < 200; made++) {
				built.push_back(ContentOf(4 * 1024, 0x5151'0000u + made));
			}
		}
		return built[index % built.size()];
	}

	// The training set, and the storage its spans point into.
	const std::vector<std::span<const std::byte>> &Samples() {
		static std::vector<std::vector<std::byte>> storage;
		static const std::vector<std::span<const std::byte>> spans = [] {
			storage.reserve(SAMPLES);
			std::vector<std::span<const std::byte>> made;
			made.reserve(SAMPLES);
			for (uint32_t index = 0; index < SAMPLES; index++) {
				storage.push_back(ContentOf(SAMPLE_BYTES, 0x9E37'79B9u + index));
				made.emplace_back(storage.back());
			}
			return made;
		}();
		return spans;
	}

	// One dictionary, trained once. Training is substantial CPU over the whole
	// sample and an origin does it per published build, never per request - so
	// training inside a row that is measuring compression would be measuring the
	// wrong thing entirely.
	const std::optional<Dictionary> &Trained() {
		static const std::optional<Dictionary> dictionary =
			Dictionary::Train(Samples(), Dictionary::DEFAULT_TRAINED_BYTES);
		return dictionary;
	}

	// A frame at each level, compressed once, for the decompression rows.
	const std::vector<std::byte> &FrameAt(int level, bool withDictionary) {
		static std::vector<std::pair<std::pair<int, bool>, std::vector<std::byte>>> built;
		for (const auto &[key, frame] : built) {
			if (key.first == level && key.second == withDictionary) {
				return frame;
			}
		}

		std::optional<std::vector<std::byte>> frame;
		if (withDictionary && Trained()) {
			frame = GroupCodec::Compress(Group(), *Trained(), level);
		} else {
			frame = GroupCodec::Compress(Group(), level);
		}

		built.emplace_back(std::make_pair(level, withDictionary), frame ? *frame : std::vector<std::byte>{});
		return built.back().second;
	}

	// Prints the ratio each level achieves, once, to **stderr**.
	//
	// **A time without a ratio answers half of a two-sided question**, and the
	// harness prints exactly one number per row - by design, because its output
	// is a tab-separated record the runner parses and diffs against a baseline.
	// Adding a column for this one suite would mean every consumer of that
	// format learns about compression.
	//
	// So the ratios go to stderr, which the runner does not read and a person
	// does. `just bench --filter delivery` shows them above the table; a
	// redirect drops them. That is the right split: the times are the thing
	// tracked over time, and the ratios are the thing read once when somebody
	// asks whether level 9 is the right default.
	void ReportRatios() {
		static const bool once = [] {
			std::fprintf(
				stderr, "\n  group compression, %zu KiB of scene-shaped content\n", GROUP_BYTES / 1024
			);
			std::fprintf(stderr, "  %-28s %12s %8s\n", "variant", "bytes", "ratio");

			const auto line = [](const char *label, size_t bytes) {
				std::fprintf(
					stderr,
					"  %-28s %12zu %7.2fx\n",
					label,
					bytes,
					bytes == 0 ? 0.0 : static_cast<double>(GROUP_BYTES) / static_cast<double>(bytes)
				);
			};

			line("level 1", FrameAt(1, false).size());
			line("level 3 (zstd default)", FrameAt(3, false).size());
			line("level 9 (engine default)", FrameAt(GroupCodec::DEFAULT_LEVEL, false).size());
			line("level 15", FrameAt(15, false).size());
			line("level 19", FrameAt(19, false).size());
			if (Trained()) {
				line("level 9 + dictionary", FrameAt(GroupCodec::DEFAULT_LEVEL, true).size());
			} else {
				std::fprintf(stderr, "  %-28s %12s\n", "level 9 + dictionary", "zstd refused");
			}

			// The size a dictionary is actually for. Totalled over two hundred
			// small groups rather than shown per group, because one 4 KiB
			// payload's saving is inside the noise of its own framing.
			size_t plain = 0;
			size_t withDictionary = 0;
			for (size_t index = 0; index < 200; index++) {
				const std::optional<std::vector<std::byte>> bare =
					GroupCodec::Compress(SmallGroup(index), GroupCodec::DEFAULT_LEVEL);
				plain += bare ? bare->size() : 0;
				if (Trained()) {
					const std::optional<std::vector<std::byte>> aided =
						GroupCodec::Compress(SmallGroup(index), *Trained(), GroupCodec::DEFAULT_LEVEL);
					withDictionary += aided ? aided->size() : 0;
				}
			}

			std::fprintf(stderr, "\n  200 small groups of 4 KiB each (%d KiB total)\n", 200 * 4);
			std::fprintf(
				stderr,
				"  %-28s %12zu %7.2fx\n",
				"level 9",
				plain,
				plain == 0 ? 0.0 : static_cast<double>(200 * 4 * 1024) / static_cast<double>(plain)
			);
			if (Trained()) {
				std::fprintf(
					stderr,
					"  %-28s %12zu %7.2fx\n",
					"level 9 + dictionary",
					withDictionary,
					withDictionary == 0
						? 0.0
						: static_cast<double>(200 * 4 * 1024) / static_cast<double>(withDictionary)
				);
			}
			std::fprintf(stderr, "\n");
			return true;
		}();
		Consume(once);
	}
}

using namespace compression_bench;

// --- the level ladder -------------------------------------------------------------
//
// **One iteration is one kibibyte of input**, so the figure converts to
// throughput and the rows are comparable to `engine.assets.bench.content`'s
// hashing and chunking numbers - which is the comparison that matters, because
// a publish pipeline does all three to the same bytes.
//
// The size each level produces is consumed rather than reported, because the
// harness prints one number per row. Read the sizes by running the binary and
// watching them, or take the ratio from `just bench --filter delivery` beside a
// build's actual group sizes. What this ladder is *for* is the shape: where the
// time starts climbing faster than the ratio improves.

BENCH("Compress · 4 MiB at level 1", GROUP_BYTES / 1024) {
	// The first row in the file, so this is where the ratio table is printed.
	// It is not part of the measurement: the harness runs two warm-up samples
	// before it keeps any, so by the time a number is recorded the table has
	// long since been written and the `once` flag is a predicted branch.
	ReportRatios();
	const std::optional<std::vector<std::byte>> frame = GroupCodec::Compress(Group(), 1);
	Consume(frame ? frame->size() : 0);
}

BENCH("Compress · 4 MiB at level 3 (zstd default)", GROUP_BYTES / 1024) {
	// Zstd's own default, and the level a caller gets by accident anywhere this
	// engine has not overridden it. The baseline the choice of 9 has to beat.
	const std::optional<std::vector<std::byte>> frame = GroupCodec::Compress(Group(), 3);
	Consume(frame ? frame->size() : 0);
}

BENCH("Compress · 4 MiB at level 9 (engine default)", GROUP_BYTES / 1024) {
	// **`DEFAULT_LEVEL`, and the row the header's open question is about.**
	const std::optional<std::vector<std::byte>> frame =
		GroupCodec::Compress(Group(), GroupCodec::DEFAULT_LEVEL);
	Consume(frame ? frame->size() : 0);
}

BENCH("Compress · 4 MiB at level 15", GROUP_BYTES / 1024) {
	const std::optional<std::vector<std::byte>> frame = GroupCodec::Compress(Group(), 15);
	Consume(frame ? frame->size() : 0);
}

BENCH("Compress · 4 MiB at level 19", GROUP_BYTES / 1024) {
	// Near the top of Zstd's ordinary range. **If the ratio here is barely
	// better than level 9 while the time is many times worse, 9 is vindicated
	// and the open question is closed.** If the ratio is materially better, the
	// question is instead why the default is not higher, given that publishing
	// happens once and streaming happens forever.
	const std::optional<std::vector<std::byte>> frame = GroupCodec::Compress(Group(), 19);
	Consume(frame ? frame->size() : 0);
}

// --- the dictionary ---------------------------------------------------------------

BENCH("Compress · 4 MiB at level 9 against a dictionary", GROUP_BYTES / 1024) {
	// The common case, per the header. Against the plain level-9 row, this is
	// what a trained dictionary costs in time - and the size it consumes is what
	// it buys in bytes. Both halves are needed: a dictionary that improves the
	// ratio while doubling the publish time is still worth it; one that improves
	// neither is the failure `Dictionary::Load`'s comment describes.
	if (!Trained()) {
		// Zstd refused to train, which is passed through rather than papered
		// over. The row still runs so the report has a line for it.
		Consume(0);
		return;
	}
	const std::optional<std::vector<std::byte>> frame =
		GroupCodec::Compress(Group(), *Trained(), GroupCodec::DEFAULT_LEVEL);
	Consume(frame ? frame->size() : 0);
}

BENCH("Compress · 200 small groups at level 9, no dictionary", 200) {
	// **The 4 MiB rows above are the wrong size to judge a dictionary by, and
	// this pair is the right one.** Zstd builds its own history as it goes, so
	// on a megabytes-long payload it has learned the content's vocabulary within
	// the first few kilobytes and a supplied dictionary adds almost nothing. A
	// dictionary earns its keep on payloads too *short* to build that history -
	// which is precisely what a group of small assets is, and what the format
	// ships them as.
	//
	// One iteration is one small group, so this row and the next are directly
	// comparable and the ratio table above the report carries the sizes.
	for (size_t index = 0; index < 200; index++) {
		const std::optional<std::vector<std::byte>> frame =
			GroupCodec::Compress(SmallGroup(index), GroupCodec::DEFAULT_LEVEL);
		Consume(frame ? frame->size() : 0);
	}
}

BENCH("Compress · 200 small groups at level 9, dictionary", 200) {
	if (!Trained()) {
		Consume(0);
		return;
	}
	for (size_t index = 0; index < 200; index++) {
		const std::optional<std::vector<std::byte>> frame =
			GroupCodec::Compress(SmallGroup(index), *Trained(), GroupCodec::DEFAULT_LEVEL);
		Consume(frame ? frame->size() : 0);
	}
}

BENCH("Dictionary::Train · 100 samples of 8 KiB", 1) {
	// **Once per published build, and it is allowed to be slow.** The row exists
	// so that "substantial CPU over the whole sample" is a figure rather than an
	// adjective - a publish step measured in seconds is fine and one measured in
	// minutes changes how a build pipeline is arranged.
	const std::optional<Dictionary> dictionary =
		Dictionary::Train(Samples(), Dictionary::DEFAULT_TRAINED_BYTES);
	Consume(dictionary.has_value());
}

BENCH("Dictionary::Load · 10k refusals of non-dictionary bytes", 10'000) {
	// **The mistake `Load` exists to catch**, timed. Zstd would happily accept
	// arbitrary bytes as a "raw content" dictionary - legal, and nearly useless
	// - so a manifest shipped where a dictionary was expected would cost ratio
	// on every group for the life of the deployment, silently. `Load` refuses
	// anything without a trained dictionary's magic instead.
	//
	// That check is a few bytes at the front, so this row should be immeasurably
	// cheap. A row that costs anything real means the refusal is walking the
	// whole buffer to decide, which on a mis-shipped multi-megabyte manifest
	// would be a startup stall on every client.
	static const std::vector<std::byte> notADictionary = ContentOf(64 * 1024, 0xABCD'1234u);
	uint32_t loaded = 0;
	for (size_t index = 0; index < 10'000; index++) {
		loaded += Dictionary::Load(notADictionary).has_value() ? 1u : 0u;
	}
	Consume(loaded);
}

// --- decompression ------------------------------------------------------------------
//
// **The only figure in this file a player experiences.** It runs on their
// machine, once per group, while they wait - and it should be nearly
// independent of the level the origin chose, because Zstd's decoder does the
// same work whatever produced the frame. Two levels are measured precisely to
// check that.

BENCH("Decompress · 4 MiB from a level 3 frame", GROUP_BYTES / 1024) {
	const std::vector<std::byte> &frame = FrameAt(3, false);
	const std::optional<std::vector<std::byte>> payload = GroupCodec::Decompress(frame, GROUP_BYTES);
	Consume(payload ? payload->size() : 0);
}

BENCH("Decompress · 4 MiB from a level 19 frame", GROUP_BYTES / 1024) {
	// Read against the row above. **A materially slower figure here would mean
	// the origin's choice of level is also the client's problem**, which would
	// turn a one-sided trade into a real one and is the single thing that could
	// argue the default down rather than up.
	const std::vector<std::byte> &frame = FrameAt(19, false);
	const std::optional<std::vector<std::byte>> payload = GroupCodec::Decompress(frame, GROUP_BYTES);
	Consume(payload ? payload->size() : 0);
}

BENCH("Decompress · 4 MiB against a dictionary", GROUP_BYTES / 1024) {
	if (!Trained()) {
		Consume(0);
		return;
	}
	const std::vector<std::byte> &frame = FrameAt(GroupCodec::DEFAULT_LEVEL, true);
	const std::optional<std::vector<std::byte>> payload =
		GroupCodec::Decompress(frame, *Trained(), GROUP_BYTES);
	Consume(payload ? payload->size() : 0);
}

// --- the hostile path -----------------------------------------------------------------

BENCH("Decompress · 10k frames refused on the expected size", 10'000) {
	// **The decompression bomb, refused.** `expectedBytes` comes from the signed
	// manifest and never from the frame, because a frame header can claim any
	// decompressed size and sizing a buffer from it turns a few kilobytes on the
	// wire into a multi-gigabyte allocation. A frame that decompresses to
	// anything other than exactly `expectedBytes` is refused rather than
	// truncated or padded - so this row asks for a size the frame does not have,
	// ten thousand times, and must stay flat and cheap.
	//
	// A row that grew with the *claimed* size, or that allocated the difference
	// before noticing, would be the bug itself.
	const std::vector<std::byte> &frame = FrameAt(3, false);
	uint32_t accepted = 0;
	for (size_t index = 0; index < 10'000; index++) {
		accepted += GroupCodec::Decompress(frame, 64).has_value() ? 1u : 0u;
	}
	Consume(accepted);
}

BENCH("Decompress · 10k frames of garbage", 10'000) {
	static const std::vector<std::byte> garbage = [] {
		std::vector<std::byte> built(1024);
		for (size_t index = 0; index < built.size(); index++) {
			built[index] = static_cast<std::byte>(index * 37u);
		}
		return built;
	}();

	uint32_t accepted = 0;
	for (size_t index = 0; index < 10'000; index++) {
		accepted += GroupCodec::Decompress(garbage, GROUP_BYTES).has_value() ? 1u : 0u;
	}
	Consume(accepted);
}
