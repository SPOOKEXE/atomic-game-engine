#pragma once

// The engine profiler. One macro feeds three consumers:
//
//   - Tracy, for the real thing - a second process, every thread, full history.
//   - FrameGraph, for the in-game F5 overlay, which has to work with nothing
//     attached and on a machine that is not the developer's.
//   - HeapProfile, for what the span *allocated* rather than what it cost.
//
// **The heap tag is on this macro rather than on a macro of its own, and that
// is what makes the heap profiler granular at all.** These scopes are already
// placed where the work is, several hundred of them across the engine, and a
// second family of macros beside them would be a second set of placements to
// keep in step - which is the shape of a thing that is right on the day it is
// written and wrong a month later. `ENGINE_HEAP_SCOPE` still exists, for the
// places that allocate and are not worth timing.
//
// `ENGINE_PROFILE_DYNAMIC` tags with its **fallback literal** and not its
// runtime name. A tag tree node is never removed, so a caller naming a zone per
// script chunk would fill the tree and every tag after it would be charged to
// an ancestor. `ENGINE_PROFILE_DYNAMIC_STABLE` does pass its name through,
// because its callers name a bounded set - the scheduler names its systems -
// and the tag tree copies what it is given rather than keeping the view.
//
// Heap tags are pushed whether or not anything is collecting, which the frame
// scopes are not. Allocation happens whether or not the panel is open, and a
// tree that only knew about the bytes taken since somebody pressed F5 would
// answer the wrong question at the moment it was asked.
//
// The planned userland profiler is a different thing entirely. It belongs at
// L13 and shares no code with this engine profiler.
//
// @tier L0 · shared

#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>

#if defined(ENGINE_TRACY)
#include <tracy/Tracy.hpp>
#endif

// Joins two preprocessor tokens after the public wrapper expands them.
//
// @param a Left token.
// @param b Right token.
#define ENGINE_PROFILE_CONCAT_(a, b) a##b

// Expands and joins two tokens to form each scope helper's unique local name.
//
// @param a Left token.
// @param b Right token.
#define ENGINE_PROFILE_CONCAT(a, b) ENGINE_PROFILE_CONCAT_(a, b)

// The build binds product translation units to their profiler owner. Engine
// modules and external consumers use the engine owner by default.
#if !defined(ENGINE_PROFILE_OWNER)
#define ENGINE_PROFILE_OWNER ::engine::core::ProfileOwner::Engine
#endif

#if defined(ENGINE_TRACY)

// Opens a Tracy zone and FrameGraph scope until the enclosing C++ scope exits.
//
// `name` must be a string literal: Tracy keeps the pointer for the life of
// the process, and FrameGraph reads it after the frame has ended.
//
// **`ZoneNamedN` rather than `ZoneScopedN`, so two of these may share a C++
// scope.** `ZoneScopedN` declares a variable called `___tracy_scoped_zone`,
// which is one fixed name - so a second macro beside the first was a
// redeclaration error naming a Tracy internal, from a header the caller never
// wrote, and only in builds with `ENGINE_TRACY` on. The FrameGraph half was
// already unique per line; this makes the Tracy half agree. Two on the *same*
// line still collide, because the source-location record is per line, and
// that is the one restriction left.
//
// @param name String literal used by both profiling consumers.
// @param category ProfileCategory used by FrameGraph.
#define ENGINE_PROFILE_CAT(name, category)                                                                   \
	ZoneNamedN(ENGINE_PROFILE_CONCAT(engineProfileZone_, __LINE__), name, true);                             \
	ENGINE_HEAP_SCOPE(name);                                                                                 \
	::engine::core::FrameGraph::Scope ENGINE_PROFILE_CONCAT(engineProfileScope_, __LINE__) {                 \
		name, category, ENGINE_PROFILE_OWNER                                                                 \
	}

// Opens a dynamically named Tracy zone and copied-name FrameGraph scope.
//
// Use for a span whose name is not known at compile time - a script chunk, a
// graph node kind. The `fallback` literal is what Tracy groups the zone
// under, and what the overlay falls back to when the caller had nothing to
// say.
//
// The text is **copied** into the frame's own pool, so `view` may be a
// local. That costs a string assign per span per frame while the overlay is
// open and nothing at all when it is closed. Use the STABLE form below when
// the caller already owns storage that outlives the frame.
//
// @param fallback String literal used by Tracy and for an empty FrameGraph name.
// @param view Runtime string view copied by FrameGraph when collected.
// @param category ProfileCategory used by FrameGraph.
#define ENGINE_PROFILE_DYNAMIC(fallback, view, category)                                                     \
	ZoneNamedN(ENGINE_PROFILE_CONCAT(engineProfileZone_, __LINE__), fallback, true);                         \
	do {                                                                                                     \
		if (!(view).empty()) {                                                                               \
			ENGINE_PROFILE_CONCAT(engineProfileZone_, __LINE__).Name((view).data(), (view).size());          \
		}                                                                                                    \
	} while (0);                                                                                             \
	ENGINE_HEAP_SCOPE(fallback);                                                                             \
	::engine::core::FrameGraph::CopiedScope ENGINE_PROFILE_CONCAT(engineProfileScope_, __LINE__) {           \
		fallback, view, category, ENGINE_PROFILE_OWNER                                                       \
	}

// Opens a dynamically named Tracy zone and stable-name FrameGraph scope.
//
// The FrameGraph name is not copied. `view` MUST remain valid until at least
// the next EndFrame(): the overlay reads
// the published spans after the frame that produced them has ended, so a
// name built into a local would dangle by the time it is drawn.
//
// This is for a caller that owns the string for the life of the run - the
// scheduler, which owns its system names - and it is the only reason the
// copying form is not the only form. `fallback` labels the Tracy zone;
// FrameGraph records `view` as given, including an empty view.
//
// @param fallback String literal used by Tracy.
// @param view Runtime string view backed by stable caller-owned storage.
// @param category ProfileCategory used by FrameGraph.
#define ENGINE_PROFILE_DYNAMIC_STABLE(fallback, view, category)                                              \
	ZoneNamedN(ENGINE_PROFILE_CONCAT(engineProfileZone_, __LINE__), fallback, true);                         \
	do {                                                                                                     \
		if (!(view).empty()) {                                                                               \
			ENGINE_PROFILE_CONCAT(engineProfileZone_, __LINE__).Name((view).data(), (view).size());          \
		}                                                                                                    \
	} while (0);                                                                                             \
	ENGINE_HEAP_SCOPE(view);                                                                                 \
	::engine::core::FrameGraph::Scope ENGINE_PROFILE_CONCAT(engineProfileScope_, __LINE__) {                 \
		view, category, ENGINE_PROFILE_OWNER                                                                 \
	}

// Profiles producer work on a non-frame thread. The frame owner reports the
// measured duration after joining, so opening a FrameGraph scope here would
// only count a deliberately rejected cross-thread span.
#define ENGINE_PROFILE_PRODUCER_DYNAMIC_STABLE(fallback, view)                                               \
	ZoneNamedN(ENGINE_PROFILE_CONCAT(engineProfileZone_, __LINE__), fallback, true);                         \
	do {                                                                                                     \
		if (!(view).empty()) {                                                                               \
			ENGINE_PROFILE_CONCAT(engineProfileZone_, __LINE__).Name((view).data(), (view).size());          \
		}                                                                                                    \
	} while (0);                                                                                             \
	ENGINE_HEAP_SCOPE(view)

// Marks the end of a frame in Tracy. FrameGraph uses BeginFrame() and
// EndFrame() separately.
#define ENGINE_PROFILE_FRAME() FrameMark

// Reports whether a Tracy profiler is attached. TRACY_ON_DEMAND collects
// nothing before that, so a short run with no listener records nothing.
#define ENGINE_PROFILE_ATTACHED() TracyIsConnected

#else

// Opens a FrameGraph scope until the enclosing C++ scope exits.
//
// `name` must remain valid until at least the next EndFrame().
//
// @param name Stable name used by FrameGraph.
// @param category ProfileCategory used by FrameGraph.
#define ENGINE_PROFILE_CAT(name, category)                                                                   \
	ENGINE_HEAP_SCOPE(name);                                                                                 \
	::engine::core::FrameGraph::Scope ENGINE_PROFILE_CONCAT(engineProfileScope_, __LINE__) {                 \
		name, category, ENGINE_PROFILE_OWNER                                                                 \
	}

// Opens a copied-name FrameGraph scope until the enclosing C++ scope exits.
//
// FrameGraph copies `view` while collecting and uses `fallback` when it is
// empty, so caller-owned runtime text may be local.
//
// @param fallback Stable name used when `view` is empty.
// @param view Runtime string view copied by FrameGraph when collected.
// @param category ProfileCategory used by FrameGraph.
#define ENGINE_PROFILE_DYNAMIC(fallback, view, category)                                                     \
	ENGINE_HEAP_SCOPE(fallback);                                                                             \
	::engine::core::FrameGraph::CopiedScope ENGINE_PROFILE_CONCAT(engineProfileScope_, __LINE__) {           \
		fallback, view, category, ENGINE_PROFILE_OWNER                                                       \
	}

// Opens a stable-name FrameGraph scope until the enclosing C++ scope exits.
//
// FrameGraph does not copy `view`; its storage must remain valid until at
// least the next EndFrame(). `fallback` is unused without Tracy, and an empty
// `view` remains empty.
//
// @param fallback String literal reserved for the Tracy build.
// @param view Runtime string view backed by stable caller-owned storage.
// @param category ProfileCategory used by FrameGraph.
#define ENGINE_PROFILE_DYNAMIC_STABLE(fallback, view, category)                                              \
	ENGINE_HEAP_SCOPE(view);                                                                                 \
	::engine::core::FrameGraph::Scope ENGINE_PROFILE_CONCAT(engineProfileScope_, __LINE__) {                 \
		view, category, ENGINE_PROFILE_OWNER                                                                 \
	}

// Keeps heap attribution on producer threads when Tracy is not compiled in.
#define ENGINE_PROFILE_PRODUCER_DYNAMIC_STABLE(fallback, view) ENGINE_HEAP_SCOPE(view)

// Does nothing when Tracy support is not compiled in.
#define ENGINE_PROFILE_FRAME() ((void)0)

// Reports false when Tracy support is not compiled in.
#define ENGINE_PROFILE_ATTACHED() false

#endif

// Opens an engine-category profiling scope until the enclosing C++ scope exits.
//
// @param name Stable name used by the active profiling consumers.
#define ENGINE_PROFILE(name) ENGINE_PROFILE_CAT(name, ::engine::core::ProfileCategory::Engine)

// Reports whether anything is collecting - Tracy attached or FrameGraph enabled.
#define ENGINE_PROFILE_COLLECTING() (ENGINE_PROFILE_ATTACHED() || ::engine::core::FrameGraph::IsEnabled())
