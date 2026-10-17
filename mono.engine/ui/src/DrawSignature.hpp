#pragma once

#include <cstddef>
#include <cstdint>

struct ImDrawData;

namespace engine::ui::draw_signature_detail {
	uint64_t FoldBytes(uint64_t hash, const void *data, size_t size);
	uint64_t DrawGeometrySignature(const ImDrawData *draw);
}
