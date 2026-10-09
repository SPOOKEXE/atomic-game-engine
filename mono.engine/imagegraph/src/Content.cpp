#include <engine/imagegraph/Content.hpp>
#include <engine/imagegraph/Reference.hpp>

#include <algorithm>

namespace engine::imagegraph {
	bool RuntimeSources(
		const Document &document,
		std::vector<std::string> &out,
		Diagnostic &diagnostic,
		std::span<const std::string_view> outputs
	) {
		Plan plan;
		if (!Compile(document, plan, diagnostic)) return false;
		std::vector<bool> needed(document.Nodes.size(), outputs.empty());
		std::vector<size_t> pending;
		for (const auto name : outputs) {
			const auto output =
				std::find_if(document.Outputs.begin(), document.Outputs.end(), [&](const Output &output) {
					return output.Name == name;
				});
			if (output == document.Outputs.end()) {
				diagnostic = {{}, "demand names an unknown graph output"};
				return false;
			}
			const size_t target = plan.Outputs[static_cast<size_t>(output - document.Outputs.begin())];
			if (!needed[target]) {
				needed[target] = true;
				pending.push_back(target);
			}
		}
		while (!pending.empty()) {
			const size_t index = pending.back();
			pending.pop_back();
			for (const auto input : plan.Inputs[index])
				if (!needed[input]) {
					needed[input] = true;
					pending.push_back(input);
				}
		}
		std::vector<std::string> candidate;
		for (size_t index = 0; index < document.Nodes.size(); ++index) {
			if (!needed[index]) continue;
			const auto &node = document.Nodes[index];
			const auto *source = std::get_if<Source>(&node.Value);
			if (!source) continue;
			if (!IsRuntimeTexture(source->Path)) {
				diagnostic = {node.Id, "runtime source must name an exact portable .atex asset"};
				return false;
			}
			if (std::find(candidate.begin(), candidate.end(), source->Path) == candidate.end())
				candidate.push_back(source->Path);
		}
		out = std::move(candidate);
		return true;
	}
	bool Content::Admit(
		std::string_view asset, std::string_view root, std::string_view encoded, Diagnostic &diagnostic
	) {
		if (!IsRuntimeAsset(asset) || root.empty()) {
			diagnostic = {{}, "runtime graph requires a portable asset name and verified root"};
			return false;
		}
		diagnostic = {};
		const auto prior = Records.find(std::string(asset));
		if (prior != Records.end() && prior->second.Root == root) return false;
		if (prior == Records.end() && Records.size() >= MaximumRecords) {
			diagnostic = {{}, "admitted graph content exceeds record budget"};
			return false;
		}
		const size_t previousBytes = prior == Records.end() ? 0 : prior->second.EncodedBytes;
		if (encoded.size() > Limits::MaximumDocumentBytes ||
			encoded.size() > Limits::MaximumRetainedBytes - (RetainedBytes - previousBytes)) {
			diagnostic = {{}, "admitted graph content exceeds retention budget"};
			return false;
		}
		Document authored, resolved;
		std::vector<std::string> sources;
		if (!Read(encoded, authored, diagnostic) || !ResolveInputs(authored, {}, resolved, diagnostic) ||
			!RuntimeSources(resolved, sources, diagnostic))
			return false;
		for (const auto &output : authored.Outputs) {
			if (!IsReferenceToken(output.Name)) {
				diagnostic = {{}, "runtime output must use a portable reference token"};
				return false;
			}
		}
		Records.insert_or_assign(
			std::string(asset),
			ContentRecord{std::move(authored), std::string(root), ++Revision, encoded.size()}
		);
		RetainedBytes = RetainedBytes - previousBytes + encoded.size();
		return true;
	}
	const ContentRecord *Content::Find(std::string_view asset) const {
		const auto found = Records.find(std::string(asset));
		return found == Records.end() ? nullptr : &found->second;
	}
}
