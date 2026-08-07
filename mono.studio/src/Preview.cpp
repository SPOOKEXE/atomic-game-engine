// The words in an empty preview box.
//
// **Separated from the panels because the wording is the feature.** One blank
// square for four different situations is what this replaces, and the phrasing
// is what turns each of them into something a person can act on — so it lives in
// one place rather than being written out at each of the three call sites, which
// is how three of them come to disagree.

#include <studio/Preview.hpp>

namespace studio {

	const char *DescribePreview(PreviewState state, engine::assets::AssetKind kind) {
		switch (state) {
		case PreviewState::Ready:
			// There is a picture. Saying anything here would be text drawn over
			// the thing it is describing.
			return nullptr;

		case PreviewState::Pending:
			// **Deliberately silent.** A caption that appeared for two frames
			// and then vanished reads as a flicker, and the placeholder box
			// already says "nothing here yet" by being empty.
			return nullptr;

		case PreviewState::TooLarge:
			return kind == engine::assets::AssetKind::Mesh ? "too many triangles to preview"
														   : "too large to preview";

		case PreviewState::Unavailable:
			break;
		}

		// **Named by what the store thinks it is**, because the useful sentence
		// differs. A mesh with no preview in a store that has never been baked
		// is the common case in this repository, and "not baked" is the action;
		// a texture that would not decode is a broken file.
		switch (kind) {
		case engine::assets::AssetKind::Mesh:
			return "no preview — bake it with assetc first";
		case engine::assets::AssetKind::Texture:
			return "not an image this reads";
		case engine::assets::AssetKind::Audio:
			return "sound";
		default:
			break;
		}
		return "no preview";
	}
}
