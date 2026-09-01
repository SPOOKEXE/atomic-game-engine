// Reading Roblox's two model containers, and asserting that they agree.
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
// construction the case pins the value - the transposed arrays are written out
// plane by plane here and read back plane by plane there, and both are asserted
// against numbers a person typed.
//
// **Both containers are in one suite because one case needs both.** `.rbxmx` is
// the same instance tree in XML and has to produce the same `RobloxModel`, so
// the case that matters most is the one that reads a model written both ways and
// compares the trees field by field. Split across two files it would need the
// binary builder copied into the second one, and a copied writer is how the two
// fixtures would quietly stop describing the same model.

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
using engine::bake::ReadRobloxFile;
using engine::bake::ReadRobloxModel;
using engine::bake::ReadRobloxModelXml;
using engine::bake::RobloxInstance;
using engine::bake::RobloxModel;
using engine::bake::RobloxValue;
using engine::bake::RobloxValueKind;

namespace {
	// The tag that ends a file, whose fourth byte is a NUL - so it is spelled
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
	// framing there is - which makes it the right one for asserting that the
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

		// One rotation byte - 0x02 is the identity Studio writes for an unrotated
		// part - then the whole chunk's positions, as one transposed array.
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

	// One root, because the parent table says the Part is inside the Model -
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
	// exercises none of the copy loop - and the copy loop is where an offset of
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
	// shallower model and one missing half a chunk is not a smaller one - and
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
		// Not a count that outruns this file - one that outruns any file, so it
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
		// A legal LZ4 block - four literals and nothing else - under a header
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

// --- the XML container ---------------------------------------------------------
//
// `.rbxmx` is the same instance tree in markup. The cases below are in the order
// the risks are: does it read at all, does it read *the same*, and is the parser
// safe against the three attacks an XML reader has that a binary one does not.

namespace {
	// The same model `Fixture` writes, in the XML container.
	//
	// **Written out rather than generated**, which is the opposite choice to the
	// binary fixture above and is the point of it: a generator that emitted both
	// containers from one description would agree with itself whatever either
	// reader did. Two hand-written files that a person can read and compare are
	// what makes the agreement case mean anything.
	//
	// Every awkward row the binary fixture carries is here in its XML spelling -
	// the `token` that is an enum and is refused, the `Color3uint8` that is one
	// packed number rather than three byte planes, the whole rotation matrix
	// where the binary container writes one byte naming it, and a script's source
	// inside the `CDATA` section that is the only way this format writes a
	// program.
	constexpr std::string_view FIXTURE_RBXMX = R"xml(<?xml version="1.0" encoding="utf-8"?>
<roblox xmlns:xmime="http://www.w3.org/2005/05/xmlmime" version="4">
	<Meta name="ExplicitAutoJoints">true</Meta>
	<External>null</External>
	<External>nil</External>
	<Item class="Model" referent="RBX0">
		<Properties>
			<string name="Name">Crate</string>
		</Properties>
		<Item class="Part" referent="RBX1">
			<Properties>
				<bool name="Anchored">true</bool>
				<token name="Material">256</token>
				<string name="Name">Lid</string>
				<Vector3 name="Size">
					<X>4</X>
					<Y>1</Y>
					<Z>2</Z>
				</Vector3>
				<float name="Transparency">0.5</float>
				<Color3uint8 name="Color">4294901760</Color3uint8>
				<CoordinateFrame name="CFrame">
					<X>1</X>
					<Y>2</Y>
					<Z>3</Z>
					<R00>1</R00>
					<R01>0</R01>
					<R02>0</R02>
					<R10>0</R10>
					<R11>1</R11>
					<R12>0</R12>
					<R20>0</R20>
					<R21>0</R21>
					<R22>1</R22>
				</CoordinateFrame>
			</Properties>
		</Item>
		<Item class="Script" referent="RBX2">
			<Properties>
				<string name="Name">Boot</string>
				<ProtectedString name="Source"><![CDATA[print('hello')
]]></ProtectedString>
			</Properties>
		</Item>
	</Item>
</roblox>
)xml";

	std::span<const std::byte> Bytes(std::string_view text) {
		return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
	}

	// A whole document, so a case can bend one line of the fixture without
	// repeating the rest of it.
	std::string WithProperties(std::string_view properties) {
		return std::string(R"xml(<roblox version="4"><Item class="Part"><Properties>)xml") +
			   std::string(properties) + "</Properties></Item></roblox>";
	}

	RobloxModel ReadXml(std::string_view document) {
		RobloxModel model;
		std::string failure;
		const bool read = ReadRobloxModelXml(Bytes(document), model, failure);
		INFO("failure was: " << failure);
		REQUIRE(read);
		CHECK(failure.empty());
		return model;
	}

	// The refusal's reason, so a case can assert what it names.
	std::string RefusedXml(std::string_view document) {
		RobloxModel model;
		std::string failure;
		REQUIRE_FALSE(ReadRobloxModelXml(Bytes(document), model, failure));
		REQUIRE_FALSE(failure.empty());

		// Left alone on failure, which is the rule every reader here follows.
		CHECK(model.Roots.empty());
		return failure;
	}

	bool Mentions(const std::string &failure, std::string_view word) {
		INFO("failure was: " << failure);
		return failure.find(word) != std::string::npos;
	}

	// Two values, compared on the field their kind says is meaningful.
	//
	// **Exact rather than approximate**, because the two containers are being
	// asserted to agree rather than to be close: they take the same bytes to the
	// same `float` through the same conversions, and a tolerance here would hide
	// the day one of them stopped doing that.
	bool Same(const RobloxValue &left, const RobloxValue &right) {
		if (left.Kind != right.Kind) {
			return false;
		}

		switch (left.Kind) {
		case RobloxValueKind::Bool:
			return left.Bool == right.Bool;
		case RobloxValueKind::Integer:
			return left.Integer == right.Integer;
		case RobloxValueKind::Number:
			return left.Number == right.Number;
		case RobloxValueKind::Text:
			return left.Text == right.Text;
		case RobloxValueKind::Vector3:
			return left.Vector3 == right.Vector3;
		case RobloxValueKind::Vector2:
			return left.Vector2 == right.Vector2;
		case RobloxValueKind::Color3:
			return left.Color3 == right.Color3;
		case RobloxValueKind::CFrame:
			return left.CFrame.Position == right.CFrame.Position &&
				   left.CFrame.QuaternionX == right.CFrame.QuaternionX &&
				   left.CFrame.QuaternionY == right.CFrame.QuaternionY &&
				   left.CFrame.QuaternionZ == right.CFrame.QuaternionZ &&
				   left.CFrame.QuaternionW == right.CFrame.QuaternionW;
		case RobloxValueKind::UDim:
			return left.UDim == right.UDim;
		case RobloxValueKind::UDim2:
			return left.UDim2 == right.UDim2;
		case RobloxValueKind::Rect:
			return left.Rect == right.Rect;
		case RobloxValueKind::NumberRange:
			return left.NumberRange == right.NumberRange;
		}
		return false;
	}

	bool Same(const RobloxInstance &left, const RobloxInstance &right) {
		INFO(
			"comparing " << left.ClassName << " '" << left.Name << "' with " << right.ClassName << " '"
						 << right.Name << "'"
		);

		if (left.ClassName != right.ClassName || left.Name != right.Name ||
			left.Properties.size() != right.Properties.size() ||
			left.Children.size() != right.Children.size()) {
			return false;
		}

		for (size_t index = 0; index < left.Properties.size(); index++) {
			INFO("property " << left.Properties[index].Name);
			if (left.Properties[index].Name != right.Properties[index].Name ||
				!Same(left.Properties[index].Value, right.Properties[index].Value)) {
				return false;
			}
		}

		for (size_t index = 0; index < left.Children.size(); index++) {
			if (!Same(left.Children[index], right.Children[index])) {
				return false;
			}
		}
		return true;
	}
}

TEST_CASE("an rbxmx comes back as the tree it describes", "[bake][rbxmx]") {
	const RobloxModel model = ReadXml(FIXTURE_RBXMX);

	// One root, and the nesting of the markup is the shape of the tree - which
	// is the whole of what a referent is allowed to become, and here it is not
	// even that: the `referent` attributes are never read.
	REQUIRE(model.Roots.size() == 1);
	const RobloxInstance &crate = model.Roots[0];
	CHECK(crate.ClassName == "Model");
	CHECK(crate.Name == "Crate");

	REQUIRE(crate.Children.size() == 2);
	const RobloxInstance &lid = crate.Children[0];
	CHECK(lid.ClassName == "Part");
	CHECK(lid.Name == "Lid");

	// **The name is lifted out of the properties**, exactly as the binary reader
	// lifts it, so nothing downstream has two places to read it from.
	CHECK(Find(lid, "Name") == nullptr);

	const RobloxValue *size = Find(lid, "Size");
	REQUIRE(size != nullptr);
	CHECK(size->Kind == RobloxValueKind::Vector3);
	CHECK(size->Vector3.X == Approx(4.0f));
	CHECK(size->Vector3.Y == Approx(1.0f));
	CHECK(size->Vector3.Z == Approx(2.0f));

	// One packed number, alpha in the top byte and discarded.
	const RobloxValue *colour = Find(lid, "Color");
	REQUIRE(colour != nullptr);
	CHECK(colour->Kind == RobloxValueKind::Color3);
	CHECK(colour->Color3.R == Approx(1.0f));
	CHECK(colour->Color3.G == Approx(0.0f));
	CHECK(colour->Color3.B == Approx(0.0f));

	// **A script's source is a CDATA section**, which is this container's
	// equivalent of the binary one's `ProtectedString` type number: a reader that
	// treated it as ordinary character data would import every script with
	// `<![CDATA[` in front of its first line.
	const RobloxInstance &boot = crate.Children[1];
	CHECK(boot.ClassName == "Script");
	CHECK(boot.Name == "Boot");

	const RobloxValue *source = Find(boot, "Source");
	REQUIRE(source != nullptr);
	CHECK(source->Kind == RobloxValueKind::Text);
	CHECK(source->Text == "print('hello')\n");
}

TEST_CASE("an rbxmx and an rbxm of one model come back the same", "[bake][rbxmx]") {
	// **The case that keeps the two readers honest.** Everything downstream -
	// `studio::RojoSync`'s class lookup, its property conversion, its source
	// staging - is written against one `RobloxModel`, so the day the two
	// containers stop producing the same one, half of it starts behaving
	// differently depending on which file an author exported.
	const Blob binary = Fixture();

	RobloxModel fromBinary;
	std::string failure;
	REQUIRE(ReadRobloxModel(Bytes(binary.Bytes), fromBinary, failure));

	const RobloxModel fromXml = ReadXml(FIXTURE_RBXMX);

	REQUIRE(fromBinary.Roots.size() == 1);
	REQUIRE(fromXml.Roots.size() == 1);
	CHECK(Same(fromBinary.Roots[0], fromXml.Roots[0]));

	// And the refusal is the same refusal, not merely a refusal each: both
	// containers carry the same enum and both name it.
	CHECK(Noted(fromBinary, "Part.Material is an Enum"));
	CHECK(Noted(fromXml, "Part.Material is an Enum"));
}

TEST_CASE("an rbxmx entity declaration is refused outright", "[bake][rbxmx]") {
	// **The billion laughs**, which is what an XML reader has instead of a
	// decompression bomb: a kilobyte of declarations that expands into gigabytes
	// while it is being parsed. There is no bound that makes this safe and
	// nothing a model needs it for, so the declaration itself is the refusal.
	const std::string bomb =
		R"xml(<?xml version="1.0"?><!DOCTYPE lolz [<!ENTITY lol "lol"><!ENTITY lol2 ")xml"
		R"xml(&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;">]>)xml" +
		WithProperties(R"xml(<string name="Name">&lol2;</string>)xml");

	CHECK(Mentions(RefusedXml(bomb), "DOCTYPE"));
	CHECK(Mentions(RefusedXml(bomb), "ENTITY"));

	// An external entity is the same declaration and the same refusal - and it
	// is a file read, which is the thing this whole module is arranged never to
	// do.
	const std::string external = R"xml(<!DOCTYPE roblox [<!ENTITY xxe SYSTEM "file:///etc/passwd">]>)xml" +
								 WithProperties(R"xml(<string name="Name">&xxe;</string>)xml");
	CHECK(Mentions(RefusedXml(external), "DOCTYPE"));

	// **The second lock, and it is the one that has to hold on its own.** No
	// declaration survives the scanner, so a reference to anything but the five
	// predefines names something nobody could have declared - refused where it
	// would have been expanded rather than dropped, because a dropped one makes
	// a bomb look like a file with a typo in it.
	CHECK(Mentions(RefusedXml(WithProperties(R"xml(<string name="Name">&xxe;</string>)xml")), "xxe"));

	// And in an attribute, which is the half a content-only check would miss.
	CHECK(Mentions(RefusedXml(R"xml(<roblox version="4"><Item class="&xxe;"/></roblox>)xml"), "xxe"));

	// The five that are read, and a numeric character reference, which expands
	// to exactly one character however many of them there are.
	const RobloxModel model =
		ReadXml(WithProperties(R"xml(<string name="Name">a &lt;b&gt; &amp; &#99;</string>)xml"));
	REQUIRE(model.Roots.size() == 1);
	CHECK(model.Roots[0].Name == "a <b> & c");
}

TEST_CASE("an rbxmx script's ampersand is source and not a reference", "[bake][rbxmx]") {
	// **The case that goes red if somebody gives this reader `Svg.cpp`'s
	// document-wide sweep.** An SVG never unescapes, so a reference there has to
	// be caught by sweeping the whole file; a model holds CDATA, and CDATA is
	// text. A real model in this repository's corpus carries the Luau pattern
	// `"[&;]"` inside a script - a sweep refuses that file while naming an entity
	// nobody wrote, which is a valid model rejected for a reason its author
	// cannot act on.
	//
	// So the refusal lives at each point a reference is actually read, with CDATA
	// exempt. `core/Xml.hpp` carries both halves and says what breaks either way
	// round; this is the half that has to hold here.
	const RobloxModel model = ReadXml(WithProperties(
		"<ProtectedString name=\"Source\"><![CDATA[local mask = \"[&;]\"\nlocal both = a and b]]>"
		"</ProtectedString>"
	));

	REQUIRE(model.Roots.size() == 1);
	const RobloxValue *source = Find(model.Roots[0], "Source");
	REQUIRE(source != nullptr);
	CHECK(source->Text == "local mask = \"[&;]\"\nlocal both = a and b");

	// And outside a CDATA section the same document is refused, so what is exempt
	// is CDATA rather than this format.
	CHECK(Mentions(RefusedXml(WithProperties(R"xml(<string name="Name">a &mask; b</string>)xml")), "mask"));
}

TEST_CASE("an rbxmx nested past what this reads is refused rather than followed", "[bake][rbxmx]") {
	// **Unbounded nesting is the third attack, and a parser that recursed would
	// meet it as a stack overflow with no file named.** Nothing in this reader
	// recurses: the walk keeps its own stack and the bound is a count on it, so
	// this is a refusal rather than a crash.
	constexpr int DEEP = 5000;

	std::string nested = R"xml(<roblox version="4">)xml";
	for (int level = 0; level < DEEP; level++) {
		nested += R"xml(<Item class="Folder">)xml";
	}
	for (int level = 0; level < DEEP; level++) {
		nested += "</Item>";
	}
	nested += "</roblox>";
	CHECK(Mentions(RefusedXml(nested), "deeper"));

	// The same bound over elements that are not instances, because a document is
	// free to nest anything.
	std::string markup = R"xml(<roblox version="4">)xml";
	for (int level = 0; level < DEEP; level++) {
		markup += "<a>";
	}
	markup += "</roblox>";
	CHECK(Mentions(RefusedXml(markup), "deeper"));

	// And inside one property's value, where the bound is tighter still: a
	// `Rect2D` is the deepest value this format has and it is two levels.
	std::string value = R"xml(<Vector3 name="Size">)xml";
	for (int level = 0; level < DEEP; level++) {
		value += "<a>";
	}
	value += "</Vector3>";
	CHECK(Mentions(RefusedXml(WithProperties(value)), "deeper"));
}

TEST_CASE("a value this cannot read costs its property and not its file", "[bake][rbxmx]") {
	// A refused type and a malformed value, each between two properties that
	// must still arrive. **Both are here rather than in cases of their own**,
	// because what is being asserted is the same fact twice: an element carries
	// its own end tag, so nothing after one depends on what was inside it.
	const RobloxModel model = ReadXml(WithProperties(
		R"xml(<float name="Reflectance">0.25</float>)xml"
		R"xml(<token name="Material">256</token>)xml"
		R"xml(<Ref name="PrimaryPart">RBX7</Ref>)xml"
		R"xml(<Vector3 name="Size"><X>4</X></Vector3>)xml"
		R"xml(<Font name="FontFace"><Family>rbxasset://x</Family></Font>)xml"
		R"xml(<bool name="Anchored">true</bool>)xml"
	));

	REQUIRE(model.Roots.size() == 1);
	const RobloxInstance &part = model.Roots[0];

	// An enum and a reference are refusals of principle rather than of effort,
	// and both are named - the same words the binary reader uses, so one refusal
	// reads the same whichever container it came out of.
	CHECK(Find(part, "Material") == nullptr);
	CHECK(Noted(model, "Part.Material is an Enum"));
	CHECK(Find(part, "PrimaryPart") == nullptr);
	CHECK(Noted(model, "Part.PrimaryPart is a reference"));
	CHECK(Noted(model, "Part.FontFace is a Font"));

	// **A malformed value costs its property here, where the binary reader has
	// to refuse the file.** A `Vector3` missing two of its three components is
	// not a `Vector3`, and this says so and carries on.
	CHECK(Find(part, "Size") == nullptr);
	CHECK(Noted(model, "Part.Size does not hold a Vector3"));

	// The two either side of all of it.
	CHECK(Find(part, "Reflectance") != nullptr);
	CHECK(Find(part, "Anchored") != nullptr);
}

TEST_CASE("the value types an rbxmx spells differently come back the same", "[bake][rbxmx]") {
	const RobloxModel model = ReadXml(WithProperties(
		R"xml(<UDim name="CornerRadius"><S>0.5</S><O>8</O></UDim>)xml"
		R"xml(<UDim2 name="Position"><XS>0.25</XS><XO>4</XO><YS>0.75</YS><YO>-6</YO></UDim2>)xml"
		R"xml(<Rect2D name="SliceCenter"><min><X>1</X><Y>2</Y></min><max><X>3</X><Y>4</Y></max></Rect2D>)xml"
		R"xml(<NumberRange name="Lifetime">1 1.5 </NumberRange>)xml"
		R"xml(<Vector2 name="SpreadAngle"><X>5</X><Y>6</Y></Vector2>)xml"
		R"xml(<Color3 name="Color3"><R>0.5</R><G>0.25</G><B>0</B></Color3>)xml"
		R"xml(<int64 name="SourceAssetId">-1</int64>)xml"
		R"xml(<double name="Weight">2.5</double>)xml"
		R"xml(<Content name="Texture"><url>rbxassetid://1</url></Content>)xml"
		R"xml(<Content name="LinkedSource"><null></null></Content>)xml"
		R"xml(<BinaryString name="Tags">aGk=</BinaryString>)xml"
	));

	REQUIRE(model.Roots.size() == 1);
	const RobloxInstance &part = model.Roots[0];

	const RobloxValue *corner = Find(part, "CornerRadius");
	REQUIRE(corner != nullptr);
	CHECK(corner->Kind == RobloxValueKind::UDim);
	CHECK(corner->UDim.Scale == Approx(0.5f));
	CHECK(corner->UDim.Offset == Approx(8.0f));

	const RobloxValue *position = Find(part, "Position");
	REQUIRE(position != nullptr);
	CHECK(position->Kind == RobloxValueKind::UDim2);
	CHECK(position->UDim2.X.Scale == Approx(0.25f));
	CHECK(position->UDim2.Y.Offset == Approx(-6.0f));

	// `min` and `max` are child elements of their own, which is the one value
	// here that nests twice.
	const RobloxValue *slice = Find(part, "SliceCenter");
	REQUIRE(slice != nullptr);
	CHECK(slice->Kind == RobloxValueKind::Rect);
	CHECK(slice->Rect.Min.X == Approx(1.0f));
	CHECK(slice->Rect.Max.Y == Approx(4.0f));

	// Two numbers in the element's own text, with the trailing space Studio
	// always writes.
	const RobloxValue *lifetime = Find(part, "Lifetime");
	REQUIRE(lifetime != nullptr);
	CHECK(lifetime->Kind == RobloxValueKind::NumberRange);
	CHECK(lifetime->NumberRange.Minimum == Approx(1.0f));
	CHECK(lifetime->NumberRange.Maximum == Approx(1.5f));

	const RobloxValue *assetId = Find(part, "SourceAssetId");
	REQUIRE(assetId != nullptr);
	CHECK(assetId->Kind == RobloxValueKind::Integer);
	CHECK(assetId->Integer == -1);

	// **`Content` and `BinaryString` are read rather than refused**, because the
	// binary container stores both as a plain `String` - refusing them here would
	// make a `Decal` lose the texture the same model keeps as a `.rbxm`.
	const RobloxValue *texture = Find(part, "Texture");
	REQUIRE(texture != nullptr);
	CHECK(texture->Text == "rbxassetid://1");
	CHECK(Find(part, "LinkedSource")->Text.empty());
	CHECK(Find(part, "Tags")->Text == "hi");
}

TEST_CASE("a shared string is resolved out of the table that follows it", "[bake][rbxmx]") {
	// **The table is written after every instance that refers to it**, which is
	// why this container needs a pass of its own to find it - and why a reader
	// that resolved as it went would hand back the key instead of the value.
	const RobloxModel model = ReadXml(
		R"xml(<roblox version="4"><Item class="Part"><Properties>)xml"
		R"xml(<SharedString name="PhysicalConfigData">a2V5</SharedString>)xml"
		R"xml(</Properties></Item>)xml"
		R"xml(<SharedStrings><SharedString md5="a2V5">aGVsbG8=</SharedString></SharedStrings>)xml"
		R"xml(</roblox>)xml"
	);

	REQUIRE(model.Roots.size() == 1);
	const RobloxValue *shared = Find(model.Roots[0], "PhysicalConfigData");
	REQUIRE(shared != nullptr);
	CHECK(shared->Kind == RobloxValueKind::Text);
	CHECK(shared->Text == "hello");

	// The table itself is not an instance, however much it looks like one from
	// the outside.
	CHECK(model.Roots.size() == 1);
}

TEST_CASE("a truncated rbxmx is refused at every length", "[bake][rbxmx]") {
	// **Every prefix, not one.** The interesting lengths are the ones that land
	// inside a tag, inside an attribute's quotes and inside a CDATA section, and
	// a loop finds all three where a hand-picked case finds none of them.
	for (size_t length = 0; length < FIXTURE_RBXMX.size() - 1; length++) {
		RobloxModel model;
		std::string failure;
		CHECK_FALSE(ReadRobloxModelXml(Bytes(FIXTURE_RBXMX.substr(0, length)), model, failure));
		CHECK(model.Roots.empty());
		CHECK_FALSE(failure.empty());
	}
}

TEST_CASE("each container refuses the other by name", "[bake][rbxmx]") {
	// A renamed file is how somebody actually arrives here, so each reader names
	// the container it was actually given rather than saying the file is broken.
	const Blob binary = Fixture();

	RobloxModel model;
	std::string failure;
	CHECK_FALSE(ReadRobloxModelXml(Bytes(binary.Bytes), model, failure));
	CHECK(Mentions(failure, "binary .rbxm container"));

	CHECK_FALSE(ReadRobloxModel(Bytes(FIXTURE_RBXMX), model, failure));
	CHECK(Mentions(failure, "wrong signature"));
}

TEST_CASE("an rbxmx this cannot trust is refused rather than half-read", "[bake][rbxmx]") {
	// A version this does not know, refused rather than guessed - the same
	// answer the binary reader gives to anything but version 0.
	CHECK(Mentions(RefusedXml(R"xml(<roblox version="3"><Item class="Part"/></roblox>)xml"), "version"));
	CHECK(Mentions(RefusedXml(R"xml(<roblox><Item class="Part"/></roblox>)xml"), "version"));

	// Something that is not this format at all.
	CHECK(Mentions(RefusedXml(R"xml(<svg width="4" height="4"/>)xml"), "not <roblox>"));

	// A closing tag naming something other than what is open, which is where a
	// scanner that only counted depth would build a tree out of the wrong
	// nesting.
	CHECK(Mentions(
		RefusedXml(R"xml(<roblox version="4"><Item class="Part"></Properties></roblox>)xml"), "closes"
	));

	// An instance with no class, which is a row of the file naming nothing.
	CHECK(Mentions(RefusedXml(R"xml(<roblox version="4"><Item/></roblox>)xml"), "no class"));

	// An unquoted attribute value, which is where a scanner that stopped at the
	// first space would read the rest of the tag as an attribute.
	CHECK(Mentions(RefusedXml(R"xml(<roblox version=4/>)xml"), "unquoted"));
}

TEST_CASE("an rbxmx may hold any number of instances at its top level", "[bake][rbxmx]") {
	// **The reader's answer and the studio's are different on purpose.** The
	// container allows any number and Rojo's file table maps a model file to
	// one, so which count is acceptable is a question for whoever asked rather
	// than for the reader - exactly as it is for the binary container.
	const RobloxModel model = ReadXml(
		R"xml(<roblox version="4"><Item class="Model"><Properties>)xml"
		R"xml(<string name="Name">First</string></Properties></Item>)xml"
		R"xml(<Item class="Model"><Properties><string name="Name">Second</string>)xml"
		R"xml(</Properties></Item></roblox>)xml"
	);

	REQUIRE(model.Roots.size() == 2);
	CHECK(model.Roots[0].Name == "First");
	CHECK(model.Roots[1].Name == "Second");

	// An empty container is empty rather than an error. What to do about a model
	// file with nothing in it is the caller's question too.
	const RobloxModel empty = ReadXml(R"xml(<roblox version="4"/>)xml");
	CHECK(empty.Roots.empty());
}

TEST_CASE("the complete roblox reader analyzes assets scripts and lost properties", "[bake][rbxl]") {
	const std::string_view document = R"xml(
		<roblox version="4">
			<Item class="Animation" referent="RBX0">
				<Properties>
					<string name="Name">Slash</string>
					<Content name="AnimationId"><url>rbxassetid://12345</url></Content>
					<NumberSequence name="Curve">0 1 0 1 2 0</NumberSequence>
				</Properties>
				<Item class="LocalScript" referent="RBX1">
					<Properties>
						<string name="Name">Driver</string>
						<ProtectedString name="Source"><![CDATA[
							local texture = "https://www.roblox.com/asset/?id=67890"
						]]></ProtectedString>
					</Properties>
				</Item>
			</Item>
		</roblox>
	)xml";

	RobloxModel model;
	std::string failure;
	REQUIRE(ReadRobloxFile(Bytes(document), model, failure));
	CHECK(failure.empty());
	REQUIRE(model.Roots.size() == 1);
	CHECK(model.Roots[0].Name == "Slash");
	REQUIRE(model.Scripts.size() == 1);
	CHECK(model.Scripts[0].InstancePath == "Slash/Driver");
	CHECK(Mentions(model.Scripts[0].Source, "67890"));

	REQUIRE(model.Assets.size() == 2);
	CHECK(model.Assets[0].Identifier == "12345");
	CHECK(model.Assets[0].Kind == engine::bake::RobloxAssetKind::Animation);
	CHECK(model.Assets[1].Identifier == "67890");
	CHECK(model.Assets[1].InstancePath == "Slash/Driver");

	REQUIRE(model.LostProperties.size() == 1);
	CHECK(model.LostProperties[0].PropertyName == "Curve");
	CHECK(model.LostProperties[0].RobloxType == "NumberSequence");
}

TEST_CASE("the complete roblox reader sniffs binary and leaves output alone on failure", "[bake][rbxl]") {
	const Blob binary = Fixture();
	RobloxModel model;
	std::string failure;
	REQUIRE(ReadRobloxFile(Bytes(binary.Bytes), model, failure));
	CHECK_FALSE(model.Roots.empty());

	RobloxModel sentinel;
	sentinel.Notes.push_back("unchanged");
	CHECK_FALSE(ReadRobloxFile(Bytes(std::string_view("not a place")), sentinel, failure));
	REQUIRE(sentinel.Notes.size() == 1);
	CHECK(sentinel.Notes[0] == "unchanged");
	CHECK_FALSE(failure.empty());
}
