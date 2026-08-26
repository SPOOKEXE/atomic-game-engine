#pragma once

// arch-waiver public-header: a fixture leaf. Nothing outside the fixture
// includes anything, so every fixture that is not about this rule says so once.

namespace codec {

	inline void Encode(engine::core::ByteWriter &writer, const engine::core::Name &name) {
		writer.WriteUInt32(name.Id());
	}
}
