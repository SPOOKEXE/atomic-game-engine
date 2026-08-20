#pragma once

// A frame as nodes and resources, compiled to an order and then run.
//
// **This replaced `graph::Pipeline`, which is deleted.** That type was a flat
// list of stage records checked for one property. This holds resources as well
// as nodes, partitions them into what runs once a frame and what runs once a
// view, and hands the result to something that owns a device. Everything
// `Pipeline` did is here, so keeping it would have been a second description of
// the same frame - see below.
//
// The fixed renderer description that preceded this type is gone. A world now
// owns one authored document, the graph compiles it, and the device backend
// accepts only node kinds it can execute. There is no parallel pass list to
// keep in step with the graph.
//
// ## Declaration order is the order, and the resources check it
//
// **A node names what it reads and writes and never names another node**, and
// the order it runs in is the order it was declared in. The reads and writes are
// a *check* on that order rather than a derivation of it.
//
// **Deriving it was tried first and a read-modify-write chain kills it.**
// A draw writes colour, `transparent` draws onto it, `overlay` onto that, and
// `interface` onto that. Every one both reads and writes the same resource, so
// a producer-to-consumer graph over the four is a cycle.
// The first version of this compiled the standard frame into exactly that and
// refused it. And the cycle is not a tangle to be undone: which of overlay and
// interface goes on top is *authored*, and no dependency analysis recovers it.
//
// The graph therefore preserves authored order instead of deriving a different
// order from dependencies. What the resources buy is that an author
// who puts a pass above the thing it samples is told which pass, at compile,
// rather than seeing a frame lit by whatever was in that memory.
//
// ## Per view, and it is the point of the version
//
// `Node::Scope` makes the sharing rule explicit. A shadow map is per light and
// shareable by every view of a world; a colour pass is per view and is not.
// `Compile` partitions on it, so four split-screen views of one world
// pay for **one** shadow pass and four colour passes - which is what "handle
// multiple worlds in parallel" resolves to once it stops being a slogan.
//
// ## No device, and that is what makes it testable
//
// This module is L9 `shared` and links no GPU. Execution calls back into a
// `NodeRunner` the caller supplies, so `render` implements one over SDL and a
// test implements one that records what it was asked to do. That is
// `bake::Graph`'s arrangement - it touches no filesystem for the same reason -
// and it is why the whole of this is exercised on a machine with no GPU.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::graph {

	// What a resource is, so an executor knows what to make.
	//
	// @since v0.11
	enum class ResourceKind : uint8_t {
		// A colour attachment.
		Colour,

		// A depth attachment.
		Depth,

		// A texture a pass samples rather than writes.
		Texture,

		// A texture a compute pass writes through an unordered access view.
		//
		// **Distinct from `Colour` because the hardware treats it so**, and
		// because a pass writing storage is `Compute` work while a pass writing
		// a colour attachment is `Raster` work - `docs/PIPELINE_NODES.md` §1.5
		// fault 10 is what conflating the two costs.
		Storage,

		// A structured buffer rather than an image: a light list, a draw list,
		// a reduction's output.
		Buffer,

		// A viewpoint: where something looks from, and through what lens.
		//
		// **Not a GPU resource either**, and a kind of its own for
		// `Entities`' reason - it is a value a node reads, not a thing a pass
		// draws into.
		//
		// **What it is for is that "the camera" was ambient.** Frustum culling,
		// the back-to-front sort and every projection matrix in the frame read
		// whichever camera the view happened to have, so a pipeline could not
		// say *cull against the light* or *order for this mirror*. The eye is
		// one viewpoint among several and the graph had no word for any of them.
		//
		// A node that takes one and is given none uses the view's own, which is
		// what makes this additive: a pipeline that wires no cameras behaves
		// exactly as it did before there were any.
		Camera,

		// A list of which instances a pass should draw.
		//
		// **Not an image and not a GPU resource at all**, which is why it is a
		// kind of its own rather than a `Buffer`. It is indices into the view's
		// draw list - pointers, in the sense that matters - and it lives on the
		// CPU until something uploads it.
		//
		// **What it is for is putting the culling in the graph.** Frustum
		// culling, tag filtering and back-to-front ordering are all
		// list-in-list-out, they all happen before any pass draws, and they were
		// all a fixed sequence inside `Renderer::Render` - so the one thing a
		// pipeline could never say was *which* geometry a pass draws. This is
		// the wire that says it.
		//
		// A pass that reads one draws what is in it. A pass that reads none
		// draws nothing, which is what makes an empty filter a black frame
		// rather than a full one.
		Entities,
	};

	// A stable, human-readable name for a resource kind.
	const char *Describe(ResourceKind kind);

	// What is in each pixel.
	//
	// **Separate from `ResourceKind` because the two are orthogonal**, which is
	// the whole of `docs/PIPELINE_NODES.md` §3. A kind says whether a pass may
	// render into a thing or only sample it; a format says how many bits it gets
	// and how many channels. A wire is legal when the kinds allow it and *lossy*
	// when the formats disagree, and only the second is a warning rather than a
	// refusal.
	//
	// **Not every format the hardware has**, and deliberately: this is the set a
	// frame is actually described in, plus the block-compressed formats an
	// uploaded texture arrives in. A format nothing in the engine can produce
	// would be a name with no meaning.
	//
	// @since v0.11
	enum class ResourceFormat : uint8_t {
		// Eight bits a channel, which is what a display takes.
		//@{
		R8,
		RG8,
		RGBA8,
		//@}

		// The same, read and written through the sRGB transfer curve.
		RGBA8_SRGB,

		// Ten bits of colour and two of alpha. **The right format for normals**
		// and for a tone-mapped frame - `PIPELINE_NODES.md` §1.5 fault 6 is a
		// frame that used four times the bits for the first of those.
		RGB10A2,

		// Eleven, eleven and ten bits of float with no alpha. HDR colour at half
		// the cost of `RGBA16F`, when nothing needs the alpha.
		RG11B10F,

		// Half floats.
		//@{
		R16F,
		RG16F,
		RGBA16F,
		//@}

		// Full floats, for depth a pass has linearised and for reductions.
		//@{
		R32F,
		RG32F,
		//@}

		// Depth, with and without a stencil channel.
		//@{
		D24S8,
		D32F,
		//@}

		// Block-compressed, which is how a texture arrives from the content
		// store. **Named here so an upload is describable**: `PIPELINE_NODES.md`
		// §1.4 counts eight of these bound to one draw, and a pipeline that
		// cannot say what it sampled cannot say what it cost.
		//@{
		BC1_SRGB,
		BC3,
		BC5,
		BC7_SRGB,
		//@}
	};

	// A stable, human-readable name for a format.
	//
	// **The spelling a capture tool uses**, so a diagnostic here and a row in
	// somebody's RenderDoc window say the same word.
	//
	// @param format The format to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ResourceFormat format);

	// How many bits one pixel of a format takes.
	//
	// @param format The format.
	// @return The bits, counting every channel. Block-compressed formats report
	//         their amortised cost per pixel, which is what a memory budget
	//         wants and is why this is bits rather than bytes.
	uint32_t BitsPerPixel(ResourceFormat format);

	// How many channels a format carries.
	//
	// @param format The format.
	// @return One to four. Depth formats report one; their stencil is not a
	//         channel anything samples as colour.
	uint32_t ChannelCount(ResourceFormat format);

	// Whether a format has an alpha channel at all.
	//
	// **The question `PIPELINE_NODES.md` fault 3 turns on.** A blank alpha is
	// only wasteful if there is one; asking this before reporting it is what
	// stops the check firing on every `RG11B10F` in a frame.
	//
	// @param format The format.
	// @return Whether a fourth channel exists.
	bool HasAlpha(ResourceFormat format);

	// How often a node runs.
	//
	// **Three, and it was a boolean.** `PerView` said per-view or not, which
	// conflated two different "not"s: a shadow map is rendered once for a
	// *world* however many views look into it, and an overlay is drawn once for
	// the *frame* however many worlds it composites. `RenderGraph::Execute`
	// already runs the shared block once per distinct world - the distinction
	// was live in the executor and absent from the vocabulary.
	//
	// **`Surface` is deliberately not here.** A fourth value for the per-surface
	// passes would be a word with no block to run in: the surface pass loops
	// inside a per-view one, and nothing schedules per surface. Shipping it
	// inert is the shape rule 6 exists to prevent; it arrives when there is
	// something for it to name.
	//
	// @since v0.11
	enum class NodeScope : uint8_t {
		// Once for the whole frame. The overlay, the editor's chrome, a present.
		Frame,

		// Once per distinct world, however many views look into it. A shadow
		// atlas: four split-screen views of one world pay for one.
		World,

		// Once per view. Everything that draws what a camera can see.
		View,
	};

	// A stable, human-readable name for a scope.
	//
	// **The spelling the document format uses**, so a diagnostic and a saved
	// pipeline never disagree about what something is called.
	//
	// @param scope The scope to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(NodeScope scope);

	// Whether a scope makes a node run once per view.
	//
	// **The predicate `Compile`'s partition turns on**, and the reason it is a
	// function rather than `scope == NodeScope::View` written out at each site:
	// that comparison is what the boolean already was, and spelling it by hand
	// is how a fourth scope would get silently misfiled the day it arrives.
	//
	// @param scope The scope.
	// @return Whether the per-view block is where it belongs.
	bool RunsPerView(NodeScope scope);

	// One thing a node reads or writes.
	//
	// @since v0.11
	struct ResourceDesc {
		// What it is called - `colour`, `depth`, `shadow`.
		//
		// **A name and not an index**, which is rule 4 and the same reason
		// `Attachment` carries one: a graph is the sort of thing a game file or
		// an editor will carry, and an index means a different resource the
		// moment the list is reordered.
		core::Name Name;

		// What kind of thing it is.
		ResourceKind Kind = ResourceKind::Colour;

		// What is in each pixel.
		ResourceFormat Format = ResourceFormat::RGBA8;

		// Its size, or zero to mean "whatever the view is".
		//
		// **Zero rather than a separate flag**, because a resource that follows
		// the view is the ordinary case and a flag would be a second field to
		// keep in step with the numbers beside it.
		//@{
		uint32_t Width = 0;
		uint32_t Height = 0;
		//@}

		// Whether this resource exists outside the graph.
		//
		// **The swapchain, and a history buffer.** RDG calls these *external*
		// and everything else *transient*, and the distinction is load-bearing
		// in three places: an external resource may not be aliased with another,
		// it may be read before any pass in this graph wrote it, and a pass
		// writing one is producing the frame rather than producing something for
		// a later pass.
		//
		// **Added because `PipelineDiagnostics` reported the standard frame.**
		// It said `opaque` was disconnected - nothing in the graph reads the
		// colour it writes - and it was right: the renderer presents that target
		// itself, and the graph had no way to say so. The alternative was to
		// exempt the finding, which would have been the checker lying to protect
		// a model that was wrong.
		bool External = false;

		// How much smaller than the view, when the size is not absolute.
		//
		// **One means full, two means half on each axis, four means a quarter.**
		// A fraction rather than a second pair of numbers, because that is what
		// a downsample chain actually is - `PIPELINE_NODES.md` §1.4 counts six
		// resolutions in one frame reached by seven halvings, and every one of
		// them is a divisor rather than an authored size.
		//
		// Ignored when `Width` and `Height` are set. Zero is read as one, so a
		// resource written before this field existed still means "the view".
		uint32_t Divisor = 1;

		// The size this resolves to for a view of a given size.
		//
		// **Here rather than in the renderer**, because it is the answer an
		// editor has to print on a wire and the answer an allocator has to
		// honour, and two derivations of it would eventually disagree.
		//
		// @param viewWidth  The view's width in pixels.
		// @param viewHeight Its height.
		// @param outWidth   Filled in. Never zero - a divisor that would round
		//                   to nothing clamps to one, because a zero-sized
		//                   target is not something a driver accepts.
		// @param outHeight  Filled in.
		void Resolve(uint32_t viewWidth, uint32_t viewHeight, uint32_t &outWidth, uint32_t &outHeight) const;
	};

	// A handle to a resource in one graph.
	//
	// @since v0.11
	struct ResourceId {
		// The value meaning "no resource". Zero is never issued.
		static constexpr uint32_t NONE = 0;

		// The handle itself.
		uint32_t Value = NONE;

		// Whether this names a resource.
		//
		// @return `false` for a default handle.
		constexpr bool IsValid() const {
			return Value != NONE;
		}

		// @param other The handle to compare.
		// @return `true` when both name the same resource.
		constexpr bool operator==(const ResourceId &other) const = default;
	};

	// A handle to a node in one graph.
	//
	// @since v0.11
	struct NodeId {
		// The value meaning "no node". Zero is never issued.
		static constexpr uint32_t NONE = 0;

		// The handle itself.
		uint32_t Value = NONE;

		// Whether this names a node.
		//
		// @return `false` for a default handle, which is what `AddNode` returns
		//         when it refuses.
		constexpr bool IsValid() const {
			return Value != NONE;
		}

		// @param other The handle to compare.
		// @return `true` when both name the same node.
		constexpr bool operator==(const NodeId &other) const = default;
	};

	// One thing a node was configured with.
	//
	// @since v0.11
	struct NodeParameter {
		core::Name Key{};
		std::string Value{};
	};

	// One pass of a frame.
	//
	// @since v0.11
	struct Node {
		// What this node is called, for a profiler and for a diagnostic.
		core::Name Name{};

		// What it does, which is what an executor switches on.
		//
		// **Apart from `Name` on purpose.** Two shadow passes for two lights are
		// one kind and two names, and an executor that switched on the name
		// would need one case per instance of a pass rather than one per sort of
		// pass.
		core::Name Kind{};

		// What it must be able to read.
		std::vector<ResourceId> Reads{};

		// What it writes.
		std::vector<ResourceId> Writes{};

		// How often the work runs.
		//
		// **The field that decides how many views cost**, and the reason this
		// type exists rather than a longer function. See `NodeScope`: it was a
		// boolean, and the boolean could not tell "once for the frame" apart
		// from "once for the world".
		NodeScope Scope = NodeScope::View;

		// Whether it may be skipped when there is nothing for it to do.
		bool Optional = false;

		// Whether it runs at all.
		//
		// **Distinct from being dead, and an editor must keep them distinct.**
		// A node nobody demanded is dead; a node somebody switched off is
		// disabled, and a black screen whose cause is one presented as the other
		// is unexplainable.
		bool Enabled = true;

		// What this particular node was configured with.
		//
		// **The difference between a kind and a node.** Two `filter-tag` nodes
		// are the same kind and filter different tags; two `raster` nodes are
		// the same kind and run different shaders. Without this a pipeline can
		// only say *which* passes run and never *how* - which is why
		// `cull-distance` had no radius and every pass ran the shader its kind
		// was compiled with.
		//
		// **Strings, and deliberately.** A parameter is authored text that has
		// to survive a save file, a diff and somebody typing it; parsing it into
		// a number here would put the parse in the wrong place and give two
		// answers for what an unset one means. Whoever reads a parameter decides
		// what its absence means, which is how `filter-tag` reads no mask as
		// *keep everything*.
		//
		// Small and linear, for `EntityFlow`'s reason: a node has a handful of
		// these and a map costs more in the lookup than the scan.
		std::vector<NodeParameter> Parameters{};

		// What a parameter was set to.
		//
		// @param key Which one.
		// @return The text, or null when it was never set. **Null rather than
		//         empty**: "unset" and "set to nothing" are different answers
		//         and only the first should take a default.
		const std::string *Parameter(core::Name key) const;

		// A parameter read as a number.
		//
		// @param key      Which one.
		// @param fallback What to use when it is unset or unreadable.
		// @return The value. **Unreadable falls back rather than refusing**,
		//         because a half-typed number in an editor is a state somebody
		//         is passing through rather than a pipeline to reject.
		float Number(core::Name key, float fallback) const;

		// A parameter read as an unsigned integer, decimal or `0x` hex.
		//
		// @param key      Which one.
		// @param fallback What to use when it is unset or unreadable.
		// @return The value.
		uint32_t Integer(core::Name key, uint32_t fallback) const;
	};

	// Why a graph is not runnable.
	//
	// @since v0.11
	enum class GraphStatus : uint8_t {
		// It is runnable.
		Ok,

		// Two nodes share a name, so a profiler cannot tell them apart.
		DuplicateNode,

		// A node neither reads nor writes, so it cannot affect the frame and
		// cannot be observed either.
		//
		// **Writing nothing is not enough to be this.** `viewer` and `capture`
		// are sinks: they consume a resource and produce a panel or a file,
		// which is outside the graph. Refusing them made two catalogue kinds
		// that could never be placed.
		WritesNothing,

		// A node names a resource this graph does not hold.
		UnknownResource,

		// A node reads something nothing earlier wrote.
		//
		// **Earlier is the whole of it.** Declaration order is the execution
		// order, so this covers a producer that was deleted, one that was never
		// added, and one that is simply below the node reading it - which are
		// three causes with one symptom and one fix.
		ReadsBeforeWrite,

		// Past `MAXIMUM_NODES`.
		TooManyNodes,

		// A shared node sits between two per-view nodes.
		//
		// **The one arrangement a three-block frame cannot express.** Shared
		// work runs either before every view or after every one of them; a
		// shared node declared in the middle would mean "every view's pass A,
		// then this once, then every view's pass B", which is four blocks and
		// then six and then however many the author writes. Refused rather than
		// silently hoisted to one end, because either end changes what it reads.
		SharedBetweenViews,

		// A per-view node writes something a shared node also writes.
		//
		// **The one failure this partition can produce and nothing else can
		// catch.** A shared node runs once and a per-view node runs per view, so
		// both writing one resource means the last view wins and the shared
		// node's work is silently discarded - which looks like the shared pass
		// not running at all.
		SharedWriteConflict,
	};

	// A stable, human-readable name for a status.
	//
	// @param status The status to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(GraphStatus status);

	// A graph compiled to an order.
	//
	// **Three lists rather than one with a flag**, so an executor loops rather
	// than branches. Holding one list and testing a bool per node per view would
	// put the partition decision inside the hot loop, where it is re-answered
	// every frame for an answer that changes only when the graph is edited.
	//
	// The frame is `Shared`, then `PerView` once for each view, then `Final`.
	//
	// **Why there is a third list and not two.** A real frame has shared work at
	// both ends. A shadow map is shared and must be written *before* any view
	// samples it; an overlay and an editor's chrome are the window's rather than
	// a view's and must be drawn *after* every view has finished with it. Two
	// lists can only put shared work at one end, and putting the overlay first
	// would draw it under the world.
	//
	// **Which end a shared node lands at is its declaration position**, not a
	// second flag. Declaration order is already the execution order, so a shared
	// node before the first per-view node runs before and one after the last
	// runs after - and nothing has to be kept in step with anything.
	//
	// @since v0.11
	struct CompiledGraph {
		// The nodes that run once, before any view.
		std::vector<NodeId> Shared;

		// The nodes that run once per view, in order.
		std::vector<NodeId> PerView;

		// The nodes that run once, after every view.
		std::vector<NodeId> Final;
	};

	// What an executor is told about the node it is running.
	//
	// @since v0.11
	struct RunContext {
		// Which node.
		NodeId Node;

		// Its name and kind, so a runner needs no second lookup.
		//@{
		core::Name Name;
		core::Name Kind;
		//@}

		// Which view this is for, or `WHOLE_FRAME` for a shared node.
		size_t View = 0;

		// Which world this is for, as a position among the frame's distinct
		// worlds - not the caller's identifier.
		//
		// **The field that makes "shared" mean what it says.** A shadow map is
		// one per world per light, so a frame showing two worlds needs two of
		// them; a shared node therefore runs once per distinct world rather than
		// once per frame, and this is how a runner tells which it is being asked
		// for. `WHOLE_FRAME` on a node in `Final`, which belongs to the window
		// rather than to any world.
		size_t World = 0;

		// The value `View` takes for a node that is not per view.
		static constexpr size_t WHOLE_FRAME = static_cast<size_t>(-1);

		// What it reads and writes, as the graph holds them.
		//@{
		std::span<const ResourceId> Reads;
		std::span<const ResourceId> Writes;
		//@}
	};

	// What runs a node.
	//
	// **The seam that keeps a device out of L9.** `render` implements this over
	// SDL; a test implements one that records what it was asked to do, which is
	// how every case in `graph/tests/RenderGraph.cpp` runs on a machine with no
	// GPU.
	//
	// @since v0.11
	class NodeRunner {
	  public:
		virtual ~NodeRunner() = default;

		// Runs one node.
		//
		// @param context What to run and for which view.
		// @return `false` to abandon the frame. An executor stops rather than
		//         carrying on, because a pass that failed leaves its writes
		//         undefined and everything downstream reads them.
		virtual bool Run(const RunContext &context) = 0;
	};

	// A frame, as nodes over resources.
	//
	// @since v0.11
	class RenderGraph {
	  public:
		// The most nodes one graph may hold.
		//
		// A bound so an editor cannot make a graph that takes unbounded time to
		// compile. `bake::Graph` takes the same shape for the same reason; this
		// is smaller because a frame with a thousand passes is a mistake rather
		// than a large scene.
		static constexpr uint32_t MAXIMUM_NODES = 256;

		// Declares a resource.
		//
		// @param desc What it is.
		// @return The handle, or an invalid one for an unnamed resource or a
		//         duplicate name.
		ResourceId AddResource(const ResourceDesc &desc);

		// Adds a node.
		//
		// @param node The node. Copied.
		// @return The handle, or an invalid one when the graph is full or the
		//         node is unnamed.
		NodeId AddNode(Node node);

		// Turns a node on or off.
		//
		// **An operation rather than a field somebody reaches into**, because
		// this is the first of `Editor`'s mutations and every one of them has to
		// be expressible as data for a graph to be saved or scripted.
		//
		// @param node The node.
		// @param enabled Whether it runs.
		// @return `false` for an unknown node.
		bool SetEnabled(NodeId node, bool enabled);

		// Checks everything that can be checked without running.
		//
		// @param offender Filled in with the node or resource that failed.
		// @return `Ok`, or why not.
		GraphStatus Validate(core::Name &offender) const;

		// Works out the order.
		//
		// **Disabled nodes are left out of the compile rather than skipped at
		// run time**, so the cost of switching a pass off is zero per frame
		// rather than a branch per node per view.
		//
		// @param out Filled in on success, left alone otherwise.
		// @param offender Filled in with what failed.
		// @return `Ok`, or why not. Validates first, so a caller need not.
		GraphStatus Compile(CompiledGraph &out, core::Name &offender) const;

		// Runs a compiled order.
		//
		// @param compiled What `Compile` produced.
		// @param runner What actually does the work.
		// @param views How many views to run the per-view nodes for. Zero runs
		//              the shared nodes once and the final nodes, which is what
		//              a headless host presenting no view does.
		// @return `false` when the runner abandoned the frame.
		bool Execute(const CompiledGraph &compiled, NodeRunner &runner, size_t views) const;

		// Runs a compiled order over views that may belong to different worlds.
		//
		// **The shared block runs once per distinct world, not once per frame**,
		// which is what `Node::PerView = false` has always claimed and could not
		// deliver while a frame had only one world in it. A shadow map is per
		// world per light: two views of one world share one, and two views of
		// two worlds need two, fitted to two sets of bounds. Running one for the
		// frame would light one world's geometry with the other's sun.
		//
		// **Worlds are identified by value and grouped in first-appearance
		// order**, so a caller passes whatever it already uses to tell worlds
		// apart and this never learns what a world is. Views of one world do not
		// have to be adjacent.
		//
		// `Final` runs once at the end however many worlds there were, because
		// the overlay and the editor's chrome are the window's.
		//
		// @param compiled What `Compile` produced.
		// @param runner   What actually does the work.
		// @param worlds   One entry per view, naming that view's world. Its size
		//                 is the view count.
		// @return `false` when the runner abandoned the frame.
		bool
		Execute(const CompiledGraph &compiled, NodeRunner &runner, std::span<const uint64_t> worlds) const;

		// One view's work: this world's shared block, then that view's.
		//
		// **What `Execute` is made of, and what a frame of several pipelines
		// needs.** A camera with its own pipeline cannot go through `Execute`,
		// because `Execute` walks every view and ends with the frame's own
		// block - and the frame has one of those however many pipelines drew
		// into it. So a caller that is picking a graph per view drives these two
		// itself.
		//
		// @param compiled What `Compile` produced, for *this view's* pipeline.
		// @param runner   What does the work.
		// @param view     Which view this is, as `RunContext::View`.
		// @param world    Which world it belongs to, as a position among the
		//                 frame's distinct worlds.
		// @param shared   Whether to run the shared block. A caller running
		//                 several views of one world through one pipeline passes
		//                 `true` for the first and `false` after - that is what
		//                 makes a shadow map per world rather than per view.
		// @return `false` when the runner abandoned the frame.
		bool ExecuteView(
			const CompiledGraph &compiled, NodeRunner &runner, size_t view, size_t world, bool shared
		) const;

		// The frame's own block, once, after every view.
		//
		// The overlay and the editor's chrome are the window's rather than any
		// camera's, so they run once over whatever the cameras produced -
		// whichever pipelines those were.
		//
		// @param compiled What `Compile` produced, for the *frame's* pipeline.
		// @param runner   What does the work.
		// @return `false` when the runner abandoned the frame.
		bool ExecuteFinal(const CompiledGraph &compiled, NodeRunner &runner) const;

		// A node, or null.
		//
		// @param node The handle.
		// @return The node, or null for an unknown handle.
		const Node *Find(NodeId node) const;

		// A resource, or null.
		//
		// @param resource The handle.
		// @return The descriptor, or null for an unknown handle.
		const ResourceDesc *FindResource(ResourceId resource) const;

		// How many nodes the graph holds, enabled or not.
		size_t Count() const {
			return Nodes.size();
		}

		// How many resources it declares.
		size_t ResourceCount() const {
			return Resources.size();
		}

	  private:
		std::vector<Node> Nodes;
		// Runs one compiled block, telling every node which view and world it is
		// for. What all three `Execute` entry points are made of.
		bool RunBlock(const std::vector<NodeId> &block, NodeRunner &runner, size_t view, size_t world) const;

		std::vector<ResourceDesc> Resources;
	};

}
