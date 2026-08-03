#include "../src/Codec.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.script.codec")

using Catch::Approx;
using engine::script::CODEC_MAX_BYTES;
using engine::script::CODEC_MAX_DEPTH;
using engine::script::CodecStatus;
using engine::script::Decode;
using engine::script::Encode;
using engine::script::ScriptValue;
using engine::script::ValueTag;

namespace {
	ScriptValue Number(double value) {
		ScriptValue result{ValueTag::Number};
		result.Number = value;
		return result;
	}

	ScriptValue Text(std::string value) {
		ScriptValue result{ValueTag::String};
		result.Text = std::move(value);
		return result;
	}

	std::vector<std::byte> Bytes(ScriptValue value) {
		std::vector<std::byte> out;
		REQUIRE(Encode(value, out) == CodecStatus::Ok);
		return out;
	}
}

TEST_CASE("every scalar round-trips", "[codec]") {
	const ScriptValue values[] = {
		ScriptValue{ValueTag::Nil},
		ScriptValue{ValueTag::False},
		Number(0.0),
		Number(-1.5),
		Number(1e300),
		Text(""),
		Text("hello"),
	};

	for (ScriptValue value : values) {
		std::vector<std::byte> bytes;
		REQUIRE(Encode(value, bytes) == CodecStatus::Ok);

		ScriptValue back;
		REQUIRE(Decode(bytes, back) == CodecStatus::Ok);
		CHECK(back == value);
	}
}

TEST_CASE("a boolean keeps its value through both tags", "[codec]") {
	ScriptValue truth{ValueTag::True};
	truth.Boolean = true;

	std::vector<std::byte> bytes;
	REQUIRE(Encode(truth, bytes) == CodecStatus::Ok);

	ScriptValue back;
	REQUIRE(Decode(bytes, back) == CodecStatus::Ok);
	CHECK(back.Tag == ValueTag::True);
	CHECK(back.Boolean);
}

TEST_CASE("a whole number stays eight bytes", "[codec]") {
	// The format must not shorten `1.0` to an integer and leave `1.5` a double.
	// That would make the bytes depend on the value's history rather than on
	// the value, and two VMs would disagree the first time one of them held a
	// number that happened to be whole.
	const std::vector<std::byte> whole = Bytes(Number(1.0));
	const std::vector<std::byte> fractional = Bytes(Number(1.5));

	CHECK(whole.size() == fractional.size());
	CHECK(whole.size() == 1 + sizeof(double));
}

TEST_CASE("map keys are sorted by bytes, whatever order they arrived in", "[codec]") {
	// **The determinism guarantee, and the whole reason `Encode` takes a
	// mutable reference.** A binding walks its table in whatever order its VM
	// offers; the sort is part of the format so neither binding has to
	// remember.
	ScriptValue forwards{ValueTag::Map};
	forwards.Entries.emplace_back("alpha", Number(1.0));
	forwards.Entries.emplace_back("beta", Number(2.0));
	forwards.Entries.emplace_back("gamma", Number(3.0));

	ScriptValue backwards{ValueTag::Map};
	backwards.Entries.emplace_back("gamma", Number(3.0));
	backwards.Entries.emplace_back("alpha", Number(1.0));
	backwards.Entries.emplace_back("beta", Number(2.0));

	CHECK(Bytes(forwards) == Bytes(backwards));

	// And the sort is applied in place, so the tree comes back in written order.
	std::vector<std::byte> out;
	REQUIRE(Encode(backwards, out) == CodecStatus::Ok);
	CHECK(backwards.Entries[0].first == "alpha");
	CHECK(backwards.Entries[2].first == "gamma");
}

TEST_CASE("the sort is by bytes and not by any language's collation", "[codec]") {
	// Lua compares with `strcoll` and JavaScript compares UTF-16 code units, so
	// a non-ASCII key is orderable two ways and neither VM's answer may reach
	// the wire. Byte order is the third answer, and it is the one both agree to.
	ScriptValue first{ValueTag::Map};
	first.Entries.emplace_back("z", Number(1.0));
	first.Entries.emplace_back("\xC3\xA9", Number(2.0)); // "é" in UTF-8

	ScriptValue second{ValueTag::Map};
	second.Entries.emplace_back("\xC3\xA9", Number(2.0));
	second.Entries.emplace_back("z", Number(1.0));

	CHECK(Bytes(first) == Bytes(second));

	// 0x7A ('z') sorts below 0xC3, so "z" comes first by bytes — which is the
	// opposite of what a locale-aware comparison would usually say.
	ScriptValue sorted = first;
	std::vector<std::byte> ignored;
	REQUIRE(Encode(sorted, ignored) == CodecStatus::Ok);
	CHECK(sorted.Entries[0].first == "z");
}

TEST_CASE("arrays and maps nest and round-trip", "[codec]") {
	ScriptValue inner{ValueTag::Array};
	inner.Items.push_back(Number(1.0));
	inner.Items.push_back(Text("two"));
	inner.Items.push_back(ScriptValue{ValueTag::Nil});

	ScriptValue outer{ValueTag::Map};
	outer.Entries.emplace_back("items", inner);
	outer.Entries.emplace_back("count", Number(3.0));

	std::vector<std::byte> bytes;
	REQUIRE(Encode(outer, bytes) == CodecStatus::Ok);

	ScriptValue back;
	REQUIRE(Decode(bytes, back) == CodecStatus::Ok);
	CHECK(back == outer);
}

TEST_CASE("the three value types round-trip exactly", "[codec]") {
	ScriptValue vector{ValueTag::Vector3};
	vector.Vector = engine::core::Vector3{1.5f, -2.25f, 3.0f};

	ScriptValue colour{ValueTag::Color3};
	colour.Colour = engine::core::Color3{0.25f, 0.5f, 0.75f};

	ScriptValue frame{ValueTag::CFrame};
	frame.Frame = engine::core::CFrame::Angles(0.3f, 0.4f, 0.5f);

	for (const ScriptValue &value : {vector, colour, frame}) {
		std::vector<std::byte> bytes;
		ScriptValue copy = value;
		REQUIRE(Encode(copy, bytes) == CodecStatus::Ok);

		ScriptValue back;
		REQUIRE(Decode(bytes, back) == CodecStatus::Ok);
		CHECK(back == value);
	}
}

TEST_CASE("a CFrame is written field by field rather than as its bytes", "[codec]") {
	// Seven floats and a tag. A memcpy of the struct would be the same size
	// today and would change meaning the day a field moved or padding appeared,
	// which would silently reinterpret every old recording.
	ScriptValue frame{ValueTag::CFrame};
	frame.Frame = engine::core::CFrame{};

	CHECK(Bytes(frame).size() == 1 + 7 * sizeof(float));
}

TEST_CASE("nesting past the limit is refused rather than truncated", "[codec]") {
	ScriptValue deep{ValueTag::Nil};
	for (uint32_t level = 0; level <= CODEC_MAX_DEPTH + 1; level++) {
		ScriptValue wrapper{ValueTag::Array};
		wrapper.Items.push_back(deep);
		deep = wrapper;
	}

	std::vector<std::byte> bytes;
	CHECK(Encode(deep, bytes) == CodecStatus::TooDeep);

	// And nothing partial is left behind: a caller that ignored the status must
	// not find half a message it could send.
	CHECK(bytes.empty());
}

TEST_CASE("a payload past the size limit is refused", "[codec]") {
	ScriptValue big{ValueTag::Array};
	for (size_t index = 0; index < CODEC_MAX_BYTES; index++) {
		big.Items.push_back(Number(static_cast<double>(index)));
	}

	std::vector<std::byte> bytes;
	CHECK(Encode(big, bytes) == CodecStatus::TooLarge);
	CHECK(bytes.empty());
}

TEST_CASE("a truncated payload is malformed rather than a partial value", "[codec]") {
	ScriptValue value{ValueTag::Map};
	value.Entries.emplace_back("key", Text("a reasonably long string"));

	std::vector<std::byte> bytes = Bytes(value);
	bytes.resize(bytes.size() - 4);

	ScriptValue back;
	CHECK(Decode(bytes, back) == CodecStatus::Malformed);
	CHECK(back.Tag == ValueTag::Nil);
}

TEST_CASE("a lying length is refused before anything is reserved", "[codec]") {
	// A corrupt payload claiming four billion entries must fail on the check
	// rather than in the allocator. One byte is the least an encoded value can
	// be, so a count larger than the bytes remaining is a lie.
	std::vector<std::byte> bytes;
	bytes.push_back(static_cast<std::byte>(ValueTag::Array));
	for (int index = 0; index < 4; index++) {
		bytes.push_back(static_cast<std::byte>(0xFF));
	}

	ScriptValue back;
	CHECK(Decode(bytes, back) == CodecStatus::Malformed);
}

TEST_CASE("trailing bytes are refused", "[codec]") {
	// Accepting them would let a sender append anything it liked to a
	// well-formed message and have it ignored — a channel, and one nobody
	// audits.
	std::vector<std::byte> bytes = Bytes(Number(1.0));
	bytes.push_back(std::byte{0});

	ScriptValue back;
	CHECK(Decode(bytes, back) == CodecStatus::Malformed);
}

TEST_CASE("an unknown tag is malformed rather than skipped", "[codec]") {
	// This format carries no length in front of a value, so there is no way to
	// step over something unknown. Guessing would decode the rest of the
	// payload as garbage that looks like data.
	std::vector<std::byte> bytes;
	bytes.push_back(static_cast<std::byte>(0x7F));

	ScriptValue back;
	CHECK(Decode(bytes, back) == CodecStatus::Malformed);
}

TEST_CASE("an empty payload is malformed", "[codec]") {
	ScriptValue back;
	CHECK(Decode({}, back) == CodecStatus::Malformed);
}

TEST_CASE("encoding is stable across repeated calls", "[codec]") {
	// What `just determinism` ultimately rests on: the same value produces the
	// same bytes every time, with no dependence on allocation addresses or on
	// how many times the encoder has run.
	ScriptValue value{ValueTag::Map};
	value.Entries.emplace_back("b", Number(2.0));
	value.Entries.emplace_back("a", Text("one"));

	const std::vector<std::byte> first = Bytes(value);
	for (int attempt = 0; attempt < 8; attempt++) {
		CHECK(Bytes(value) == first);
	}
}
