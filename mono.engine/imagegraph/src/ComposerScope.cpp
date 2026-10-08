#include <engine/imagegraph/ComposerScope.hpp>
#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	namespace {
		ComposerDomain PortDomain(ValueType type) {
			if (type == ValueType::AudioBit) return ComposerDomain::Audio;
			if (type == ValueType::Mesh || type == ValueType::Scene3D || type == ValueType::Material3D ||
				type == ValueType::Light3D || type == ValueType::Armature)
				return ComposerDomain::Mesh3D;
			return ComposerDomain::Image;
		}
		bool EqualExtension(std::string_view left, std::string_view right) {
			if (left.size() != right.size()) return false;
			for (size_t index = 0; index < left.size(); ++index) {
				const char ch = left[index];
				if ((ch >= 'A' && ch <= 'Z' ? char(ch + ('a' - 'A')) : ch) != right[index]) return false;
			}
			return true;
		}
	}

	ComposerDomain ComposerNodeDomain(std::string_view type) {
		if (type == "pc.image_mp4") return ComposerDomain::Video;
		if (type.starts_with("image.audio_") || type.starts_with("pc.audio_") ||
			type.starts_with("pc.wav_file_"))
			return ComposerDomain::Audio;
		if (type == "pc.3_d_particle" || type.starts_with("pc.p_system_3_d_")) return ComposerDomain::Mesh3D;
		const auto *schema = FindSchema(type);
		if (!schema) return ComposerDomain::Image;
		for (const auto &port : schema->Ports) {
			// grug a union with an image/data branch still serves image work.
			if (!port.Alternatives.empty()) {
				bool imageBranch = false;
				for (const auto alternative : port.Alternatives)
					imageBranch = imageBranch || PortDomain(alternative) == ComposerDomain::Image;
				if (imageBranch) continue;
				return PortDomain(port.Alternatives.front());
			}
			const auto domain = PortDomain(port.Type);
			if (domain != ComposerDomain::Image) return domain;
		}
		return ComposerDomain::Image;
	}

	ComposerDomain ComposerNodeDomain(const Node &node) {
		const auto domain = ComposerNodeDomain(node.Type);
		if (domain != ComposerDomain::Image) return domain;
		for (const auto &input : node.DynamicInputs) {
			const auto inputDomain = PortDomain(input.Type);
			if (inputDomain != ComposerDomain::Image) return inputDomain;
		}
		for (const auto &output : node.DynamicOutputs) {
			const auto outputDomain = PortDomain(output.Type);
			if (outputDomain != ComposerDomain::Image) return outputDomain;
		}
		return ComposerDomain::Image;
	}

	bool ComposerNodeEnabled(std::string_view type, ComposerScope scope) {
		return scope == ComposerScope::Unrestricted ||
			   (scope == ComposerScope::ImageOnly && ComposerNodeDomain(type) == ComposerDomain::Image);
	}
	bool ComposerNodeEnabled(const Node &node, ComposerScope scope) {
		return scope == ComposerScope::Unrestricted ||
			   (scope == ComposerScope::ImageOnly && ComposerNodeDomain(node) == ComposerDomain::Image);
	}
	Status CheckComposerNodeScope(const Node &node, ComposerScope scope, Diagnostic &diagnostic) {
		if (scope != ComposerScope::Unrestricted && scope != ComposerScope::ImageOnly) {
			diagnostic = {Status::InvalidValue, node.Id, {}, "Composer scope is invalid"};
			return diagnostic.Code;
		}
		if (ComposerNodeEnabled(node, scope)) return Status::Ok;
		const auto domain = ComposerNodeDomain(node);
		diagnostic = {
			Status::UnsupportedExecution,
			node.Id,
			{},
			domain == ComposerDomain::Audio	  ? "Audio is disabled in the image composer"
			: domain == ComposerDomain::Video ? "Video is disabled in the image composer"
											  : "3D meshes are disabled in the image composer"
		};
		return diagnostic.Code;
	}
	bool ComposerExportEnabled(std::string_view extension, ComposerScope scope) {
		return scope == ComposerScope::Unrestricted ||
			   (scope == ComposerScope::ImageOnly && !EqualExtension(extension, ".mp4") &&
				!EqualExtension(extension, ".webm") && !EqualExtension(extension, ".wav"));
	}
}
