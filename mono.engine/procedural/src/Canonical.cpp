#include "Encoding.hpp"

#include <engine/procedural/Canonical.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <type_traits>

namespace engine::procedural {

	bool ValidStableName(std::string_view name) {
		if (name.empty()) return false;
		for (size_t at = 0; at < name.size();) {
			const auto first = static_cast<uint8_t>(name[at]);
			if (first < 0x20 || first == 0x7f) return false;
			if (first < 0x80) {
				++at;
				continue;
			}
			const size_t width = first >= 0xf0 && first <= 0xf4	  ? 4
								 : first >= 0xe0 && first <= 0xef ? 3
								 : first >= 0xc2 && first <= 0xdf ? 2
																  : 0;
			if (width == 0 || width > name.size() - at) return false;
			uint32_t scalar = first & (width == 4 ? 0x07 : width == 3 ? 0x0f : 0x1f);
			for (size_t part = 1; part < width; ++part) {
				const auto byte = static_cast<uint8_t>(name[at + part]);
				if ((byte & 0xc0) != 0x80) return false;
				scalar = (scalar << 6) | (byte & 0x3f);
			}
			if (scalar < (width == 4   ? 0x10000u
						  : width == 3 ? 0x800u
									   : 0x80u) ||
				(scalar >= 0x80u && scalar <= 0x9fu) || (scalar >= 0xd800u && scalar <= 0xdfffu) ||
				scalar > 0x10ffffu)
				return false;
			at += width;
		}
		return true;
	}

	bool StableNameLess(std::string_view left, std::string_view right) {
		return std::lexicographical_compare(
			left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) {
				return static_cast<uint8_t>(a) < static_cast<uint8_t>(b);
			}
		);
	}

	namespace {
		bool LinkLess(const OperationLink *left, const OperationLink *right) {
			const std::array<std::string_view, 4> a{
				left->FromNode, left->FromPort, left->ToNode, left->ToPort
			};
			const std::array<std::string_view, 4> b{
				right->FromNode, right->FromPort, right->ToNode, right->ToPort
			};
			for (size_t index = 0; index < a.size(); ++index) {
				if (StableNameLess(a[index], b[index])) return true;
				if (StableNameLess(b[index], a[index])) return false;
			}
			return false;
		}

		bool WriteValue(detail::Writer &writer, const ParameterData &value, const CanonicalLimits &limits) {
			return std::visit(
				[&](const auto &item) -> bool {
					using Type = std::decay_t<decltype(item)>;
					if constexpr (std::is_same_v<Type, bool>) {
						return writer.Text("bool") && writer.U8(item ? 1 : 0);
					} else if constexpr (std::is_same_v<Type, int64_t>) {
						return writer.Text("i64") && writer.U64(std::bit_cast<uint64_t>(item));
					} else if constexpr (std::is_same_v<Type, uint64_t>) {
						return writer.Text("u64") && writer.U64(item);
					} else if constexpr (std::is_same_v<Type, double>) {
						return writer.Text("f64") && writer.Real(item);
					} else if constexpr (std::is_same_v<Type, Vector2> || std::is_same_v<Type, Vector3> ||
										 std::is_same_v<Type, Vector4>) {
						const std::string_view tag = std::is_same_v<Type, Vector2>	 ? "vector2"
													 : std::is_same_v<Type, Vector3> ? "vector3"
																					 : "vector4";
						if (!writer.Text(tag)) return false;
						for (double number : item.Values)
							if (!writer.Real(number)) return false;
						return true;
					} else if constexpr (std::is_same_v<Type, StableText>) {
						return item.Value.size() <= limits.MaximumNameBytes && ValidStableName(item.Value) &&
							   writer.Text("text") && writer.Text(item.Value);
					} else {
						static_assert(std::is_same_v<Type, ByteBuffer>);
						return item.Value.size() <= limits.MaximumValueBytes && writer.Text("bytes") &&
							   writer.U32(static_cast<uint32_t>(item.Value.size())) && writer.Raw(item.Value);
					}
				},
				value
			);
		}

		Status WriteParameters(
			detail::Writer &writer,
			const std::vector<Parameter> &parameters,
			const CanonicalLimits &limits,
			Diagnostic &diagnostic
		) {
			std::vector<const Parameter *> ordered;
			ordered.reserve(parameters.size());
			for (const Parameter &parameter : parameters) {
				if (parameter.Name.size() > limits.MaximumNameBytes || !ValidStableName(parameter.Name))
					return detail::Refuse(diagnostic, Status::InvalidRecipe, "parameter.name");
				ordered.push_back(&parameter);
			}
			std::sort(ordered.begin(), ordered.end(), [](const Parameter *a, const Parameter *b) {
				return StableNameLess(a->Name, b->Name);
			});
			for (size_t index = 1; index < ordered.size(); ++index)
				if (ordered[index - 1]->Name == ordered[index]->Name)
					return detail::Refuse(diagnostic, Status::InvalidRecipe, ordered[index]->Name);
			if (!writer.U16(static_cast<uint16_t>(ordered.size())))
				return detail::Refuse(diagnostic, Status::LimitExceeded, "recipe.bytes");
			for (const Parameter *parameter : ordered) {
				if (!writer.Text(parameter->Name))
					return detail::Refuse(diagnostic, Status::LimitExceeded, parameter->Name);
				if (!WriteValue(writer, parameter->Value, limits))
					return detail::Refuse(diagnostic, Status::InvalidRecipe, parameter->Name);
			}
			return Status::Ok;
		}
	}

	Status EncodeRecipe(
		const Recipe &recipe, std::vector<uint8_t> &out, Diagnostic &diagnostic, const CanonicalLimits &limits
	) {
		if (limits.MaximumParameters > 64 || limits.MaximumNodes > 8 || limits.MaximumLinks > 16 ||
			limits.MaximumNameBytes > 128 || limits.MaximumValueBytes > 4096 || limits.MaximumBytes > 32768 ||
			recipe.Parameters.size() > limits.MaximumParameters ||
			recipe.Nodes.size() > limits.MaximumNodes || recipe.Links.size() > limits.MaximumLinks)
			return detail::Refuse(diagnostic, Status::LimitExceeded, "recipe.parameters");
		if (recipe.SchemaVersion == 0 || recipe.Name.size() > limits.MaximumNameBytes ||
			recipe.Generator.size() > limits.MaximumNameBytes || !ValidStableName(recipe.Name) ||
			!ValidStableName(recipe.Generator))
			return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.identity");
		try {
			size_t parameterCount = recipe.Parameters.size();
			for (const OperationNode &node : recipe.Nodes) {
				if (node.Parameters.size() > limits.MaximumParameters - parameterCount)
					return detail::Refuse(diagnostic, Status::LimitExceeded, "recipe.parameters");
				parameterCount += node.Parameters.size();
			}
			detail::Writer writer{{}, limits.MaximumBytes};
			if (!writer.Raw(std::array<uint8_t, 4>{'R', 'E', 'C', '1'}) || !writer.Text(recipe.Name) ||
				!writer.Text(recipe.Generator) || !writer.U32(recipe.SchemaVersion) ||
				!writer.U64(recipe.RootSeed))
				return detail::Refuse(diagnostic, Status::LimitExceeded, "recipe.bytes");
			const Status globals = WriteParameters(writer, recipe.Parameters, limits, diagnostic);
			if (globals != Status::Ok) return globals;

			std::vector<const OperationNode *> nodes;
			nodes.reserve(recipe.Nodes.size());
			for (const OperationNode &node : recipe.Nodes) {
				if (node.Id.size() > limits.MaximumNameBytes || node.Kind.size() > limits.MaximumNameBytes ||
					!ValidStableName(node.Id) || !ValidStableName(node.Kind))
					return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.node");
				nodes.push_back(&node);
			}
			std::sort(nodes.begin(), nodes.end(), [](const OperationNode *a, const OperationNode *b) {
				return StableNameLess(a->Id, b->Id);
			});
			if (!writer.U16(static_cast<uint16_t>(nodes.size())))
				return detail::Refuse(diagnostic, Status::LimitExceeded, "recipe.bytes");
			for (size_t index = 0; index < nodes.size(); ++index) {
				const OperationNode &node = *nodes[index];
				if (index != 0 && nodes[index - 1]->Id == node.Id)
					return detail::Refuse(diagnostic, Status::InvalidRecipe, node.Id);
				if (!writer.Text(node.Id) || !writer.Text(node.Kind))
					return detail::Refuse(diagnostic, Status::LimitExceeded, node.Id);
				const Status status = WriteParameters(writer, node.Parameters, limits, diagnostic);
				if (status != Status::Ok) return status;
			}

			std::vector<const OperationLink *> links;
			links.reserve(recipe.Links.size());
			for (const OperationLink &link : recipe.Links) {
				const std::array<std::string_view, 4> names{
					link.FromNode, link.FromPort, link.ToNode, link.ToPort
				};
				for (std::string_view name : names)
					if (name.size() > limits.MaximumNameBytes || !ValidStableName(name))
						return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.link");
				links.push_back(&link);
			}
			std::sort(links.begin(), links.end(), LinkLess);
			if (!writer.U16(static_cast<uint16_t>(links.size())))
				return detail::Refuse(diagnostic, Status::LimitExceeded, "recipe.bytes");
			for (size_t index = 0; index < links.size(); ++index) {
				const OperationLink &link = *links[index];
				if (index != 0 && !LinkLess(links[index - 1], links[index]))
					return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.link");
				if (!writer.Text(link.FromNode) || !writer.Text(link.FromPort) || !writer.Text(link.ToNode) ||
					!writer.Text(link.ToPort))
					return detail::Refuse(diagnostic, Status::LimitExceeded, "recipe.link");
			}
			out = std::move(writer.Bytes);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::Refuse(diagnostic, Status::AllocationFailed);
		}
	}
}
