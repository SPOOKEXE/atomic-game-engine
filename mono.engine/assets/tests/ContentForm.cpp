#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentForm.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.assets.contentform")

using engine::assets::AllForms;
using engine::assets::AssetKind;
using engine::assets::ContentForm;
using engine::assets::Describe;
using engine::assets::FormFromName;
using engine::assets::FormOfName;
using engine::assets::IsRuntimeReadable;
using engine::assets::IsSourceForm;
using engine::assets::KindOfForm;
using engine::assets::KindOfName;

namespace {
	// The whole table, written out here rather than derived from the one under
	// test.
	//
	// **This is the point of the case.** The form column was merged into the
	// routing table from a second list that used to sit beside it, so the guard
	// that the merge changed nothing has to be an independent statement of what
	// every extension means - a test that asked the table what the table says
	// would pass against any merge at all.
	struct Expectation {
		const char *Extension;
		ContentForm Form;
		AssetKind Kind;
		bool Source;
	};

	constexpr Expectation TABLE[] = {
		{"amesh", ContentForm::AMesh, AssetKind::Mesh, false},
		{"mesh", ContentForm::Mesh, AssetKind::Mesh, false},
		{"glb", ContentForm::Glb, AssetKind::Mesh, true},
		{"gltf", ContentForm::Gltf, AssetKind::Mesh, true},
		{"obj", ContentForm::Obj, AssetKind::Mesh, true},
		{"fbx", ContentForm::Fbx, AssetKind::Mesh, true},
		{"pmx", ContentForm::Pmx, AssetKind::Mesh, true},

		{"atex", ContentForm::ATex, AssetKind::Texture, false},
		{"png", ContentForm::Png, AssetKind::Texture, true},
		{"jpg", ContentForm::Jpeg, AssetKind::Texture, true},
		{"jpeg", ContentForm::Jpeg, AssetKind::Texture, true},
		{"bmp", ContentForm::Bmp, AssetKind::Texture, true},
		{"tga", ContentForm::Tga, AssetKind::Texture, true},
		{"ktx2", ContentForm::Ktx2, AssetKind::Texture, false},
		{"dds", ContentForm::Dds, AssetKind::Texture, false},
		{"basis", ContentForm::Basis, AssetKind::Texture, false},
		{"gif", ContentForm::Gif, AssetKind::Texture, true},
		{"svg", ContentForm::Svg, AssetKind::Texture, true},

		{"wav", ContentForm::Wav, AssetKind::Audio, false},
		{"ogg", ContentForm::Ogg, AssetKind::Audio, false},
		{"flac", ContentForm::Flac, AssetKind::Audio, false},
		{"mp3", ContentForm::Mp3, AssetKind::Audio, false},

		{"amat", ContentForm::AMat, AssetKind::Material, false},
		{"mat", ContentForm::Mat, AssetKind::Material, true},
		{"surface", ContentForm::Surface, AssetKind::Material, true},

		{"ttf", ContentForm::Ttf, AssetKind::Font, false},
		{"otf", ContentForm::Otf, AssetKind::Font, false},

		{"luau", ContentForm::Luau, AssetKind::Script, false},
		{"lua", ContentForm::Lua, AssetKind::Script, false},
		{"ts", ContentForm::TypeScript, AssetKind::Script, false},
		{"js", ContentForm::JavaScript, AssetKind::Script, false},

		{"mp4", ContentForm::Mp4, AssetKind::Video, false},

		{"agame", ContentForm::AGame, AssetKind::Data, false},
		{"aworld", ContentForm::AWorld, AssetKind::Data, false},

		{"spv", ContentForm::Spv, AssetKind::Shader, false},
		{"frag", ContentForm::Frag, AssetKind::Shader, true},
		{"vert", ContentForm::Vert, AssetKind::Shader, true},
		{"comp", ContentForm::Comp, AssetKind::Shader, true},
		{"glsl", ContentForm::Glsl, AssetKind::Shader, true},
	};
}

TEST_CASE("every extension names one form, one kind and one side of the bake", "[content][form]") {
	for (const Expectation &row : TABLE) {
		const std::string name = std::string("content/thing.") + row.Extension;
		INFO(name);

		CHECK(FormOfName(name) == row.Form);
		CHECK(KindOfForm(row.Form) == row.Kind);

		// The two older questions, still asked through their own doors, because
		// those are what the rest of the engine calls.
		CHECK(KindOfName(name) == row.Kind);
		CHECK(IsSourceForm(row.Form) == row.Source);
		CHECK(IsRuntimeReadable(name) == !row.Source);

		// The extension is read whatever case it was written in.
		std::string shouted(row.Extension);
		std::transform(shouted.begin(), shouted.end(), shouted.begin(), [](char letter) {
			// Letters only: `ktx2` and `mp4` carry digits, and shifting one by
			// the case offset produces a character no row has.
			return (letter >= 'a' && letter <= 'z') ? static_cast<char>(letter - 'a' + 'A') : letter;
		});
		CHECK(FormOfName(std::string("thing.") + shouted) == row.Form);
	}
}

TEST_CASE("a form's name round-trips and is its canonical extension", "[content][form]") {
	for (const ContentForm form : AllForms()) {
		INFO(Describe(form));
		CHECK(FormFromName(Describe(form)) == form);
	}

	// Where two extensions name one form, the canonical one is what a setting
	// is written with - and the other still resolves.
	CHECK(std::string(Describe(ContentForm::Jpeg)) == "jpeg");
	CHECK(FormOfName("photo.jpg") == ContentForm::Jpeg);

	CHECK(FormFromName("nonesuch") == ContentForm::Unknown);
	CHECK(std::string(Describe(ContentForm::Unknown)) == "unknown");
}

TEST_CASE("a name with no extension of its own has no form", "[content][form]") {
	CHECK(FormOfName("plain") == ContentForm::Unknown);
	CHECK(FormOfName("") == ContentForm::Unknown);
	CHECK(FormOfName("trailing.") == ContentForm::Unknown);

	// A dot inside a directory component is not an extension: reading `2/rock`
	// as one would put an asset in whatever form that happened to match.
	CHECK(FormOfName("v1.2/rock") == ContentForm::Unknown);
	CHECK(KindOfName("v1.2/rock") == AssetKind::Unknown);

	// And `Unknown` is not a source, so a name with no extension is readable -
	// the answer `IsRuntimeReadable` has always given it.
	CHECK_FALSE(IsSourceForm(ContentForm::Unknown));
	CHECK(IsRuntimeReadable("plain"));
	CHECK(IsRuntimeReadable("v1.2/rock"));
}

TEST_CASE("AllForms holds every form once and no unknown", "[content][form]") {
	const std::span<const ContentForm> forms = AllForms();

	std::vector<ContentForm> seen(forms.begin(), forms.end());
	std::sort(seen.begin(), seen.end());
	CHECK(std::adjacent_find(seen.begin(), seen.end()) == seen.end());
	CHECK(std::find(seen.begin(), seen.end(), ContentForm::Unknown) == seen.end());

	// Every row in the expectation above is reachable from it, which is what
	// makes a form added to the table without a row here fail rather than pass
	// quietly.
	for (const Expectation &row : TABLE) {
		INFO(row.Extension);
		CHECK(std::find(forms.begin(), forms.end(), row.Form) != forms.end());
	}

	// And nothing is in the table that this case does not know about - the
	// other direction, which is the one that catches an *addition*.
	for (const ContentForm form : forms) {
		INFO(Describe(form));
		const bool named = std::any_of(std::begin(TABLE), std::end(TABLE), [form](const Expectation &row) {
			return row.Form == form;
		});
		CHECK(named);
	}
}
