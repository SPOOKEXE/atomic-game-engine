#pragma once

// What a rig is, and where the pose it is in gets written.
//
// `assets::MeshVertex` carries four joint indices and weights, `bake` preserves
// the glTF streams, and the renderer builds one GPU palette per rig from these
// rows. `AnimationTrack` evaluation writes `Bone::Transform`; `ResolveBones`
// composes the result before the palette is collected.
//
// **A bone is an instance and not a row in a private table**, which is Roblox's
// arrangement and is also what the roadmap asks for one line later: a character
// controller that is "more modular than roblox standard humanoid" needs an
// author and a script to be able to reach one joint. An instance is reachable,
// saves, replicates and can be renamed; an array inside an animation system is
// none of those.
//
// **A bone hierarchy is not the transform hierarchy `Components.hpp` refuses.**
// That refusal is about parts: a part's `Transform` is world space and parenting
// re-resolves nothing, which is what buys physics a read with no propagation
// pass. A rig is the opposite case by construction - a forearm is defined
// relative to an upper arm and there is no other way to say it - and its cost is
// bounded by the rig rather than by the world. `ResolveBones` is the same shape
// as `ResolveAttachments`: one pass, in `PreRender`, over rows the draw path
// reads.
//
// **`Bone::ParentJoint` is the ordering and the instance tree is the
// ownership**, and they are not two copies of one fact. The tree says which
// drawable a bone belongs to and what an author called it. The joint indices say
// in what order the palette resolves, which the tree cannot say: sibling order
// in the store is archetype order rather than authored order, and a forward pass
// needs a parent evaluated before its child. `TagTable::Names` keeps its order
// for the same kind of reason, where the index *is* the bit.
//
// arch-waiver public-header: forward API. `Skeleton` and `Bone` are the rig storage `bake`
// feeds from imported or authored rig data.
// Decision 16.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// The palette slot a root bone hangs off, meaning "nothing".
	//
	// **`0xFFFF` rather than a signed sentinel**, because the field is the index
	// a vertex's joint stream names and those are unsigned in every format that
	// has them.
	inline constexpr uint16_t NO_JOINT = 0xFFFFu;

	// The most joints one skeleton may have.
	//
	// **Chosen against what a palette costs rather than against what a rig
	// wants.** A skinning frame is a `CFrame` at 28 bytes, so 256 joints is a
	// 7 KB palette per rig per frame - a uniform buffer rather than a texture
	// fetch, on every backend this engine targets. glTF's own common limit is
	// the same number for the same reason.
	inline constexpr uint16_t MAX_JOINTS = 256;

	// One joint of a rig, on a `Bone` instance.
	//
	// **Four frames and not one, because they answer four different
	// questions.** `Rest` is where the joint sits when nothing is playing;
	// `Transform` is what an animation adds on top of it; `InverseBind` takes a
	// vertex out of the mesh's own space and into this joint's; `WorldFrame` is
	// where the joint ended up. An animation writes the second and only the
	// second, which is what makes stopping a track a matter of clearing one
	// field rather than restoring a pose from somewhere.
	//
	// **`InverseBind` is not the inverse of the accumulated `Rest` chain, even
	// though a well-formed rig makes them agree.** glTF carries both and permits
	// them to disagree, and exporters routinely ship rigs where they do: the
	// inverse bind decides how the skin deforms and the rest pose decides what an
	// unanimated rig looks like. Deriving one from the other would import those
	// rigs wrong with nothing in the file to say so, which is the opposite case
	// from the derived facts this module refuses to store - there, one true
	// answer exists and a copy of it goes stale.
	//
	// **No scale anywhere, and that is a refusal rather than an omission.**
	// Every placement in this module is a `CFrame`, a scaled joint is a shear in
	// the palette, and neither `DrawInstance` nor the solver has anywhere to put
	// one. A file whose inverse bind carries scale is the importer's to bake into
	// the vertices.
	//
	// @since v0.19
	struct Bone {
		// Where this joint sits when nothing is playing, relative to its parent
		// joint. A root bone's is relative to the entity carrying the
		// `Skeleton`.
		core::CFrame Rest;

		// What an animation adds on top of `Rest`, in the parent joint's space.
		//
		// Roblox's `Bone.Transform`, and the identity means "the rest pose". An
		// animation handler writes this and nothing else on the row, so a track
		// that stops leaves the rig standing rather than collapsed.
		core::CFrame Transform;

		// Where the mesh's vertices sit relative to this joint at bind time.
		//
		// glTF's `inverseBindMatrices` entry for this slot, as a `CFrame`.
		core::CFrame InverseBind;

		// Where the joint ended up in world space, as of the last
		// `ResolveBones`.
		//
		// **Derived, never authored**, exactly as `Attachment::WorldFrame` is,
		// and the property surface makes the same split: `Transform` is writable
		// and `TransformedWorldCFrame` is read-only.
		core::CFrame WorldFrame;

		// Which palette slot this joint is, and therefore which slot a vertex's
		// joint index names.
		uint16_t Joint = 0;

		// The slot this one hangs off, or `NO_JOINT` for a root.
		//
		// **Strictly less than `Joint`, and that is what makes the resolve a
		// forward pass rather than a walk.** `ResolveBones` orders a rig's bones
		// by `Joint` once and then composes in that order, so every parent is
		// already resolved when its child is reached. An importer that cannot
		// produce that ordering has produced a cycle.
		uint16_t ParentJoint = NO_JOINT;
	};

	// What makes an entity a rig, on the skinned drawable itself.
	//
	// **Two fields, and neither is derivable from the bones under it.** Counting
	// the `Bone` rows present answers how many bones a *world* currently has,
	// which is a different question from how many slots the mesh's vertex stream
	// is allowed to name: a script may delete a bone, and an index past the end
	// of a palette is a read past the end of a buffer. The rig name is what an
	// animation clip is authored against, so playing a fox's walk cycle on a
	// dragon is refusable rather than merely wrong-looking.
	//
	// @since v0.19
	struct Skeleton {
		// What the file called this skin. Rule 4: it crosses a save file and a
		// wire, so it is a string somebody chose rather than an index into the
		// glTF's `skins` array.
		core::Name Rig;

		// How many palette slots the mesh's joint indices may name.
		//
		// Authored by the importer from the length of the skin's joint list, and
		// never recomputed from the tree.
		uint16_t JointCount = 0;

		// Explicit padding, for the reason `Components.hpp` opens with: this
		// component's object representation reaches a file.
		uint8_t Reserved[2] = {};
	};

	// Fills every `Bone::WorldFrame` under every `Skeleton`.
	//
	// A `void(Store &)` wrapper registers as an ordinary system, exactly as
	// `ResolveAttachments` does.
	//
	// **Runs in `PreRender` beside the other presentation-derived passes.** What
	// reads a bone's world frame is a skinning palette and whatever is welded to
	// a joint, both of which are drawn rather than simulated. A caller that needs
	// a joint's frame *during* the tick - a ragdoll, a constraint anchored to a
	// bone - is asking for something this pass does not promise, and should say
	// so rather than moving it earlier.
	//
	// **A bone whose `ParentJoint` names a slot no bone under the same skeleton
	// carries falls back to the skeleton's own frame.** That is the useful state
	// rather than an error: it is what a rig looks like while a script is
	// half-way through building one, and it stops a deleted bone from taking its
	// whole subtree to the origin.
	//
	// @param store The world to resolve in.
	// @return How many bones were resolved.
	size_t ResolveBones(ecs::Store &store);

	// The world-space frame of one bone, resolved on the spot.
	//
	// **For the callers that cannot wait for the pass**, which is
	// `ResolveAttachment`'s pair of cases: a script reading a bone's world frame
	// on the frame it made one, and a test. Everything on the draw path reads the
	// resolved field, because doing this per reader is the chain walk the field
	// exists to avoid.
	//
	// @param store The world.
	// @param bone  The bone instance.
	// @return Its world frame, or its own rest-times-transform when it sits under
	//         no skeleton.
	core::CFrame ResolveBone(const ecs::Store &store, ecs::Entity bone);

	// What a vertex bound to this joint is transformed by.
	//
	// **A function and not a field, for `ReflectCamera`'s reason.** The palette
	// entry is `WorldFrame * InverseBind`, and there are going to be at least two
	// callers of it - a renderer building a palette, and whatever welds a held
	// tool to a hand - so a second derivation is a second chance to get the order
	// of that product the wrong way round.
	//
	// @param bone The resolved bone.
	// @return The frame a bound vertex is moved by.
	core::CFrame SkinningFrameOf(const Bone &bone);

	// The entity carrying the `Skeleton` a bone belongs to.
	//
	// Walks up from the bone until a `Skeleton` is found, so a rig may nest its
	// bones as deeply as the author likes.
	//
	// @param store The world.
	// @param bone  The bone instance.
	// @return The rig's entity, or a null entity when the bone sits under none.
	ecs::Entity SkeletonOf(const ecs::Store &store, ecs::Entity bone);

	// The `Bone` class id, registering the scene tree on first call.
	//
	// **Derives from `Instance` and not from Roblox's `Attachment`, which is the
	// one place this rig departs from that tree.** An `Attachment` already
	// carries a frame relative to its parent *part* and `ResolveAttachments`
	// already fills it, so a class carrying both components would have two world
	// frames on one row, resolved by two passes against two different parents.
	// That is the pair of opinions `Attachment`'s own class comment refuses, and
	// inheriting it here would create it rather than avoid it.
	//
	// @return The class id.
	ecs::ClassId BoneClass();
}
