#include "Inputs.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Reference.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace engine::imagegraph {
	namespace {
		bool InputResolutionFailure(Diagnostic &diagnostic, std::string_view node, std::string_view message) {
			diagnostic = {std::string(node.substr(0, 128)), std::string(message.substr(0, 4096))};
			return false;
		}
		bool NodeIdentityValid(std::string_view name) {
			return !name.empty() && name.size() <= 128 &&
				   std::none_of(name.begin(), name.end(), [](unsigned char ch) { return ch < 32; });
		}
		bool InputValueValid(const InputValue &value) {
			if (value.valueless_by_exception()) return false;
			if (const auto *number = std::get_if<double>(&value)) return std::isfinite(*number);
			if (const auto *text = std::get_if<std::string>(&value))
				return text->size() <= 4096 &&
					   std::none_of(text->begin(), text->end(), [](unsigned char ch) { return ch < 32; });
			return true;
		}
		// All conversion guards run before assigning a typed control. Invalid resolved
		// ranges (opacity, scale, relative source path) are checked by CompileResolved.
		bool Apply(Operation &operation, std::string_view property, const InputValue &input) {
			if (PropertyType(operation, property) != static_cast<int>(input.index())) return false;
			return std::visit(
				[&](auto &value) {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, Source>) {
						if (property == "path") {
							value.Path = std::get<std::string>(input);
							return true;
						}
					}
					if constexpr (std::is_same_v<T, Solid> || std::is_same_v<T, Resize> ||
								  std::is_same_v<T, Crop> || std::is_same_v<T, Transform>) {
						if (property == "width" || property == "height") {
							const double number = std::get<double>(input);
							if (number < 1 || number > Limits::MaximumDimension ||
								std::trunc(number) != number)
								return false;
							(property == "width" ? value.Width : value.Height) =
								static_cast<uint32_t>(number);
							return true;
						}
					}
					if constexpr (std::is_same_v<T, Solid>) {
						if (property == "colour") {
							value.Colour = std::get<std::array<uint8_t, 4>>(input);
							return true;
						}
					}
					if constexpr (std::is_same_v<T, Resize> || std::is_same_v<T, Transform>) {
						if (property == "filter") {
							const auto &filter = std::get<std::string>(input);
							if (filter != "nearest" && filter != "bilinear") return false;
							value.Filter = filter == "nearest" ? Sampling::Nearest : Sampling::Bilinear;
							return true;
						}
					}
					if constexpr (std::is_same_v<T, Crop>) {
						if (property == "x" || property == "y") {
							const double number = std::get<double>(input);
							if (number < INT32_MIN || number > INT32_MAX || std::trunc(number) != number)
								return false;
							(property == "x" ? value.X : value.Y) = static_cast<int32_t>(number);
							return true;
						}
					}
					if constexpr (std::is_same_v<T, Transform>) {
						double *control = nullptr;
						if (property == "translate_x") control = &value.TranslateX;
						if (property == "translate_y") control = &value.TranslateY;
						if (property == "scale_x") control = &value.ScaleX;
						if (property == "scale_y") control = &value.ScaleY;
						if (property == "degrees") control = &value.Degrees;
						if (property == "pivot_x") control = &value.PivotX;
						if (property == "pivot_y") control = &value.PivotY;
						if (control) {
							*control = std::get<double>(input);
							return true;
						}
					}
					if constexpr (std::is_same_v<T, Flip>) {
						if (property == "horizontal" || property == "vertical") {
							(property == "horizontal" ? value.Horizontal : value.Vertical) =
								std::get<bool>(input);
							return true;
						}
					}
					if constexpr (std::is_same_v<T, Blend>) {
						if (property == "opacity") {
							value.Opacity = std::get<double>(input);
							return true;
						}
					}
					return false;
				},
				operation
			);
		}
	}
	int PropertyType(const Operation &operation, std::string_view property) noexcept {
		if (operation.valueless_by_exception()) return -1;
		return std::visit(
			[&](const auto &value) -> int {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, Source>) {
					if (property == "path") return 3;
				}
				if constexpr (std::is_same_v<T, Solid> || std::is_same_v<T, Resize> ||
							  std::is_same_v<T, Crop> || std::is_same_v<T, Transform>) {
					if (property == "width" || property == "height") return 0;
				}
				if constexpr (std::is_same_v<T, Solid>) {
					if (property == "colour") return 2;
				}
				if constexpr (std::is_same_v<T, Resize> || std::is_same_v<T, Transform>) {
					if (property == "filter") return 3;
				}
				if constexpr (std::is_same_v<T, Crop>) {
					if (property == "x" || property == "y") return 0;
				}
				if constexpr (std::is_same_v<T, Transform>) {
					if (property == "translate_x" || property == "translate_y" || property == "scale_x" ||
						property == "scale_y" || property == "degrees" || property == "pivot_x" ||
						property == "pivot_y")
						return 0;
				}
				if constexpr (std::is_same_v<T, Flip>) {
					if (property == "horizontal" || property == "vertical") return 1;
				}
				if constexpr (std::is_same_v<T, Blend>) {
					if (property == "opacity") return 0;
				}
				return -1;
			},
			operation
		);
	}
	bool ResolveInputs(
		const Document &document,
		std::span<const InputOverride> overrides,
		Document &out,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph input resolution");
		if (document.Nodes.size() > Limits::MaximumNodes ||
			document.Outputs.size() > Limits::MaximumOutputs ||
			document.Parameters.size() > Limits::MaximumParameters ||
			document.Bindings.size() > Limits::MaximumBindings ||
			overrides.size() > Limits::MaximumParameters)
			return InputResolutionFailure(diagnostic, {}, "graph input or document count exceeds limits");
		try {
			std::unordered_map<std::string_view, const InputValue *> values;
			size_t bytes = 0;
			for (const auto &parameter : document.Parameters) {
				if (!IsReferenceToken(parameter.Name) || !InputValueValid(parameter.Default) ||
					!values.emplace(parameter.Name, &parameter.Default).second)
					return InputResolutionFailure(
						diagnostic, {}, "input name/default is invalid or duplicated"
					);
				bytes += parameter.Name.size();
				if (const auto *text = std::get_if<std::string>(&parameter.Default)) bytes += text->size();
			}
			std::unordered_set<std::string_view> overridden;
			for (const auto &override : overrides) {
				const auto found = values.find(override.Name);
				if (found == values.end() || !overridden.insert(override.Name).second ||
					!InputValueValid(override.Value) || found->second->index() != override.Value.index())
					return InputResolutionFailure(
						diagnostic, {}, "input override is unknown, duplicated or has incompatible type/value"
					);
				found->second = &override.Value;
			}
			std::unordered_map<std::string_view, size_t> nodes;
			for (size_t index = 0; index < document.Nodes.size(); ++index) {
				const auto &node = document.Nodes[index];
				if (!NodeIdentityValid(node.Id) || !nodes.emplace(node.Id, index).second)
					return InputResolutionFailure(
						diagnostic, node.Id, "node identity is invalid or duplicated"
					);
				bytes += node.Id.size();
				for (const auto &input : node.Inputs) {
					if (!NodeIdentityValid(input))
						return InputResolutionFailure(diagnostic, node.Id, "input node identity is invalid");
					bytes += input.size();
				}
				if (node.Inputs.size() > 2)
					return InputResolutionFailure(diagnostic, node.Id, "input count exceeds node limits");
				if (const auto *source = std::get_if<Source>(&node.Value)) {
					if (source->Path.size() > 4096)
						return InputResolutionFailure(diagnostic, node.Id, "source path exceeds limits");
					bytes += source->Path.size();
				}
			}
			for (const auto &output : document.Outputs) {
				if (!NodeIdentityValid(output.Name) || !NodeIdentityValid(output.Node))
					return InputResolutionFailure(diagnostic, {}, "output identity is invalid");
				bytes += output.Name.size() + output.Node.size();
			}
			for (const auto &binding : document.Bindings) {
				if (!NodeIdentityValid(binding.Node) || !NodeIdentityValid(binding.Property) ||
					!IsReferenceToken(binding.Input))
					return InputResolutionFailure(diagnostic, binding.Node, "binding identity is invalid");
				bytes += binding.Node.size() + binding.Property.size() + binding.Input.size();
			}
			if (bytes > Limits::MaximumDocumentBytes)
				return InputResolutionFailure(diagnostic, {}, "authored data exceeds document byte limit");
			Document resolved = document;
			std::unordered_set<std::string> bound;
			for (const auto &binding : document.Bindings) {
				const auto node = nodes.find(binding.Node);
				const auto input = values.find(binding.Input);
				if (node == nodes.end() || input == values.end() ||
					!bound.insert(binding.Node + '\0' + binding.Property).second)
					return InputResolutionFailure(
						diagnostic, binding.Node, "binding names missing input/node or duplicates a control"
					);
				if (!Apply(resolved.Nodes[node->second].Value, binding.Property, *input->second))
					return InputResolutionFailure(
						diagnostic, binding.Node, "binding control type, name or value is incompatible"
					);
			}
			resolved.Parameters.clear();
			resolved.Bindings.clear();
			Plan checked;
			if (!detail::CompileResolved(resolved, checked, diagnostic)) return false;
			out = std::move(resolved);
			diagnostic = {};
			return true;
		} catch (const std::exception &error) {
			return InputResolutionFailure(diagnostic, {}, error.what());
		}
	}
	bool Compile(const Document &document, Plan &out, Diagnostic &diagnostic) {
		if (document.Parameters.empty() && document.Bindings.empty())
			return detail::CompileResolved(document, out, diagnostic);
		Document resolved;
		if (!ResolveInputs(document, {}, resolved, diagnostic)) return false;
		return detail::CompileResolved(resolved, out, diagnostic);
	}
}
