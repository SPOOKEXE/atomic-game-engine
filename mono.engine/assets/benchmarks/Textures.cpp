// What a texture costs to prepare, to store and to refuse.
//
// **Textures are where the bytes are.** A game's meshes are megabytes and its
// textures are gigabytes, so every figure here multiplies by a number far
// larger than anything else in this repository - and all three of the paths
// below run over every one of them. A publisher builds a mip chain per texture;
// a client reads one per texture; and anything that arrives from a disk or a
// wire has to be *refused* per texture when it is malformed.
//
// **The mip chain is the publish path's real cost and it is not obvious.**
// A chain is a third more pixels than the base image, so building one touches
// every pixel of the original and then two thirds again - and it is a
// separable box filter over the whole thing rather than a copy. At 4096 on a
// side that is sixteen megapixels in and twenty-two out, per texture, and a
// publication has thousands. The rows walk the size so the growth is visible:
// four times the pixels should be four times the cost, and anything steeper is
// the allocation of the levels rather than the filtering of them.
//
// **`R8` is a separate row because a mask is a quarter of the bytes.** The
// format is carried rather than assumed precisely so a mask is not uploaded as
// RGBA, and the pair of rows is what says whether the filter's cost tracks the
// bytes or the pixels. If a single-channel image costs the same as a
// four-channel one of the same dimensions, something is widening it.
//
// **The refusal rows are the security half.** A header field is four bytes an
// attacker chooses, and `Texture::Read` sums the whole chain's size and checks
// it against the bytes actually present *before allocating anything* - which is
// the difference between a refusal and a decompression bomb. The row that hands
// it a header claiming sixteen thousand on a side with no pixels behind it is
// the one that proves the check is a comparison rather than a `resize`. It has
// to be as cheap as any other malformed input; a refusal that allocated first
// would be a way to exhaust a client's memory using a hundred bytes of
// bandwidth.
//
// Nothing here decodes a PNG. This format is the engine's own, `Texture.hpp`
// says at length why, and the vendored decoder that turns an authored image
// into one of these runs in `assetc` rather than at runtime.

#include <engine/assets/Resample.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

TEST_SUITE_ID("engine.assets.bench.textures")

using engine::assets::BuildMipChain;
using engine::assets::BytesPerPixel;
using engine::assets::MipLevelCount;
using engine::assets::ResizeImage;
using engine::assets::Texture;
using engine::assets::TextureData;
using engine::assets::TextureFormat;
using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::testing::Consume;

namespace texture_bench {
	// An icon, a character's albedo and a terrain sheet.
	//
	// **Capped at 2048, and the cap is about the neighbours rather than about
	// textures.** The images, their chains and their encoded forms are all
	// cached for the life of the process so that no row pays to build its own
	// input - and at 4096 that was better than half a gigabyte resident, which
	// evicted every other suite in this binary from cache. `Hasher::Of · 16 MiB`
	// in `Content.cpp` measured 131 ns/KiB run on its own and 229 ns/KiB run
	// after these rows, for no reason except that they had been there.
	//
	// A benchmark that makes its neighbours slower is reporting its own working
	// set in their figures, which is worse than not measuring a 4096 texture.
	// Three sizes a factor of two apart answer the scaling question just as
	// well, and the same-size `ResizeImage` result below is just as visible at
	// 2048.
	constexpr uint32_t SMALL = 512;
	constexpr uint32_t MEDIUM = 1024;
	constexpr uint32_t LARGE = 2048;

	// Refusals per sample. A client under a flood of malformed content is
	// answering at this rate, so the figure is what one costs multiplied by
	// however fast somebody can send.
	constexpr size_t REFUSALS = 100'000;

	// An image with content in it, built once per shape.
	//
	// **Not flat, and not random either.** A flat image would let a filter that
	// short-circuits equal neighbours look fast; random noise would defeat every
	// cache in a way no real texture does. A gradient with a repeating pattern
	// on top is what an authored texture actually looks like to a box filter.
	//
	// A deque, so a reference handed out earlier survives a later shape being
	// built.
	const TextureData &Image(uint32_t side, TextureFormat format = TextureFormat::RGBA8) {
		struct Built {
			uint32_t Side;
			TextureFormat Format;
			TextureData Data;
		};
		static std::deque<Built> built;

		for (const Built &entry : built) {
			if (entry.Side == side && entry.Format == format) {
				return entry.Data;
			}
		}

		TextureData data;
		data.Width = side;
		data.Height = side;
		data.Format = format;
		const size_t stride = static_cast<size_t>(side) * BytesPerPixel(format);
		data.Pixels.resize(stride * side);
		for (uint32_t row = 0; row < side; row++) {
			for (size_t at = 0; at < stride; at++) {
				const size_t index = static_cast<size_t>(row) * stride + at;
				data.Pixels[index] = static_cast<std::byte>((row * 3 + at * 5) ^ (at >> 4));
			}
		}

		built.push_back(Built{side, format, std::move(data)});
		return built.back().Data;
	}

	// The same image with a full chain already on it, for the rows that store
	// one rather than build one.
	const TextureData &Chained(uint32_t side) {
		struct Built {
			uint32_t Side;
			TextureData Data;
		};
		static std::deque<Built> built;

		for (const Built &entry : built) {
			if (entry.Side == side) {
				return entry.Data;
			}
		}

		TextureData data = Image(side);
		BuildMipChain(data);
		built.push_back(Built{side, std::move(data)});
		return built.back().Data;
	}

	// The encoded form of that, so the read rows parse rather than encode.
	const std::vector<std::byte> &Encoded(uint32_t side) {
		struct Built {
			uint32_t Side;
			std::vector<std::byte> Bytes;
		};
		static std::deque<Built> built;

		for (const Built &entry : built) {
			if (entry.Side == side) {
				return entry.Bytes;
			}
		}

		ByteWriter writer;
		Texture::Write(writer, Chained(side));
		built.push_back(Built{side, std::vector<std::byte>(writer.Bytes().begin(), writer.Bytes().end())});
		return built.back().Bytes;
	}

	// Reads the prepared bytes `REFUSALS` times and counts what was accepted, so
	// both branches stay live.
	size_t ReadAll(const std::vector<std::byte> &bytes, size_t attempts) {
		size_t accepted = 0;
		for (size_t attempt = 0; attempt < attempts; attempt++) {
			ByteReader reader(bytes);
			TextureData out;
			accepted += Texture::Read(reader, out) ? 1 : 0;
		}
		return accepted;
	}
}

using namespace texture_bench;

// --- building the chain -------------------------------------------------------
//
// Four times the pixels between each pair of rows. Four times the cost is a
// filter that scales with its input; anything steeper is the levels being
// allocated rather than filtered.

BENCH("BuildMipChain · 512x512 RGBA8", 1) {
	TextureData image = Image(SMALL);
	Consume(BuildMipChain(image));
	Consume(image.Mips.size());
}

BENCH("BuildMipChain · 1024x1024 RGBA8", 1) {
	TextureData image = Image(MEDIUM);
	Consume(BuildMipChain(image));
	Consume(image.Mips.size());
}

BENCH("BuildMipChain · 2048x2048 RGBA8", 1) {
	// Four megapixels in, five and a half out. A publication with a thousand of
	// these spends this figure a thousand times, and a person iterating on
	// content sits in that loop.
	TextureData image = Image(LARGE);
	Consume(BuildMipChain(image));
	Consume(image.Mips.size());
}

BENCH("BuildMipChain · 1024x1024 R8", 1) {
	// **A quarter of the bytes at the same pixel count.** Read against the
	// RGBA8 row of the same size: near a quarter means the filter is bounded by
	// memory, near equal means something is widening a mask to four channels on
	// the way through - which is the exact mistake the format field exists to
	// prevent, made one layer lower. As measured it is a quarter, so the filter
	// tracks bytes and a mask costs what a mask should.
	TextureData image = Image(MEDIUM, TextureFormat::R8);
	Consume(BuildMipChain(image));
	Consume(image.Mips.size());
}

BENCH("MipLevelCount · 100k calls", 100'000) {
	// Arithmetic, and the bound `Texture::Read` checks a claimed level count
	// against. It is here because that check is on the refusal path and a
	// refusal path's cost is the thing this file is most interested in.
	size_t levels = 0;
	for (uint32_t call = 0; call < 100'000; call++) {
		levels += MipLevelCount(LARGE, LARGE - (call & 0xFF));
	}
	Consume(levels);
}

// --- resizing -----------------------------------------------------------------

BENCH("ResizeImage · 2048x2048 down to 512x512", 1) {
	// The common direction: an authored texture reduced to the budget a
	// platform was given. Four megapixels read, a quarter of one written.
	TextureData out;
	Consume(ResizeImage(Image(LARGE), SMALL, SMALL, out));
	Consume(out.Pixels.size());
}

BENCH("ResizeImage · 256x256 up to 1024x1024", 1) {
	// The other direction, which costs by its *output* rather than its input -
	// so it is the row that says whether the filter loops over source pixels or
	// destination ones.
	TextureData out;
	Consume(ResizeImage(Image(256), MEDIUM, MEDIUM, out));
	Consume(out.Pixels.size());
}

BENCH("ResizeImage · 2048x2048 to the same size", 1) {
	// **The most expensive row in this file, and it is a no-op.** As measured it
	// costs more than building the entire mip chain of the same image and
	// several times what resizing it *down* costs, because `ResizeImage` has no
	// early-out for a target equal to the source: it resamples four megapixels
	// into four megapixels at the same per-output-pixel rate every other
	// direction pays.
	//
	// It is reachable. `bakegraph`'s `NodeKind::Resize` passes the size the
	// graph author wrote, so a pipeline that normalises every texture to a
	// target pays this for every texture that already was that size.
	//
	// The fix is not obviously free, which is why this is a row rather than a
	// patch: a box filter at a scale of exactly one is not guaranteed to
	// reproduce its input byte for byte, so short-circuiting to a copy is a
	// change to what comes out and not only to how long it takes.
	TextureData out;
	Consume(ResizeImage(Image(LARGE), LARGE, LARGE, out));
	Consume(out.Pixels.size());
}

// --- storing and loading ------------------------------------------------------

BENCH("Texture::Write · 1024x1024 with a full chain", 1) {
	// What a publisher writes per texture. The chain is a third more bytes than
	// the base image and every one of them is copied into the writer.
	ByteWriter writer;
	Consume(Texture::Write(writer, Chained(MEDIUM)));
	Consume(writer.Bytes().size());
}

BENCH("Texture::Read · 1024x1024 with a full chain", 1) {
	// What a client pays per texture on the load path, and it is a parse plus
	// the allocation of every level. Against the write row, the difference is
	// what the bounds checks cost - which should be nothing next to the copy.
	Consume(ReadAll(Encoded(MEDIUM), 1));
}

BENCH("Texture::Read · 2048x2048 with a full chain", 1) {
	// Four times the bytes of the row above. Any excess over four times the cost
	// is the allocator rather than the parser - the larger set of levels is
	// released between samples and faulted in again on each - so read the pair
	// as an upper bound on a cold load rather than as a clean scaling curve.
	Consume(ReadAll(Encoded(LARGE), 1));
}

// --- refusing -----------------------------------------------------------------
//
// Every byte here came off a disk or a wire. A header field an attacker chooses
// must cost a comparison and not an allocation, and these rows are how that is
// stated as a number rather than as an intention.

BENCH("Texture::Read · 100k datagrams that are not textures", REFUSALS) {
	// A wrong magic, refused at the first four bytes. The cheapest thing this
	// format does and the most frequent one under a flood.
	static const std::vector<std::byte> rubbish(256, std::byte{0x5C});
	Consume(ReadAll(rubbish, REFUSALS));
}

BENCH("Texture::Read · 100k headers claiming 16384 on a side with no pixels", REFUSALS) {
	// **The decompression bomb, and the row this section exists for.** A
	// hundred-odd bytes of header claim a gigabyte of image. The whole chain's
	// size is summed and compared against what actually arrived before anything
	// is allocated, so this must land beside the wrong-magic row above rather
	// than anywhere near the read rows. A figure in between is a `resize`
	// happening before the check, and a hundred bytes of bandwidth would then
	// buy an out-of-memory kill. As measured it is nanoseconds, five orders of
	// magnitude under the read it is pretending to be.
	static const std::vector<std::byte> bomb = [] {
		std::vector<std::byte> header = Encoded(SMALL);
		header.resize(64);
		// The dimensions sit after the magic and the version; rewriting them to
		// the maximum leaves a header that is well formed and a payload that
		// cannot possibly be there.
		for (size_t at = 6; at + 8 <= header.size() && at < 22; at++) {
			header[at] = static_cast<std::byte>(at % 2 == 0 ? 0x00 : 0x40);
		}
		return header;
	}();
	Consume(ReadAll(bomb, REFUSALS));
}

BENCH("Texture::Read · 100k truncated textures", REFUSALS) {
	// A real texture cut in half, which is what a clipped download or a
	// half-written file looks like. Refused on the length sum, not by reading
	// past the end.
	static const std::vector<std::byte> clipped = [] {
		const std::vector<std::byte> &whole = Encoded(SMALL);
		return std::vector<std::byte>(whole.begin(), whole.begin() + whole.size() / 2);
	}();
	Consume(ReadAll(clipped, REFUSALS));
}
