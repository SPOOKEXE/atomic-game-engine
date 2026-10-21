#pragma once

#include <engine/parallel/Channel.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace client {
	class Client;
	struct FrameObservation;

	// The diagnostic link occupies the process's sole inherited channel. Its
	// selections replace prior samples rather than retaining a frame archive.
	struct FrameObservationChannelRequest {
		uint64_t Run = 0;
		size_t MaxOwnedBytes = 8u * 1024u * 1024u;
		std::string RetainedPresenceField = "retained_world";
		std::string RetainedIntegerField = "delivered_textures";
		uint64_t RetainedIntegerValue = 2;
	};

	struct FrameObservationChannelSummary {
		uint64_t Run = 0;
		uint64_t Generation = 0;
		uint64_t CapturedFrames = 0;
		int RunResult = 0;
		bool Complete = false;
		std::string Failure;
		// Wire records contain native metadata and final RGBA/provenance. Object
		// rows are deliberately outside this channel's declared observation set.
		std::shared_ptr<const FrameObservation> Latest;
		std::shared_ptr<const FrameObservation> Retained;
	};

	inline constexpr size_t FRAME_OBSERVATION_CHANNEL_MAXIMUM_WIRE = 1024u * 1024u;
	inline constexpr size_t FRAME_OBSERVATION_CHANNEL_MAXIMUM_METADATA = 128u * 1024u;
	inline constexpr size_t FRAME_OBSERVATION_CHANNEL_MAXIMUM_PIXELS = 64u * 1024u;

	engine::parallel::ChannelSettings FrameObservationChannelSettings();
	std::vector<std::byte> EncodeFrameObservationChannelRequest(const FrameObservationChannelRequest &);
	bool DecodeFrameObservationChannelRequest(
		std::span<const std::byte>, uint64_t run, FrameObservationChannelRequest &, std::string &failure
	);
	bool RetainFrameObservationChannel(
		FrameObservationChannelSummary &,
		const FrameObservationChannelRequest &,
		std::shared_ptr<const FrameObservation>,
		std::string &failure
	);
	std::vector<std::byte> EncodeFrameObservationChannelSummary(const FrameObservationChannelSummary &);
	bool DecodeFrameObservationChannelSummary(
		std::span<const std::byte>,
		const FrameObservationChannelRequest &,
		FrameObservationChannelSummary &,
		std::string &failure
	);
	std::vector<std::byte>
	EncodeFrameObservationChannelAcknowledgement(uint64_t run, uint64_t generation, bool accepted);
	bool FrameObservationChannelAcknowledged(std::span<const std::byte>, uint64_t run, uint64_t generation);

	// Installed before Initialise. Finish observes the ordinary Run result and
	// exchanges the final report/ack after the engine loop has ended.
	class FrameObservationChannel {
	  public:
		FrameObservationChannel(Client &, std::unique_ptr<engine::parallel::Channel>, uint64_t run);
		~FrameObservationChannel();
		FrameObservationChannel(const FrameObservationChannel &) = delete;
		FrameObservationChannel &operator=(const FrameObservationChannel &) = delete;
		bool Ready() const;
		std::string Failure() const;
		bool Finish(int runResult);

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
