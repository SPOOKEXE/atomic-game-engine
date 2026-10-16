// Typed buffer host ABI v1. Rows 0..15 are readonly slots; rows 16..31 are writable slots.
layout(set = 2, binding = 0, std140) uniform ComputeParameters {
	uvec4 Dispatch;
	uvec4 Buffers[32];
} computeParameters;

const uint COMPUTE_GROUP_SIZE = 64u;
