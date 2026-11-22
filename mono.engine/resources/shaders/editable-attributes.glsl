// Free float4 shader channels of an editable mesh, read by vertex id.
//
// `render::MeshTable::Attributes` lays the words out: a range count, one
// `{first vertex, vertex count, first value, mask}` row per mesh with channels,
// sorted by first vertex, then sixteen floats per vertex. A vertex outside
// every range, or a channel its mask leaves out, reads as zero.
layout(set = 0, binding = 4, std430) readonly buffer EditableAttributeWords {
	uint words[];
}
editableAttributes;

void LoadEditableAttributes(uint vertex, out vec4 channels[4]) {
	for (int channel = 0; channel < 4; channel++)
		channels[channel] = vec4(0.0);
	uint low = 0u;
	uint high = editableAttributes.words[0];
	while (low < high) {
		uint middle = (low + high) / 2u;
		uint row = 1u + middle * 4u;
		uint first = editableAttributes.words[row];
		if (vertex < first) {
			high = middle;
			continue;
		}
		uint local = vertex - first;
		if (local >= editableAttributes.words[row + 1u]) {
			low = middle + 1u;
			continue;
		}
		uint mask = editableAttributes.words[row + 3u];
		uint base = 1u + editableAttributes.words[0] * 4u + (editableAttributes.words[row + 2u] + local) * 16u;
		for (uint channel = 0u; channel < 4u; channel++) {
			if ((mask & (1u << channel)) == 0u) continue;
			uint at = base + channel * 4u;
			channels[channel] = uintBitsToFloat(uvec4(
				editableAttributes.words[at],
				editableAttributes.words[at + 1u],
				editableAttributes.words[at + 2u],
				editableAttributes.words[at + 3u]
			));
		}
		return;
	}
}
