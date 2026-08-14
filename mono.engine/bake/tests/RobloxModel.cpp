// Reading Roblox's binary model container.
//
// **The fixture is written by this file rather than checked in**, and the reason
// is the opposite of `Model.cpp`'s: a `.glb` fixture is a blob because a blob is
// the only way to hold one, while an `.rbxm` is a handful of chunks whose *shape*
// is the thing under test. A builder that lays the bytes out field by field is
// how a case can say "and now claim a thousand instances in a chunk holding one"
// without hand-editing hex.
//
// The builder is the format's writer, so it is deliberately dumb: it computes
// nothing the reader will check. Where the two would otherwise agree by
// construction the case pins the value — the transposed arrays are written out
// plane by plane here and read back plane by plane there, and both are asserted
// against numbers a person typed.

#include <engine/bake/RobloxModel.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.bake.robloxmodel")

using Catch::Approx;
using engine::bake::ReadRobloxModel;
using engine::bake::RobloxInstance;
using engine::bake::RobloxModel;
using engine::bake::RobloxValueKind;

namespace {
	// The tag that ends a file, whose fourth byte is a NUL — so it is spelled
	// with its length rather than left to `strlen`, which would write three
	// bytes and a chunk header the reader would land in the middle of.
	constexpr std::string_view END_TAG("END\0", 4);

	// The format's writer, as far as these cases need one.
	struct Blob {
		std::vector<uint8_t> Bytes;

		void U8(uint8_t value) {
			Bytes.push_back(value);
		}

		void U16(uint16_t value) {
			Bytes.push_back(static_cast<uint8_t>(value & 0xFF));
			Bytes.push_back(static_cast<uint8_t>(value >> 8));
		}

		void U32(uint32_t value) {
			for (int shift = 0; shift < 32; shift += 8) {
				Bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
			}
		}

		void I32(int32_t value) {
			U32(static_cast<uint32_t>(value));
		}

		// A float written in place, which is how the nine of a rotation matrix
		// and the two of a NumberRange arrive.
		void F32(float value) {
			uint32_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			U32(bits);
		}

		void Text(std::string_view text) {
			U32(static_cast<uint32_t>(text.size()));
			Bytes.insert(Bytes.end(), text.begin(), text.end());
		}

		void Raw(std::string_view text) {
			Bytes.insert(Bytes.end(), text.begin(), text.end());
		}

		// The byte-plane layout every numeric array in a property payload uses:
		// the top byte of every value, then the next of every value.
		void Transposed(const std::vector<uint32_t> &values) {
			for (int shift = 24; shift >= 0; shift -= 8) {
				for (const uint32_t value : values) {
					Bytes.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
				}
			}
		}

		// Transposed floats, with the sign bit rotated to the bottom.
		void Reals(const std::vector<float> &values) {
			std::vector<uint32_t> words;
			for (const float value : values) {
				uint32_t bits = 0;
				std::memcpy(&bits, &value, sizeof(bits));
				words.push_back((bits << 1) | (bits >> 31));
			}
			Transposed(words);
		}

		// Transposed, zigzagged and *cumulative*, which is what a referent list
		// is. The deltas are written, not the values.
		void Referents(const std::vector<int32_t> &values) {
			std::vector<uint32_t> words;
			int32_t previous = 0;
			for (const int32_t value : values) {
				const int32_t delta = value - previous;
				previous = value;
				words.push_back(static_cast<uint32_t>((delta << 1) ^ (delta >> 31)));
			}
			Transposed(words);
		}
	};

	// A chunk stored with no compression at all, which the format spells as a
	// compressed length of zero.
	void Chunk(Blob &file, std::string_view tag, const Blob &payload) {
		file.Raw(tag);
		file.U32(0);
		file.U32(static_cast<uint32_t>(payload.Bytes.size()));
		file.U32(0);
		file.Bytes.insert(file.Bytes.end(), payload.Bytes.begin(), payload.Bytes.end());
	}

	// The same, LZ4-framed as a single all-literal block.
	//
	// **A legal block that happens to compress nothing.** The last sequence of an
	// LZ4 block is literals with no match after it, so this is the shortest valid
	// framing there is — which makes it the right one for asserting that the
	// *framing* is understood without the case also owning a compressor.
	void Lz4Chunk(Blob &file, std::string_view tag, const Blob &payload) {
		Blob block;
		const size_t length = payload.Bytes.size();
		if (length < 15) {
			block.U8(static_cast<uint8_t>(length << 4));
		} else {
			block.U8(0xF0);
			size_t left = length - 15;
			while (left >= 255) {
				block.U8(255);
				left -= 255;
			}
			block.U8(static_cast<uint8_t>(left));
		}
		block.Bytes.insert(block.Bytes.end(), payload.Bytes.begin(), payload.Bytes.end());

		file.Raw(tag);
		file.U32(static_cast<uint32_t>(block.Bytes.size()));
		file.U32(static_cast<uint32_t>(length));
		file.U32(0);
		file.Bytes.insert(file.Bytes.end(), block.Bytes.begin(), block.Bytes.end());
	}

	void Header(Blob &file, int32_t classes, int32_t instances) {
		file.Raw("<roblox!\x89\xff\x0d\x0a\x1a\x0a");
		file.U16(0);
		file.I32(classes);
		file.I32(instances);
		file.U32(0);
		file.U32(0);
	}

	Blob Instances(int32_t classIndex, std::string_view className, const std::vector<int32_t> &referents) {
		Blob payload;
		payload.I32(classIndex);
		payload.Text(className);
		payload.U8(0);
		payload.I32(static_cast<int32_t>(referents.size()));
		payload.Referents(referents);
		return payload;
	}

	// A property chunk's header, so a case only writes the values.
	Blob Property(int32_t classIndex, std::string_view name, uint8_t type) {
		Blob payload;
		payload.I32(classIndex);
		payload.Text(name);
		payload.U8(type);
		return payload;
	}

	// A `Model` holding a `Part`, with one property of each shape that is easy to
	// read back wrong.
	//
	// The `Material` chunk sits **between** two chunks that must arrive, because
	// the property it holds is a type this reader refuses: a refusal that
	// consumed the wrong number of bytes would take everything after it with it,
	// and a case that put the refused chunk last could not see that.
	Blob Fixture() {
		Blob file;
		Header(file, 3, 3);

		Chunk(file, "META", [] {
			Blob meta;
			meta.I32(1);
			meta.Text("ExplicitAutoJoints");
			meta.Text("true");
			return meta;
		}());

		Chunk(file, "INST", Instances(0, "Model", {0}));
		Chunk(file, "INST", Instances(1, "Part", {1}));
		Chunk(file, "INST", Instances(2, "Script", {2}));

		Chunk(file, "PROP", [] {
			Blob prop = Property(0, "Name", 0x01);
			prop.Text("Crate");
			return prop;
		}());

		Chunk(file, "PROP", [] {
			Blob prop = Property(1, "Name", 0x01);
			prop.Text("Lid");
			return prop;
		}());

		Chunk(file, "PROP", [] {
			Blob prop = Property(1, "Anchored", 0x02);
			prop.U8(1);
			return prop;
		}());

		// An Enum, which is a number naming a member of Roblox's table. Refused,
		// and the two chunks after it must still arrive.
		Chunk(file, "PROP", [] {
			Blob prop = Property(1, "Material", 0x12);
			prop.Transposed({256});
			return prop;
		}());

		Chunk(file, "PROP", [] {
			Blob prop = Property(1, "Size", 0x0E);
			prop.Reals({4.0f});
			prop.Reals({1.0f});
			prop.Reals({2.0f});
			return prop;
		}());

		Chunk(file, "PROP", [] {
			Blob prop = Property(1, "Transparency", 0x04);
			prop.Reals({0.5f});
			return prop;
		}());

		// Red, as three byte planes rather than three float ones.
		Chunk(file, "PROP", [] {
			Blob prop = Property(1, "Color", 0x1A);
			prop.U8(255);
			prop.U8(0);
			prop.U8(0);
			return prop;
		}());

		// One rotation byte — 0x02 is the identity Studio writes for an unrotated
		// part — then the whole chunk's positions, as one transposed array.
		Chunk(file, "PROP", [] {
			Blob prop = Property(1, "CFrame", 0x10);
			prop.U8(0x02);
			prop.Reals({1.0f});
			prop.Reals({2.0f});
			prop.Reals({3.0f});
			return prop;
		}());

		// **A script's source is a `ProtectedString` and not a `String`**, which
		// is a separate type number carrying identical bytes. A reader written
		// from the type list alone knows the second and not the first, and the
		// symptom is every script importing with no program in it.
		Chunk(file, "PROP", [] {
			Blob prop = Property(2, "Name", 0x01);
			prop.Text("Boot");
			return prop;
		}());

		Chunk(file, "PROP", [] {
			Blob prop = Property(2, "Source", 0x1D);
			prop.Text("print('hello')\n");
			return prop;
		}());

		Chunk(file, "PRNT", [] {
			Blob parents;
			parents.U8(0);
			parents.I32(3);
			parents.Referents({0, 1, 2});
			parents.Referents({-1, 0, 0});
			return parents;
		}());

		Chunk(file, END_TAG, {});
		return file;
	}

	std::span<const std::byte> Bytes(const std::vector<uint8_t> &blob) {
		return {reinterpret_cast<const std::byte *>(blob.data()), blob.size()};
	}

	std::span<const std::byte> Bytes(const std::vector<uint8_t> &blob, size_t length) {
		return {reinterpret_cast<const std::byte *>(blob.data()), length};
	}

	bool Noted(const RobloxModel &model, std::string_view fragment) {
		for (const std::string &note : model.Notes) {
			if (note.find(fragment) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	const engine::bake::RobloxValue *Find(const RobloxInstance &instance, std::string_view name) {
		for (const engine::bake::RobloxProperty &property : instance.Properties) {
			if (property.Name == name) {
				return &property.Value;
			}
		}
		return nullptr;
	}
}

TEST_CASE("an rbxm comes back as the tree it describes", "[bake][rbxm]") {
	const Blob file = Fixture();

	RobloxModel model;
	std::string failure;
	REQUIRE(ReadRobloxModel(Bytes(file.Bytes), model, failure));
	CHECK(failure.empty());

	// One root, because the parent table says the Part is inside the Model —
	// which is the only thing a referent is allowed to become.
	REQUIRE(model.Roots.size() == 1);
	const RobloxInstance &crate = model.Roots[0];
	CHECK(crate.ClassName == "Model");
	CHECK(crate.Name == "Crate");

	// **Two children, in the order the parent table listed them.** A tree
	// assembled by walking the flat list would reorder siblings the moment a
	// file declared its classes in another order, and a reordered tree is a
	// different set of entity ids on the far side of the sync.
	REQUIRE(crate.Children.size() == 2);
	const RobloxInstance &lid = crate.Children[0];
	CHECK(lid.ClassName == "Part");
	CHECK(lid.Name == "Lid");

	const RobloxInstance &boot = crate.Children[1];
	CHECK(boot.ClassName == "Script");
	CHECK(boot.Name == "Boot");

	// A script's source is a `ProtectedString`, which is a type number of its
	// own carrying the same bytes a `String` does.
	const engine::bake::RobloxValue *source = Find(boot, "Source");
	REQUIRE(source != nullptr);
	CHECK(source->Kind == RobloxValueKind::Text);
	CHECK(source->Text == "print('hello')\n");

	// **The name is lifted out of the properties rather than left in both**, so
	// nothing downstream has two places to read it from.
	CHECK(Find(lid, "Name") == nullptr);
}

TEST_CASE("every value type this reads comes back as itself", "[bake][rbxm]") {
	const Blob file = Fixture();

	RobloxModel model;
	std::string failure;
	REQUIRE(ReadRobloxModel(Bytes(file.Bytes), model, failure));
	REQUIRE(model.Roots.size() == 1);
	REQUIRE(model.Roots[0].Children.size() == 2);
	const RobloxInstance &lid = model.Roots[0].Children[0];

	const engine::bake::RobloxValue *anchored = Find(lid, "Anchored");
	REQUIRE(anchored != nullptr);
	CHECK(anchored->Kind == RobloxValueKind::Bool);
	CHECK(anchored->Bool);

	// A transposed float array read as a plain one gives numbers that are wrong
	// rather than absent, so the components are pinned individually and are
	// deliberately three different values.
	const engine::bake::RobloxValue *size = Find(lid, "Size");
	REQUIRE(size != nullptr);
	CHECK(size->Kind == RobloxValueKind::Vector3);
	CHECK(size->Vector3.X == Approx(4.0f));
	CHECK(size->Vector3.Y == Approx(1.0f));
	CHECK(size->Vector3.Z == Approx(2.0f));

	const engine::bake::RobloxValue *transparency = Find(lid, "Transparency");
	REQUIRE(transparency != nullptr);
	CHECK(transparency->Kind == RobloxValueKind::Number);
	CHECK(transparency->Number == Approx(0.5));

	// Byte planes on 0..255, not float ones on 0..1.
	const engine::bake::RobloxValue *colour = Find(lid, "Color");
	REQUIRE(colour != nullptr);
	CHECK(colour->Kind == RobloxValueKind::Color3);
	CHECK(colour->Color3.R == Approx(1.0f));
	CHECK(colour->Color3.G == Approx(0.0f));
	CHECK(colour->Color3.B == Approx(0.0f));

	// **The positions live after every rotation, not beside each one.** A reader
	// that expected them interleaved would consume the same number of bytes and
	// put the model somewhere else.
	const engine::bake::RobloxValue *frame = Find(lid, "CFrame");
	REQUIRE(frame != nullptr);
	CHECK(frame->Kind == RobloxValueKind::CFrame);
	CHECK(frame->CFrame.Position.X == Approx(1.0f));
	CHECK(frame->CFrame.Position.Y == Approx(2.0f));
	CHECK(frame->CFrame.Position.Z == Approx(3.0f));
	CHECK(frame->CFrame.QuaternionW == Approx(1.0f));
}

TEST_CASE("a property type this does not decode costs its property only", "[bake][rbxm]") {
	const Blob file = Fixture();

	RobloxModel model;
	std::string failure;
	REQUIRE(ReadRobloxModel(Bytes(file.Bytes), model, failure));
	REQUIRE(model.Roots.size() == 1);
	REQUIRE(model.Roots[0].Children.size() == 2);
	const RobloxInstance &lid = model.Roots[0].Children[0];

	// Named by what it is, so a gap reads as a gap rather than as a property
	// that was never there.
	CHECK(Find(lid, "Material") == nullptr);
	CHECK(Noted(model, "Part.Material is an Enum"));

	// **And the chunks after it arrived**, which is the half that matters: a
	// refusal that consumed the wrong number of bytes would take the rest of the
	// file with it, silently.
	CHECK(Find(lid, "Size") != nullptr);
	CHECK(Find(lid, "CFrame") != nullptr);
}

TEST_CASE("an lz4 chunk inflates, matches included", "[bake][rbxm]") {
	// Every chunk of the fixture through the LZ4 framing, so the two paths are
	// asserted to agree rather than merely both existing.
	Blob file;
	Header(file, 1, 1);
	Lz4Chunk(file, "INST", Instances(0, "Model", {0}));
	Lz4Chunk(file, "PROP", [] {
		Blob prop = Property(0, "Name", 0x01);
		prop.Text("Crate");
		return prop;
	}());
	Lz4Chunk(file, END_TAG, {});

	RobloxModel model;
	std::string failure;
	REQUIRE(ReadRobloxModel(Bytes(file.Bytes), model, failure));
	REQUIRE(model.Roots.size() == 1);
	CHECK(model.Roots[0].Name == "Crate");

	// **A block with a match in it, hand-written**, because an all-literal block
	// exercises none of the copy loop — and the copy loop is where an offset of
	// one has to repeat a byte it is in the middle of producing. This one spells
	// a `Name` of sixteen a's as one literal and a fifteen-long match at offset
	// one, which a block copy would read as uninitialised memory.
	Blob literals = Property(0, "Name", 0x01);
	literals.U32(16);
	literals.Raw("a");

	// Literal length 18, which is past the four bits the token holds, so it is
	// fifteen in the token and three in an extension byte. Match length 15, which
	// the token carries as 15 - 4 because the format's minimum match is four.
	REQUIRE(literals.Bytes.size() == 18);

	Blob block;
	block.U8(0xFB);
	block.U8(0x03);
	block.Bytes.insert(block.Bytes.end(), literals.Bytes.begin(), literals.Bytes.end());
	block.U8(1);
	block.U8(0);

	Blob repeated;
	Header(repeated, 1, 1);
	Lz4Chunk(repeated, "INST", Instances(0, "Model", {0}));
	repeated.Raw("PROP");
	repeated.U32(static_cast<uint32_t>(block.Bytes.size()));
	repeated.U32(static_cast<uint32_t>(literals.Bytes.size() + 15));
	repeated.U32(0);
	repeated.Bytes.insert(repeated.Bytes.end(), block.Bytes.begin(), block.Bytes.end());
	Lz4Chunk(repeated, END_TAG, {});

	RobloxModel overlapped;
	REQUIRE(ReadRobloxModel(Bytes(repeated.Bytes), overlapped, failure));
	REQUIRE(overlapped.Roots.size() == 1);
	CHECK(overlapped.Roots[0].Name == "aaaaaaaaaaaaaaaa");
}

TEST_CASE("a truncated rbxm is refused at every length", "[bake][rbxm]") {
	// **Every prefix, not one.** A model missing its parent table is not a
	// shallower model and one missing half a chunk is not a smaller one — and
	// the interesting lengths are the ones that land inside a length field,
	// which is exactly what a loop finds and a hand-picked case does not.
	const Blob file = Fixture();

	for (size_t length = 0; length < file.Bytes.size(); length++) {
		RobloxModel model;
		std::string failure;
		CHECK_FALSE(ReadRobloxModel(Bytes(file.Bytes, length), model, failure));
		CHECK(model.Roots.empty());
		CHECK_FALSE(failure.empty());
	}
}

TEST_CASE("a count larger than the bytes behind it is refused", "[bake][rbxm]") {
	// The shape every one of these has: a number off the wire, and fewer bytes
	// after it than the number says.

	SECTION("an instance count") {
		Blob file;
		Header(file, 1, 1);
		Chunk(file, "INST", [] {
			Blob payload;
			payload.I32(0);
			payload.Text("Model");
			payload.U8(0);
			payload.I32(1000);
			payload.Referents({0});
			return payload;
		}());
		Chunk(file, END_TAG, {});

		RobloxModel model;
		std::string failure;
		CHECK_FALSE(ReadRobloxModel(Bytes(file.Bytes), model, failure));
		CHECK(failure.find("more instances than it holds") != std::string::npos);
	}

	SECTION("an instance count past the ceiling") {
		// Not a count that outruns this file — one that outruns any file, so it
		// is refused before it is turned into an allocation.
		Blob file;
		Header(file, 1, 1);
		Chunk(file, "INST", [] {
			Blob payload;
			payload.I32(0);
			payload.Text("Model");
			payload.U8(0);
			payload.I32(0x40000000);
			return payload;
		}());
		Chunk(file, END_TAG, {});

		RobloxModel model;
		std::string failure;
		CHECK_FALSE(ReadRobloxModel(Bytes(file.Bytes), model, failure));
		CHECK(failure.find("more instances than this will read") != std::string::npos);
	}

	SECTION("a string length inside a property") {
		Blob file;
		Header(file, 1, 1);
		Chunk(file, "INST", Instances(0, "Model", {0}));
		Chunk(file, "PROP", [] {
			Blob prop = Property(0, "Name", 0x01);
			prop.U32(0x7FFFFFFF);
			prop.Raw("Crate");
			return prop;
		}());
		Chunk(file, END_TAG, {});

		RobloxModel model;
		std::string failure;
		CHECK_FALSE(ReadRobloxModel(Bytes(file.Bytes), model, failure));
		CHECK(failure.find("holds less than its type says") != std::string::npos);
	}

	SECTION("a chunk that inflates to less than it states") {
		// A legal LZ4 block — four literals and nothing else — under a header
		// claiming sixty-four kilobytes. **The refusal has to come from the
		// length rather than from the parse**, because the bytes it produces are
		// perfectly readable and simply are not all of them.
		Blob file;
		Header(file, 1, 1);
		file.Raw("INST");
		file.U32(5);
		file.U32(0x10000);
		file.U32(0);
		file.U8(0x40);
		file.Raw("ABCD");

		RobloxModel model;
		std::string failure;
		CHECK_FALSE(ReadRobloxModel(Bytes(file.Bytes), model, failure));
		CHECK(failure.find("did not inflate") != std::string::npos);
	}

	SECTION("a chunk longer than the file") {
		Blob file;
		Header(file, 1, 1);
		file.Raw("INST");
		file.U32(0);
		file.U32(0x10000);
		file.U32(0);

		RobloxModel model;
		std::string failure;
		CHECK_FALSE(ReadRobloxModel(Bytes(file.Bytes), model, failure));
		CHECK(failure.find("more bytes than the file holds") != std::string::npos);
	}

	SECTION("a chunk that states more than this will ever inflate") {
		Blob file;
		Header(file, 1, 1);
		file.Raw("INST");
		file.U32(4);
		file.U32(0xF0000000);
		file.U32(0);
		file.U32(0);

		RobloxModel model;
		std::string failure;
		CHECK_FALSE(ReadRobloxModel(Bytes(file.Bytes), model, failure));
		CHECK(failure.find("larger than this will inflate") != std::string::npos);
	}
}

TEST_CASE("a parent table that loops is refused rather than followed", "[bake][rbxm]") {
	// Two instances, each inside the other. Assembling that recursively is a
	// stack that runs out with no file named, which is the one failure a reader
	// of somebody else's file must not have.
	Blob file;
	Header(file, 1, 2);
	Chunk(file, "INST", Instances(0, "Model", {0, 1}));
	Chunk(file, "PRNT", [] {
		Blob parents;
		parents.U8(0);
		parents.I32(2);
		parents.Referents({0, 1});
		parents.Referents({1, 0});
		return parents;
	}());
	Chunk(file, END_TAG, {});

	RobloxModel model;
	std::string failure;
	CHECK_FALSE(ReadRobloxModel(Bytes(file.Bytes), model, failure));
	CHECK(failure.find("loops") != std::string::npos);
}

TEST_CASE("something that is not an rbxm is refused by its signature", "[bake][rbxm]") {
	const std::string_view xml = R"(<roblox version="4"><Item class="Part"/></roblox>)";

	RobloxModel model;
	std::string failure;
	CHECK_FALSE(ReadRobloxModel(
		std::span(reinterpret_cast<const std::byte *>(xml.data()), xml.size()), model, failure
	));

	// Named, because an `.rbxmx` renamed to `.rbxm` is the way somebody actually
	// arrives here and "not an rbxm" would not tell them that.
	CHECK(failure.find("wrong signature") != std::string::npos);
}
