#include <engine/ang/Document.hpp>
#include <engine/graphcore/Document.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <type_traits>

namespace engine::ang {

	namespace document_detail {
		namespace text = graphcore::text;

		constexpr std::string_view HEADER = "ang 1";

		template <typename T> void AppendNumber(std::string &out, T value) {
			char buffer[64];
			const auto end = std::to_chars(buffer, buffer + sizeof(buffer), value);
			out += ' ';
			out.append(buffer, end.ptr);
		}

		template <typename T> bool TakeNumber(std::string_view &line, T &out) {
			const std::string_view word = text::TakeWord(line);
			const auto read = std::from_chars(word.data(), word.data() + word.size(), out);
			if (word.empty() || read.ec != std::errc{} || read.ptr != word.data() + word.size()) {
				return false;
			}
			if constexpr (std::is_floating_point_v<T>) {
				return std::isfinite(out);
			}
			return true;
		}

		void AppendName(std::string &out, std::string_view name) {
			out += ' ';
			text::AppendQuoted(out, name);
		}

		// `kind "type" fields`. The kind is the stored form, which a retagged
		// scalar's type name does not give.
		bool AppendValue(std::string &out, const Value &value) {
			return std::visit(
				[&](const auto &held) {
					using Held = std::decay_t<decltype(held)>;
					const auto floats = [&](const char *kind, auto... parts) {
						out += kind;
						AppendName(out, value.TypeName());
						(AppendNumber(out, parts), ...);
						return (std::isfinite(parts) && ...);
					};
					if constexpr (std::is_same_v<Held, bool>) {
						out += " bool";
						AppendName(out, value.TypeName());
						out += ' ';
						out += text::FlagText(held);
						return true;
					} else if constexpr (std::is_same_v<Held, int64_t>) {
						out += " int";
						AppendName(out, value.TypeName());
						AppendNumber(out, held);
						return true;
					} else if constexpr (std::is_same_v<Held, double>) {
						return floats(" number", held);
					} else if constexpr (std::is_same_v<Held, core::Vector2>) {
						return floats(" vector2", held.X, held.Y);
					} else if constexpr (std::is_same_v<Held, core::Vector3>) {
						return floats(" vector3", held.X, held.Y, held.Z);
					} else if constexpr (std::is_same_v<Held, Vector4>) {
						return floats(" vector4", held.X, held.Y, held.Z, held.W);
					} else if constexpr (std::is_same_v<Held, core::Color3>) {
						return floats(" color3", held.R, held.G, held.B);
					} else if constexpr (std::is_same_v<Held, std::string>) {
						out += " string";
						AppendName(out, value.TypeName());
						AppendName(out, held);
						return true;
					} else if constexpr (std::is_same_v<Held, core::Name>) {
						out += " name";
						AppendName(out, value.TypeName());
						AppendName(out, held.Text());
						return true;
					} else {
						return false;
					}
				},
				value.Raw()
			);
		}

		bool TakeValue(std::string_view &line, Value &out) {
			const std::string_view kind = text::TakeWord(line);
			std::string type;
			if (!text::TakeQuoted(line, type) || type.empty()) {
				return false;
			}
			Value value;
			if (kind == "bool") {
				bool flag = false;
				if (!text::TakeFlag(line, flag)) {
					return false;
				}
				value = Value(flag);
			} else if (kind == "int") {
				int64_t number = 0;
				if (!TakeNumber(line, number)) {
					return false;
				}
				value = Value(number);
			} else if (kind == "number") {
				double number = 0.0;
				if (!TakeNumber(line, number)) {
					return false;
				}
				value = Value(number);
			} else if (kind == "string" || kind == "name") {
				std::string held;
				if (!text::TakeQuoted(line, held)) {
					return false;
				}
				value = kind == "string" ? Value(held) : Value(core::Name(held));
			} else {
				std::array<float, 4> parts{};
				const size_t count = kind == "vector2"						 ? 2
									 : kind == "vector3" || kind == "color3" ? 3
									 : kind == "vector4"					 ? 4
																			 : 0;
				if (count == 0) {
					return false;
				}
				for (size_t index = 0; index < count; ++index) {
					if (!TakeNumber(line, parts[index])) {
						return false;
					}
				}
				if (kind == "vector2") {
					value = Value(core::Vector2{parts[0], parts[1]});
				} else if (kind == "vector3") {
					value = Value(core::Vector3{parts[0], parts[1], parts[2]});
				} else if (kind == "color3") {
					value = Value(core::Color3{parts[0], parts[1], parts[2]});
				} else {
					value = Value(Vector4{parts[0], parts[1], parts[2], parts[3]});
				}
			}
			out = value.WithType(std::move(type));
			return true;
		}

		bool TakePoint(std::string_view &line, core::Vector2 &out) {
			return TakeNumber(line, out.X) && TakeNumber(line, out.Y);
		}

		// One line into the pending lists. `param` belongs to the last node.
		bool TakeLine(
			std::string_view line,
			std::vector<NodeInstance> &nodes,
			std::vector<Link> &links,
			std::vector<Group> &groups
		) {
			const std::string_view word = text::TakeWord(line);
			if (word == "node") {
				NodeInstance node;
				if (!text::TakeQuoted(line, node.Id) || !text::TakeQuoted(line, node.Def) ||
					!text::TakeUnsigned(line, node.Version) || !TakePoint(line, node.Position) ||
					!text::TakeQuoted(line, node.Group) || !text::TakeFlag(line, node.Bypassed) ||
					!text::TakeFlag(line, node.Collapsed) || !text::AtEnd(line)) {
					return false;
				}
				nodes.push_back(std::move(node));
				return true;
			}
			if (word == "param") {
				std::string name;
				Value value;
				return !nodes.empty() && text::TakeQuoted(line, name) && TakeValue(line, value) &&
					   text::AtEnd(line) &&
					   nodes.back().Params.emplace(std::move(name), std::move(value)).second;
			}
			if (word == "link") {
				Link link;
				if (!text::TakeQuoted(line, link.FromNode) || !text::TakeQuoted(line, link.FromPort) ||
					!text::TakeQuoted(line, link.ToNode) || !text::TakeQuoted(line, link.ToPort) ||
					!text::AtEnd(line)) {
					return false;
				}
				links.push_back(std::move(link));
				return true;
			}
			if (word == "group") {
				Group group;
				if (!text::TakeQuoted(line, group.Id) || !text::TakeQuoted(line, group.Label) ||
					!TakePoint(line, group.Position) || !TakePoint(line, group.Size) || !text::AtEnd(line)) {
					return false;
				}
				groups.push_back(std::move(group));
				return true;
			}
			return false;
		}
	}

	bool WriteGraph(const Graph &graph, std::string &out, std::string &error) {
		using namespace document_detail;
		std::string written(HEADER);
		written += '\n';
		for (const Group &group : graph.Groups()) {
			written += "group";
			AppendName(written, group.Id);
			AppendName(written, group.Label);
			for (const float part : {group.Position.X, group.Position.Y, group.Size.X, group.Size.Y}) {
				AppendNumber(written, part);
			}
			written += '\n';
		}
		for (const NodeInstance &node : graph.Nodes()) {
			written += "node";
			AppendName(written, node.Id);
			AppendName(written, node.Def);
			AppendNumber(written, node.Version);
			AppendNumber(written, node.Position.X);
			AppendNumber(written, node.Position.Y);
			AppendName(written, node.Group);
			written += ' ';
			written += text::FlagText(node.Bypassed);
			written += ' ';
			written += text::FlagText(node.Collapsed);
			written += '\n';
			for (const auto &[name, value] : node.Params) {
				if (value.Empty()) {
					continue;
				}
				written += "param";
				AppendName(written, name);
				if (!AppendValue(written, value)) {
					error = "param '" + name + "' of node '" + node.Id + "' has no text form";
					return false;
				}
				written += '\n';
			}
		}
		for (const Link &link : graph.Links()) {
			written += "link";
			for (const std::string *part : {&link.FromNode, &link.FromPort, &link.ToNode, &link.ToPort}) {
				AppendName(written, *part);
			}
			written += '\n';
		}
		out = std::move(written);
		return true;
	}

	bool ReadGraph(std::string_view source, Graph &graph, std::string &error, const DocumentLimits &limits) {
		using namespace document_detail;
		if (source.size() > limits.MaximumBytes) {
			error = "document is too large";
			return false;
		}
		static constexpr std::array<std::string_view, 1> HEADERS{HEADER};
		if (text::TakeHeader(source, HEADERS) < 0) {
			error = "not a node graph document";
			return false;
		}
		std::vector<NodeInstance> nodes;
		std::vector<Link> links;
		std::vector<Group> groups;
		while (!source.empty()) {
			const std::string_view line = text::NextLine(source);
			if (line.empty()) {
				continue;
			}
			if (!TakeLine(line, nodes, links, groups)) {
				error = "bad line: " + std::string(line.substr(0, 128));
				return false;
			}
			if (nodes.size() > limits.MaximumNodes || links.size() > limits.MaximumLinks ||
				groups.size() > limits.MaximumNodes) {
				error = "document has too many nodes, links or groups";
				return false;
			}
		}
		const EditResult result = graph.Load(std::move(nodes), std::move(links), std::move(groups));
		if (!result.Ok) {
			error = result.Error;
		}
		return result.Ok;
	}
}
