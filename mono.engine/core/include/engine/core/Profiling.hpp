#pragma once

// The engine profiler. One macro feeds both consumers:
//
//   - Tracy, for the real thing — a second process, every thread, full history.
//   - FrameGraph, for the in-game F5 overlay, which has to work with nothing
//     attached and on a machine that is not the developer's.
//
// The planned userland profiler is a different thing entirely. It belongs at
// L13 and shares no code with this engine profiler.
//
// @tier L0 · shared

#include <engine/core/FrameGraph.hpp>

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

#if defined(ENGINE_TRACY)

// Opens a Tracy zone and FrameGraph scope until the enclosing C++ scope exits.
//
// `name` must be a string literal: Tracy keeps the pointer for the life of
// the process, and FrameGraph reads it after the frame has ended.
//
// **`ZoneNamedN` rather than `ZoneScopedN`, so two of these may share a C++
// scope.** `ZoneScopedN` declares a variable called `___tracy_scoped_zone`,
// which is one fixed name — so a second macro beside the first was a
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
	::engine::core::FrameGraph::Scope ENGINE_PROFILE_CONCAT(engineProfileScope_, __LINE__) {                 \
		name, category                                                                                       \
	}

// Opens a dynamically named Tracy zone and copied-name FrameGraph scope.
//
// Use for a span whose name is not known at compile time — a script chunk, a
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
	::engine::core::FrameGraph::CopiedScope ENGINE_PROFILE_CONCAT(engineProfileScope_, __LINE__) {           \
		fallback, view, category                                                                             \
	}

// Opens a dynamically named Tracy zone and stable-name FrameGraph scope.
//
// The FrameGraph name is not copied. `view` MUST remain valid until at least
// the next EndFrame(): the overlay reads
// the published spans after the frame that produced them has ended, so a
// name built into a local would dangle by the time it is drawn.
//
// This is for a caller that owns the string for the life of the run — the
// scheduler, which owns its system names — and it is the only reason the
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
	::engine::core::FrameGraph::Scope ENGINE_PROFILE_CONCAT(engineProfileScope_, __LINE__) {                 \
		view, category                                                                                       \
	}

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
	::engine::core::FrameGraph::Scope ENGINE_PROFILE_CONCAT(engineProfileScope_, __LINE__) {                 \
		name, category                                                                                       \
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
	::engine::core::FrameGraph::CopiedScope ENGINE_PROFILE_CONCAT(engineProfileScope_, __LINE__) {           \
		fallback, view, category                                                                             \
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
	::engine::core::FrameGraph::Scope ENGINE_PROFILE_CONCAT(engineProfileScope_, __LINE__) {                 \
		view, category                                                                                       \
	}

// Does nothing when Tracy support is not compiled in.
#define ENGINE_PROFILE_FRAME() ((void)0)

// Reports false when Tracy support is not compiled in.
#define ENGINE_PROFILE_ATTACHED() false

#endif

// Opens an engine-category profiling scope until the enclosing C++ scope exits.
//
// @param name Stable name used by the active profiling consumers.
#define ENGINE_PROFILE(name) ENGINE_PROFILE_CAT(name, ::engine::core::ProfileCategory::Engine)

// Reports whether anything is collecting — Tracy attached or FrameGraph enabled.
#define ENGINE_PROFILE_COLLECTING() (ENGINE_PROFILE_ATTACHED() || ::engine::core::FrameGraph::IsEnabled())
