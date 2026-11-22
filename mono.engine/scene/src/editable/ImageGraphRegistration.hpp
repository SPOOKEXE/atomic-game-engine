#pragma once

#include <engine/core/Bytes.hpp>

namespace engine::scene::detail {
	void WriteImageGraphs(core::ByteWriter &writer, const void *source, size_t count);
	void ReadImageGraphs(core::ByteReader &reader, void *destination, size_t count);
}
