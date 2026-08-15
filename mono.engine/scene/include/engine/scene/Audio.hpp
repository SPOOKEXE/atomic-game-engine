#pragma once

// What a world decides about how it is heard.
//
// **The audio twin of `Input.hpp`, and it exists for the identical reason.**
// `engine::audio` owns the mixer and sits at L12 `client`; the script binding is
// at L9 `shared` and may not name it. So `SoundService` cannot reach the module
// that makes the noise - what a script decides has to come to rest somewhere both
// can see, and that place is a resource on the world. `scene` is where a fact
// about the world lives; who acts on it is somebody else's business.
//
// **Per world rather than per mixer, and that is what makes the master gain
// unambiguous.** A client hosts several worlds and has exactly one output, so a
// number kept beside the output node would have N worlds writing it and the last
// writer of the frame winning. `client::SoundStage` folds this into each voice's
// own fader instead, so a world's setting scales that world's sounds and nothing
// else - which is also Roblox's arrangement, where `SoundService` is a service of
// one place rather than of the whole universe.
//
// **A server has one of these and nothing reads it, which is the point.** A world
// ticks the same scripts whoever hosts it, so a script that turns the music down
// runs headless and simply changes a number nobody is listening to.
//
// @tier L7 · shared

#include <engine/ecs/Entity.hpp>

#include <cstdint>

namespace engine::scene {

	// Where the ear is.
	//
	// **Two members and not Roblox's four**, and the omission is the same rule
	// `KeyCode` states about keys: every member here is one the mixer can
	// actually honour, and a name that mapped to nothing would offer an author
	// completion for a setting that does nothing. `ListenerType.CFrame` and
	// `ListenerType.ObjectCFrame` place the ear *and turn it*, and
	// `client::SoundStage` posts a position with no facing - `audio::ListenerPose`
	// has a forward and a right vector, and nothing sets either - so a listener
	// with a rotation would be a rotation the panning ignores.
	//
	// The ordinals are this file's own and are free to move; nothing serialises
	// one. `scene/Part.cpp` registers the member names in this order.
	//
	// @since v0.16
	enum class ListenerMode : uint8_t {
		// The composed camera's position, which is what a client already uses
		// and therefore what a world that has never been told means.
		Camera = 0,

		// `AudioState::Listener`'s `Transform`, for a game whose ear is on a
		// character rather than on the picture.
		ObjectPosition = 1,

		// Not a mode. The count.
		Count,
	};

	// What a world decides about its own audio.
	//
	// A resource: there is one ear and one master gain per world, and nothing
	// iterates them.
	//
	// Widest first so the object representation a snapshot writes holds no
	// uninitialised bytes between fields.
	//
	// @since v0.16
	struct AudioState {
		// The instance the ear sits on, under `ListenerMode::ObjectPosition`.
		//
		// **An instance and never a position**, for `scene::Sound`'s reason one
		// door along: a copy of where something is would be a second opinion
		// about it, and the one that goes stale the moment the thing moves.
		// Ignored under any other mode, and a mode naming an instance that has
		// no `Transform` falls back to the camera rather than to the origin -
		// see `client::SoundStage::Sync`.
		ecs::Entity Listener;

		// Linear gain over every sound in this world, 1 being as authored.
		//
		// **Linear rather than decibels, for `audio::Node::Gain`'s reason**: the
		// mixer multiplies, and a decibel is what a user interface shows.
		// Multiplied into each voice's fader rather than applied at the output,
		// so two worlds on one client do not fight over one number.
		//
		// Values above 1 are legal and clipped once at the output stage, which is
		// `audio/AGENTS.md`'s headroom rule reaching the property surface.
		float MasterVolume = 1.0f;

		// Where the ear is.
		ListenerMode Mode = ListenerMode::Camera;

		// Explicit padding, so the object representation a snapshot writes holds
		// no uninitialised bytes. `scene::InputState` learned this the expensive
		// way and its comment carries the argument.
		uint8_t Reserved[3] = {};
	};

	// The name a listener mode is known by.
	//
	// **Round-trips**, because these are the names `ecs::EnumTable` registers and
	// a script compares against - the same contract `Describe(KeyCode)` has.
	//
	// @param mode The mode.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ListenerMode mode);
}
