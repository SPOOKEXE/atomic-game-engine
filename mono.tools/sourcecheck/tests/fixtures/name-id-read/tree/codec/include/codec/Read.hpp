#pragma once

// arch-waiver public-header: a fixture leaf. Nothing outside the fixture
// includes anything, so every fixture that is not about this rule says so once.

namespace codec {

	inline engine::core::Name Decode(engine::core::ByteReader &reader) {
		return engine::core::Name::FromId(reader.ReadUInt32());
	}
}
