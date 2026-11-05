#include <engine/gui/SemanticSnapshotCodec.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <vector>
namespace engine::gui {
	// A private in-process encoding whose bytes are owned by a retained frame.
	bool
	EncodeSemanticSnapshot(const SemanticSnapshot &snapshot, size_t limit, std::vector<std::byte> &output) {
		constexpr size_t fixed =
			3 * sizeof(uint64_t) + 4 * sizeof(float) + sizeof(uint32_t) + 4 + 4 * sizeof(uint32_t);
		if (limit < 8) return false;
		size_t size = 8, accepted = 0;
		for (const auto &node : snapshot.Nodes) {
			if (accepted == 65536) break;
			if (!std::isfinite(node.Bounds.Min.X) || !std::isfinite(node.Bounds.Min.Y) ||
				!std::isfinite(node.Bounds.Max.X) || !std::isfinite(node.Bounds.Max.Y) ||
				static_cast<uint8_t>(node.Role) > static_cast<uint8_t>(SemanticRole::Group) ||
				static_cast<uint8_t>(node.Direction) >
					static_cast<uint8_t>(SemanticTextDirection::RightToLeft) ||
				(static_cast<uint8_t>(node.Actions) & ~uint8_t{7}) != 0)
				return false;
			if (fixed > limit - size) break;
			size_t nodeSize = fixed;
			bool fits = true;
			for (const std::string *text : {&node.Name, &node.Value, &node.Description, &node.Language}) {
				if (text->size() > UINT32_MAX || text->size() > limit - size - nodeSize) {
					fits = false;
					break;
				}
				nodeSize += text->size();
			}
			if (!fits) break;
			size += nodeSize;
			++accepted;
		}
		try {
			std::vector<std::byte> bytes(size);
			size_t at = 0;
			const auto write = [&](const auto &value) {
				std::memcpy(bytes.data() + at, &value, sizeof(value));
				at += sizeof(value);
			};
			const uint32_t count = static_cast<uint32_t>(accepted),
						   truncated = snapshot.Truncated || accepted != snapshot.Nodes.size() ? 1 : 0;
			write(count);
			write(truncated);
			for (size_t index = 0; index < accepted; ++index) {
				const auto &node = snapshot.Nodes[index];
				write(node.Instance.Id);
				write(node.Collector.Id);
				write(node.Parent.Id);
				write(node.Bounds.Min.X);
				write(node.Bounds.Min.Y);
				write(node.Bounds.Max.X);
				write(node.Bounds.Max.Y);
				write(node.ReadingOrder);
				const uint8_t role = static_cast<uint8_t>(node.Role),
							  actions = static_cast<uint8_t>(node.Actions),
							  direction = static_cast<uint8_t>(node.Direction);
				const uint8_t flags = static_cast<uint8_t>(
					(node.Enabled ? 1 : 0) | (node.Selected ? 2 : 0) | (node.Focused ? 4 : 0) |
					(node.Editable ? 8 : 0) | (node.Password ? 16 : 0)
				);
				write(role);
				write(actions);
				write(direction);
				write(flags);
				for (const std::string *text : {&node.Name, &node.Value, &node.Description, &node.Language}) {
					const uint32_t length = static_cast<uint32_t>(text->size());
					write(length);
					if (length) std::memcpy(bytes.data() + at, text->data(), length);
					at += length;
				}
			}
			output = std::move(bytes);
			return true;
		} catch (const std::bad_alloc &) {
			return false;
		}
	}
	bool DecodeSemanticSnapshot(std::span<const std::byte> bytes, size_t budget, SemanticSnapshot &output) {
		constexpr size_t allocationAllowance = 128;
		constexpr size_t fixed =
			3 * sizeof(uint64_t) + 4 * sizeof(float) + sizeof(uint32_t) + 4 + 4 * sizeof(uint32_t);
		size_t at = 0;
		const auto read = [&](auto &value) {
			if (sizeof(value) > bytes.size() - at) return false;
			std::memcpy(&value, bytes.data() + at, sizeof(value));
			at += sizeof(value);
			return true;
		};
		uint32_t count = 0, truncated = 0;
		if (!read(count) || !read(truncated) || truncated > 1 || count > 65536 ||
			count > (bytes.size() - at) / fixed || count > budget / sizeof(SemanticNode))
			return false;
		try {
			SemanticSnapshot result;
			result.Truncated = truncated != 0;
			result.Nodes.reserve(count);
			size_t decoded = result.Nodes.capacity() * sizeof(SemanticNode);
			if (decoded > budget) return false;
			if (count != 0) {
				if (allocationAllowance > budget - decoded) return false;
				decoded += allocationAllowance;
			}
			for (uint32_t i = 0; i < count; ++i) {
				SemanticNode node;
				uint8_t role = 0, actions = 0, direction = 0, flags = 0;
				if (!read(node.Instance.Id) || !read(node.Collector.Id) || !read(node.Parent.Id) ||
					!read(node.Bounds.Min.X) || !read(node.Bounds.Min.Y) || !read(node.Bounds.Max.X) ||
					!read(node.Bounds.Max.Y) || !read(node.ReadingOrder) || !read(role) || !read(actions) ||
					!read(direction) || !read(flags))
					return false;
				if (role > static_cast<uint8_t>(SemanticRole::Group) ||
					direction > static_cast<uint8_t>(SemanticTextDirection::RightToLeft) ||
					(actions & ~uint8_t{7}) || (flags & ~uint8_t{31}) || !std::isfinite(node.Bounds.Min.X) ||
					!std::isfinite(node.Bounds.Min.Y) || !std::isfinite(node.Bounds.Max.X) ||
					!std::isfinite(node.Bounds.Max.Y))
					return false;
				node.Role = static_cast<SemanticRole>(role);
				node.Actions = static_cast<SemanticActionSet>(actions);
				node.Direction = static_cast<SemanticTextDirection>(direction);
				node.Enabled = flags & 1;
				node.Selected = flags & 2;
				node.Focused = flags & 4;
				node.Editable = flags & 8;
				node.Password = flags & 16;
				for (std::string *text : {&node.Name, &node.Value, &node.Description, &node.Language}) {
					uint32_t length = 0;
					if (!read(length) || length > bytes.size() - at || length > budget - decoded)
						return false;
					text->assign(reinterpret_cast<const char *>(bytes.data() + at), length);
					at += length;
					if (allocationAllowance > budget - decoded ||
						text->capacity() > budget - decoded - allocationAllowance)
						return false;
					decoded += text->capacity() + allocationAllowance;
				}
				result.Nodes.push_back(std::move(node));
			}
			if (at != bytes.size()) return false;
			output = std::move(result);
			return true;
		} catch (const std::bad_alloc &) {
			return false;
		}
	}
}
