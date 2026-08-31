#pragma once

// The one extension table, and the three questions it answers.
//
// A name's extension says three things and they used to be answered by two
// lists that had to agree: `AssetKind.cpp` carried an `EXTENSIONS` table for
// routing and a separate `SOURCES` list for "does this still need a bake", with
// a comment on the second saying it is the one that must not go stale. It is
// one table with three columns now, so a format added to it arrives everywhere
// at once and there is no second list to forget.
//
// - **The form** is the format - `Png`, `Gif`, `Svg`, `Mp4`, `AMesh`. This is
//   the fine-grained answer, and the one a policy is written against, because
//   "turn GIF off" is not a statement about textures.
// - **The kind** is the routing label - `assets::AssetKind`. Several forms map
//   to one kind by design: a publisher pointed at a source tree and one pointed
//   at a baked tree must classify the same way.
// - **Whether it is a source**, meaning a baker still has to convert it. That
//   is what `IsRuntimeReadable` asks, one negation away.
//
// **A name is a claim and not evidence**, which is the same thing
// `bake::ImageFormatOfName` says: this reads an extension and nothing else, so
// a `.png` holding a JPEG is a `Png` here. Whoever opens the bytes is who finds
// out otherwise, and `ImageFormatOfBytes` is asked first for exactly that
// reason.
//
// @tier L8 · shared
// @since v0.15

#include <engine/assets/AssetKind.hpp>

#include <cstdint>
#include <span>
#include <string_view>

namespace engine::assets {

	// A content format, as an extension names it.
	//
	// **Ordinals are not on the wire.** A manifest records the `AssetKind`, and
	// a form is derived from the name every time it is wanted - so this list may
	// be reordered, which `AssetKind` may not be. What *does* cross a boundary is
	// `Describe`'s text, because a flag is named after a form.
	enum class ContentForm : uint8_t {
		// An extension this engine has no row for, or no extension at all.
		//
		// **Delivered like anything else.** An origin moves bytes it does not
		// interpret, so a policy that refused what it could not name would be a
		// policy that refuses the next format before it is added.
		Unknown = 0,

		// Meshes. `AMesh` is the baked form and the rest are sources.
		AMesh,
		Mesh,
		Glb,
		Gltf,
		Obj,
		Fbx,
		Pmx,

		// Textures. `ATex` is the baked form; `Ktx2`, `Dds` and `Basis` arrive
		// already compressed for a GPU and are not this engine's to bake.
		ATex,
		Png,
		Jpeg,
		Bmp,
		Tga,
		Ktx2,
		Dds,
		Basis,

		// **A texture whose baked form is a flipbook sheet**, which is why it is
		// its own form rather than a variation of `Png`: turning it off is a
		// decision about an animation decoder and about `bake/src/Gif.cpp`'s
		// frame-count truncation, not about images.
		Gif,

		// **A texture whose bake is a rasterisation**, and the form most worth
		// naming separately. `bake/src/Svg.cpp` carries a hand-written
		// rasteriser and a cost charge that exists because a hostile 4000-rect
		// file took over two minutes before it was added. An operator who does
		// not want vector content in their pipeline is turning off a *parser*,
		// which is a security decision rather than a content one.
		Svg,

		// Sound.
		Wav,
		Ogg,
		Flac,
		Mp3,

		// Materials. `AMat` is the baked form.
		AMat,
		Mat,
		Surface,

		// Typefaces.
		Ttf,
		Otf,

		// Source and bytecode. Whether one *runs* is `script`'s decision and its
		// sandbox's; whether it is delivered at all is this.
		Luau,
		Lua,
		TypeScript,
		JavaScript,

		// **Moving pictures, with nothing behind them.** `AssetKind::Video` is a
		// routing label and this engine holds no decoder - `examples/Assets.luau`
		// has a bay that says "no decoder" rather than showing a stand-in. The
		// form exists so a deployment can refuse the bytes rather than deliver
		// megabytes nothing will ever draw.
		Mp4,

		// Whole documents.
		AGame,
		AWorld,

		// Shaders. `Spv` is what a renderer is handed; the rest are sources.
		Spv,
		Frag,
		Vert,
		Comp,
		Glsl,

		// Baked joint animation channels.
		AAnim,
	};

	// The lowercase name for a form, which is also its canonical extension.
	//
	// **The name is the contract**, which is rule 4: it reaches a flag, a config
	// file, a log line and a refusal message. Where two extensions name one form
	// - `.jpg` and `.jpeg` - this answers the one a setting is written with.
	//
	// @param form The form.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ContentForm form);

	// The form `Describe` wrote.
	//
	// @param text The lowercase name.
	// @return The form, or `Unknown` for anything else.
	ContentForm FormFromName(std::string_view text);

	// The form a content name's extension claims.
	//
	// @param name The content name, as the manifest holds it. A dot inside a
	//        directory component is not an extension.
	// @return The form, or `Unknown`.
	ContentForm FormOfName(std::string_view name);

	// What subsystem a form routes to.
	//
	// @param form The form.
	// @return Its kind.
	AssetKind KindOfForm(ContentForm form);

	// Whether a baker still has to convert this form.
	//
	// **`Unknown` is not a source.** A name with no extension, or one this
	// engine has no row for, is whatever it is - the same answer `KindOfName`
	// gives it.
	//
	// @param form The form.
	// @return `true` for a source form.
	bool IsSourceForm(ContentForm form);

	// Every form except `Unknown`, in declaration order.
	//
	// **What a policy and its flag table are built from**, so neither is a
	// second list. A form added above appears in `--flags` with no other edit.
	//
	// @return The forms.
	std::span<const ContentForm> AllForms();
}
