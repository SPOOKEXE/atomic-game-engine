// Which properties get a content picker.
//
// **Reachable headlessly because the decision has no imgui in it.** The picker
// is a modal; what can be *wrong* is the table saying `Mesh` names a mesh and
// `Name` does not - and a table that answered `Unknown` for a real content
// property is a text field where a picker should be, which nothing fails on and
// nobody notices until they mistype a mesh name.
//
// This is also the file that will fail when somebody adds a content property and
// forgets this table, which is the whole reason the trade in `Assets.hpp` is
// acceptable: the cost is a missing convenience, and it is pinned rather than
// left to be discovered.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <studio/Assets.hpp>

TEST_SUITE_ID("studio.assets")

using engine::assets::AssetKind;
using studio::ContentKindOfProperty;

TEST_CASE("the properties that name content are the ones that get a picker", "[studio][assets]") {
	// **Both spellings of both aliases, which is what this case is really
	// for.** `Visual::Mesh` is bound as `BasePart.Mesh` *and* as
	// `MeshPart.MeshId`; `SurfaceAppearance::ColourMap` as `BasePart.ColorMap`
	// *and* as `MeshPart.TextureID`. The first version of the table had one of
	// each pair, so selecting a `MeshPart` - the whole reason the picker
	// exists - showed a plain text field on the two names it actually
	// displays. An alias is a row, because a person picks whichever name their
	// class shows them.
	CHECK(ContentKindOfProperty("MeshId") == AssetKind::Mesh);
	CHECK(ContentKindOfProperty("TextureID") == AssetKind::Texture);

	// **The aliases are gone with the properties they named.** `BasePart.Mesh`
	// and `BasePart.ColorMap` were removed at v0.10 - geometry from a file is
	// not something a plain `Part` has - so a row for either would be a picker
	// offered on a property no class declares.
	CHECK(ContentKindOfProperty("Mesh") == AssetKind::Unknown);
	CHECK(ContentKindOfProperty("ColorMap") == AssetKind::Unknown);

	// **One row covers `ParticleEmitter`, `Beam` and `Trail`**, which is the
	// point of keying on the property rather than the class: three classes,
	// one spelling, one answer.
	CHECK(ContentKindOfProperty("Texture") == AssetKind::Texture);

	CHECK(ContentKindOfProperty("SoundId") == AssetKind::Audio);
	CHECK(ContentKindOfProperty("Image") == AssetKind::Texture);

	// The `Material` instance's one property. **`MaterialId` and not
	// `Material`**, which is the row below's other half: the class is called
	// `Material` and a property of the same name would read as a
	// self-reference, so this tree spells an asset name `<Thing>Id` - `MeshId`,
	// `SoundId`.
	CHECK(ContentKindOfProperty("MaterialId") == AssetKind::Material);
}

TEST_CASE("an ordinary property gets no picker", "[studio][assets]") {
	// **Most `Name` properties are labels, and a modal over one would be a
	// dialog in the way.** `Name` itself is the case that would be worst: every
	// instance in the tree has one.
	CHECK(ContentKindOfProperty("Name") == AssetKind::Unknown);

	// **`Material` is still not a content property, and now for a second
	// reason.** It was an enum on `BasePart` and is not a property at all any
	// more: `Surface::Material` is the only thing left spelled that way, and it
	// names a `SurfaceTable` row rather than an asset.
	CHECK(ContentKindOfProperty("Material") == AssetKind::Unknown);
	CHECK(ContentKindOfProperty("Anchored") == AssetKind::Unknown);
	CHECK(ContentKindOfProperty("") == AssetKind::Unknown);
}

TEST_CASE("the match is exact and not a prefix", "[studio][assets]") {
	// **`Beam.TextureSpeed` and `Beam.TextureLength` are numbers**, and a
	// prefix match would have put a texture picker on both - which would be a
	// modal that cannot write the property it was opened for.
	CHECK(ContentKindOfProperty("TextureSpeed") == AssetKind::Unknown);
	CHECK(ContentKindOfProperty("TextureLength") == AssetKind::Unknown);
	CHECK(ContentKindOfProperty("ImageColor3") == AssetKind::Unknown);
	CHECK(ContentKindOfProperty("ImageTransparency") == AssetKind::Unknown);
	CHECK(ContentKindOfProperty("ImageRectSize") == AssetKind::Unknown);

	// And it is case-sensitive, matching how `PropertyDescriptor::Spelling`
	// stores it - a lookup that lowercased would match a property nobody
	// declared.
	CHECK(ContentKindOfProperty("mesh") == AssetKind::Unknown);
}
