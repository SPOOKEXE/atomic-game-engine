#include "ShaderResidency.hpp"
namespace engine::assets {
	std::optional<uint64_t> ShaderRetainedPayloadBytes(const ShaderData &data, uint64_t maximumBytes) {
		detail::ShaderByteCounter counter{0, maximumBytes};
		if (!counter.Add(sizeof(data)) || !detail::ShaderFields(data, counter)) return {};
		return counter.Bytes;
	}
}
