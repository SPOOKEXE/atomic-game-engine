#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentForm.hpp>

namespace engine::assets {

	const char *Describe(AssetKind kind) {
		switch (kind) {
		case AssetKind::Mesh:
			return "mesh";
		case AssetKind::Texture:
			return "texture";
		case AssetKind::Audio:
			return "audio";
		case AssetKind::Material:
			return "material";
		case AssetKind::Font:
			return "font";
		case AssetKind::Script:
			return "script";
		case AssetKind::Video:
			return "video";
		case AssetKind::Data:
			return "data";
		case AssetKind::Shader:
			return "shader";
		case AssetKind::Unknown:
			break;
		}
		return "unknown";
	}

	bool IsRuntimeReadable(std::string_view name) {
		// **One negation over the one table**, where this used to be a second
		// list of fourteen source extensions beside the routing table with a
		// comment saying it was the one that must not go stale. A format is a
		// row with a source column now, so the two answers cannot disagree.
		//
		// A name with no extension, or one the table has no row for, is
		// `ContentForm::Unknown` - not a source, so readable, which is the
		// answer this function has always given it.
		return !IsSourceForm(FormOfName(name));
	}

	AssetKind KindFromName(std::string_view text) {
		for (uint8_t value = 1; value <= static_cast<uint8_t>(AssetKind::Shader); ++value) {
			const auto kind = static_cast<AssetKind>(value);
			if (text == Describe(kind)) {
				return kind;
			}
		}
		return AssetKind::Unknown;
	}

	AssetKind KindOfName(std::string_view name) {
		return KindOfForm(FormOfName(name));
	}
}
