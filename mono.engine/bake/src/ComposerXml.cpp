#include <engine/bake/ComposerXml.hpp>
#include <engine/core/Xml.hpp>

#include <algorithm>
#include <array>
namespace engine::bake {
	bool
	ReadComposerXml(std::string_view text, ComposerXml &output, std::string &failure, uint64_t maximumBytes) {
		failure.clear();
		if (text.size() > 1048576 || maximumBytes < sizeof(ComposerXml) || text.size() > maximumBytes / 128) {
			failure = "native XML input exceeds its operation budget";
			return false;
		}
		const core::xml::Options options{"composer XML", 64, false};
		core::xml::Failure refusal;
		ComposerXml parsed;
		parsed.Root.Type = "root";
		size_t nodes = 0;
		uint64_t used = sizeof(ComposerXml);
		const auto charge = [&](uint64_t bytes) {
			if (bytes > maximumBytes - used) return false;
			used += bytes;
			return true;
		};
		const auto attributes = [&](std::string_view run, ComposerXmlNode &node) {
			std::vector<core::xml::Attribute> values;
			if (!core::xml::ReadAttributes(run, options, values, refusal)) {
				failure = refusal.Message;
				return false;
			}
			for (const auto &value : values) {
				if (!charge(
						sizeof(std::pair<std::string, std::string>) + value.Name.size() + value.Value.size()
					))
					return false;
				std::string decoded(value.Value);
				const std::array<std::pair<std::string_view, std::string_view>, 5> entities{
					{{"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}, {"&apos;", "'"}, {"&quot;", "\""}}
				};
				for (const auto &[entity, replacement] : entities) {
					size_t position = 0;
					while ((position = decoded.find(entity, position)) != std::string::npos) {
						decoded.replace(position, entity.size(), replacement);
						position += replacement.size();
					}
				}
				node.Attributes.emplace_back(value.Name, std::move(decoded));
			}
			return true;
		};
		const auto prolog = text.find_first_not_of(" \r\n\t");
		if (prolog != std::string_view::npos && text.substr(prolog).starts_with("<?xml")) {
			const auto end = text.find("?>", prolog + 5);
			if (end == std::string_view::npos) {
				failure = "native XML prolog is truncated";
				return false;
			}
			parsed.Prolog.emplace();
			parsed.Prolog->Type = "prolog";
			if (!attributes(text.substr(prolog + 5, end - prolog - 5), *parsed.Prolog)) return false;
			text.remove_prefix(end + 2);
		}
		std::vector<ComposerXmlNode *> stack{&parsed.Root};
		while (!text.empty()) {
			const auto next = text.find('<');
			if (next == std::string_view::npos) break;
			const auto raw = text.substr(0, next);
			// The native scanner stops collecting text at a newline until the next opening tag.
			if (!raw.empty() && raw.find_first_of("\r\n") == std::string_view::npos) {
				if (!charge(raw.size())) {
					failure = "native XML text exceeds its budget";
					return false;
				}
				stack.back()->Text = std::string(raw);
			}
			core::xml::Tag tag;
			const auto state = core::xml::NextTag(text, options, tag, refusal);
			if (state == core::xml::Scan::Error) {
				failure = refusal.Message;
				return false;
			}
			if (state == core::xml::Scan::End) break;
			if (tag.Closing) {
				if (stack.size() == 1 || stack.back()->Type != tag.Name) {
					failure = "native XML closing tag does not match its parent";
					return false;
				}
				stack.pop_back();
				continue;
			}
			if (stack.size() > 64 || ++nodes > 4096 || !charge(sizeof(ComposerXmlNode) + tag.Name.size())) {
				failure = "native XML tree exceeds its depth or storage budget";
				return false;
			}
			stack.back()->Children.emplace_back();
			auto &node = stack.back()->Children.back();
			node.Type = tag.Name;
			if (!attributes(tag.Attributes, node)) {
				if (failure.empty()) failure = "native XML attributes exceed their budget";
				return false;
			}
			if (!tag.SelfClosing) stack.push_back(&node);
		}
		if (stack.size() != 1 || parsed.Root.Children.empty()) {
			failure = "native XML document is truncated or has no root";
			return false;
		}
		output = std::move(parsed);
		return true;
	}
}
