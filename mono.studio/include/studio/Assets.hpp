#pragma once

// arch-waiver public-header: forward studio API. Editor integrations use this
// complete asset authoring contract.

// Which properties name content, and what a picker should therefore offer.
//
// **A table in the editor rather than a field on `PropertyDescriptor`, and that
// is a deliberate trade.** The tidy answer is to mark the property itself - a
// `Mesh` property would say it names a mesh, and the picker would ask. It is the
// wrong answer here for a structural reason: `PropertyDescriptor` lives in
// `ecs`, `AssetKind` lives in `assets`, and `assets` is six tiers above `ecs`.
// Threading a content kind down would mean either a second enum in `ecs` that
// mirrors `AssetKind` and drifts from it, or an edge the tier check refuses at
// configure time.
//
// So the knowledge lives next to the one thing that uses it. **What that costs
// is stated rather than hidden**: a content property added later and not added
// here gets a plain text field instead of a picker. That is a missing
// convenience and never a wrong value - the field still writes the same name,
// and a scene authored through it is identical.
//
// **Keyed on the property's spelling alone, not on its class.** Every content
// property in the engine is unambiguous by name - `Mesh`, `TextureID`,
// `Texture`, `SoundId`, `Image` - and adding the class would mean five more
// rows to name `Texture` on `ParticleEmitter`, `Beam` and `Trail` separately,
// each of which is the same answer. If a class ever wants a `Name` property
// called `Mesh` that is not a mesh, this is where that stops being true and the
// key grows.
//
// @tier client

#include <engine/assets/AssetKind.hpp>
#include <engine/game/Values.hpp>

#include <string_view>

namespace studio {

	// Which kind of content a property names.
	//
	// @param property The property's spelling, as `PropertyDescriptor::Spelling`
	//        gives it.
	// @return The kind, or `AssetKind::Unknown` for a property that does not
	//         name content - which is almost all of them, and is what makes the
	//         picker appear on the five that do rather than on every string in
	//         the panel.
	// @since v0.10
	engine::assets::AssetKind ContentKindOfProperty(std::string_view property);

	// The property write a confirmed content picker produces.
	//
	// **A function rather than four lines in the panel, because those four lines
	// were wrong for a whole version and nothing could see it.** The panel built
	// a `game::PropertyValue`, set `Name` on it and left `Type` at its default of
	// `Opaque`; `game::WriteProperty` refuses a value whose type disagrees with
	// the descriptor's, on its first line, before it looks at the store. So every
	// choice made in the picker - a mesh, a texture, a material, a sound, an
	// image - was dropped one call short of the world.
	//
	// Nothing said so. `WriteProperty`'s `false` is used only to decide whether
	// an undo entry is worth recording, so a refused write and a write that
	// changed nothing are the same non-event, and the visible result was a
	// property that stayed blank and a `MeshPart` that kept drawing `MeshTable`'s
	// fallback cube - which is also exactly what a mesh that has not arrived
	// looks like.
	//
	// **Taking the type as a parameter is the fix**, not the assignment inside.
	// A caller can no longer forget it: there is nowhere to put a name without
	// also saying what kind of property is receiving it. Assuming
	// `PropertyType::Name` here would work today and would be the same coincidence
	// that broke it - `ContentKindOfProperty` matches on spelling, and nothing
	// promises the property behind a spelling stays a `Name`.
	//
	// @param type   The receiving property's `PropertyDescriptor::Type`.
	// @param chosen What the picker returned. Empty is the "Clear" button, and it
	//        has to reach the store as an invalid name rather than be skipped -
	//        a part with no mesh is a plain part, and that is a thing to want.
	// @return A value `game::WriteProperty` will accept for that descriptor.
	// @since v0.10
	engine::game::PropertyValue ChosenContentValue(engine::ecs::PropertyType type, std::string_view chosen);
}
