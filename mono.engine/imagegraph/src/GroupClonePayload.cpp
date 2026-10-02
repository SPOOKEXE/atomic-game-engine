#include "ValuePayload.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <limits>
namespace engine::imagegraph {
	std::optional<uint64_t> ValueClonePayloadBytes(const Value &value) {
		if (!detail::ValidRuntimeValue(value)) return std::nullopt;
		const auto owned = detail::RetainedPayloadBytes(value);
		if (owned > UINT64_MAX - sizeof(Value)) return std::nullopt;
		return sizeof(Value) + owned;
	}
	std::optional<uint64_t> NodeClonePayloadBytes(const Node &node) {
		if (node.Values.size() > UINT64_MAX / sizeof(AuthoredValue) ||
			node.DynamicInputs.size() > Limits::MaximumDynamicInputsPerNode)
			return std::nullopt;
		if (node.InstanceOverrides.size() > Limits::MaximumArrayElements ||
			node.SourceAnimatedInputs.size() > Limits::MaximumArrayElements ||
			node.SourceStaticInputs.size() > Limits::MaximumArrayElements)
			return std::nullopt;
		uint64_t bytes = sizeof(Node);
		const auto add = [&](uint64_t count) {
			if (count > UINT64_MAX - bytes) return false;
			bytes += count;
			return true;
		};
		const auto text = [&](const std::string &value) {
			return value.size() <= Limits::MaximumTextBytes &&
				   add(std::max(value.size(), std::string{}.capacity()));
		};
		if (!text(node.Id) || !text(node.Type) || !text(node.GroupId) || !text(node.InstanceBase) ||
			!add(node.Values.size() * sizeof(AuthoredValue)) ||
			!add(node.DynamicInputs.size() * sizeof(DynamicInput)))
			return std::nullopt;
		if (!add(node.InstanceOverrides.size() * sizeof(std::string))) return std::nullopt;
		for (const auto &port : node.InstanceOverrides)
			if (!text(port)) return std::nullopt;
		if (!add(node.SourceAnimatedInputs.size() * sizeof(std::string))) return std::nullopt;
		for (const auto &port : node.SourceAnimatedInputs)
			if (!text(port)) return std::nullopt;
		if (!add(node.SourceStaticInputs.size() * sizeof(std::string))) return std::nullopt;
		for (const auto &port : node.SourceStaticInputs)
			if (!text(port)) return std::nullopt;
		for (const auto &value : node.Values) {
			const auto size = ValueClonePayloadBytes(value.Data);
			if (!size || !text(value.Port) || !add(*size - sizeof(Value))) return std::nullopt;
		}
		for (const auto &input : node.DynamicInputs) {
			if (!text(input.Id)) return std::nullopt;
			if (input.Default) {
				const auto size = ValueClonePayloadBytes(*input.Default);
				if (!size || !add(*size - sizeof(Value))) return std::nullopt;
			}
		}
		return bytes;
	}
	std::optional<uint64_t> DocumentRetainedPayloadBytes(const Document &document) {
		if (document.Nodes.size() > Limits::MaximumNodes || document.Groups.size() > Limits::MaximumGroups ||
			document.Junctions.size() > Limits::MaximumJunctions ||
			document.Links.size() > Limits::MaximumLinks ||
			document.Keyframes.size() > Limits::MaximumKeyframes ||
			document.Tracks.size() > Limits::MaximumTracks ||
			document.Outputs.size() > Limits::MaximumOutputs)
			return std::nullopt;
		uint64_t bytes = sizeof(Document);
		const auto add = [&](uint64_t amount) {
			if (amount > UINT64_MAX - bytes) return false;
			bytes += amount;
			return true;
		};
		const auto slots = [&](size_t count, size_t item) {
			return count <= UINT64_MAX / item && add(count * item);
		};
		const auto text = [&](const std::string &value) {
			return value.size() <= Limits::MaximumTextBytes && add(value.capacity());
		};
		const auto value = [&](const Value &data) {
			return detail::ValidRuntimeValue(data) && add(detail::RetainedPayloadBytes(data));
		};
		if (!slots(document.Nodes.capacity(), sizeof(Node)) ||
			!slots(document.Groups.capacity(), sizeof(Group)) ||
			!slots(document.Junctions.capacity(), sizeof(Junction)) ||
			!slots(document.Links.capacity(), sizeof(Link)) ||
			!slots(document.Keyframes.capacity(), sizeof(Keyframe)) ||
			!slots(document.Tracks.capacity(), sizeof(AnimationTrack)) ||
			!slots(document.Outputs.capacity(), sizeof(Output)))
			return std::nullopt;
		for (const auto &node : document.Nodes) {
			if (node.Values.size() > Limits::MaximumArrayElements ||
				node.DynamicInputs.size() > Limits::MaximumDynamicInputsPerNode ||
				node.InstanceOverrides.size() > Limits::MaximumArrayElements ||
				node.SourceAnimatedInputs.size() > Limits::MaximumArrayElements ||
				node.SourceStaticInputs.size() > Limits::MaximumArrayElements)
				return std::nullopt;
			if (!text(node.Id) || !text(node.Type) || !text(node.GroupId) || !text(node.InstanceBase) ||
				!slots(node.Values.capacity(), sizeof(AuthoredValue)) ||
				!slots(node.DynamicInputs.capacity(), sizeof(DynamicInput)) ||
				!slots(node.InstanceOverrides.capacity(), sizeof(std::string)) ||
				!slots(node.SourceAnimatedInputs.capacity(), sizeof(std::string)) ||
				!slots(node.SourceStaticInputs.capacity(), sizeof(std::string)))
				return std::nullopt;
			for (const auto &port : node.InstanceOverrides)
				if (!text(port)) return std::nullopt;
			for (const auto &port : node.SourceAnimatedInputs)
				if (!text(port)) return std::nullopt;
			for (const auto &port : node.SourceStaticInputs)
				if (!text(port)) return std::nullopt;
			for (const auto &entry : node.Values)
				if (!text(entry.Port) || !value(entry.Data)) return std::nullopt;
			for (const auto &input : node.DynamicInputs)
				if (!text(input.Id) || (input.Default && !value(*input.Default))) return std::nullopt;
		}
		for (const auto &group : document.Groups) {
			if (group.Ports.size() > Limits::MaximumGroupPorts || !text(group.Id) || !text(group.Name) ||
				!text(group.ParentId) || !text(group.InstanceBase) ||
				!slots(group.Ports.capacity(), sizeof(GroupPort)))
				return std::nullopt;
			for (const auto &port : group.Ports)
				if (!text(port.Id) || !text(port.JunctionId) || !text(port.ControlNodeId))
					return std::nullopt;
		}
		for (const auto &junction : document.Junctions)
			if (!text(junction.Id) || !text(junction.GroupId) ||
				(junction.Default && !value(*junction.Default)))
				return std::nullopt;
		for (const auto &link : document.Links)
			if (!text(link.FromNode) || !text(link.FromPort) || !text(link.ToNode) || !text(link.ToPort))
				return std::nullopt;
		for (const auto &output : document.Outputs)
			if (!text(output.Id) || !text(output.NodeId) || !text(output.Port)) return std::nullopt;
		for (const auto &key : document.Keyframes) {
			if (!text(key.NodeId) || !text(key.Port) || !text(key.Interpolation) || !value(key.Data))
				return std::nullopt;
			if (key.Ease && (!text(key.Ease->InType) || !text(key.Ease->OutType))) return std::nullopt;
			if (key.SourceDriver) {
				if (!ValidKeyframeSourceDriver(*key.SourceDriver)) return std::nullopt;
				if (const auto *curve = std::get_if<KeyframeCurveDriver>(&*key.SourceDriver);
					curve && !add(detail::RetainedPayloadBytes(curve->Data)))
					return std::nullopt;
			}
		}
		for (const auto &track : document.Tracks)
			if (!text(track.NodeId) || !text(track.Port) || !text(track.End)) return std::nullopt;
		if (document.Timeline && !text(document.Timeline->Playback)) return std::nullopt;
		if (document.Project &&
			(!slots(document.Project->Palette.capacity(), sizeof(Colour)) ||
			 !slots(document.Project->PreviewRulers.capacity(), sizeof(PreviewRulerGuide))))
			return std::nullopt;
		return bytes;
	}

}
