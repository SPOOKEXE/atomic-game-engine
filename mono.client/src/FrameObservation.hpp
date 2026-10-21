#pragma once

#include <engine/render/Renderer.hpp>

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace client {
	class Client;
	struct FrameObservationState;

	struct FrameObservationOptions {
		size_t MaxPendingFrames = 4;
		// Retained observation charge, including pending metadata and worker-held copies.
		// JSON/object construction and renderer transfer staging are producer transients.
		size_t MaxOwnedBytes = 128u * 1024u * 1024u;
		std::vector<uint64_t> CaptureFrames;
	};

	struct FrameObjectObservation {
		std::string World;
		std::string Object;
		std::string Mesh;
		std::string Shape;
		engine::core::CFrame Frame;
		engine::core::Vector3 HalfExtent;
		engine::core::Color3 Tint;
		uint64_t Rig = 0;
		uint64_t Source = 0;
		uint64_t Variant = 0;
	};

	struct FrameObservation {
		uint64_t Generation = 0;
		uint64_t ClientFrame = 0;
		size_t OwnedBytes = 0;
		nlohmann::json Metadata;
		std::vector<FrameObjectObservation> Objects;
		engine::render::SceneCaptureResult Image;
		std::string Pipeline;
	};

	using FrameObservationCallback = std::function<void(std::shared_ptr<const FrameObservation>)>;

	// The lease owns the subscription. Delivered immutable copies may outlive it.
	class FrameObservationLease {
	  public:
		FrameObservationLease() = default;
		~FrameObservationLease();
		FrameObservationLease(FrameObservationLease &&) noexcept;
		FrameObservationLease &operator=(FrameObservationLease &&) noexcept;
		FrameObservationLease(const FrameObservationLease &) = delete;
		FrameObservationLease &operator=(const FrameObservationLease &) = delete;
		explicit operator bool() const;
		std::string Failure() const;
		FrameObservationOptions Options() const;
		uint64_t Generation() const;
		uint64_t CapturedFrames() const;
		size_t OwnedBytes() const;
		// Ends capture and validates every selected frame before releasing the callback.
		// Failure remains readable after Finish or Reset, including after client shutdown.
		bool Finish();
		void Reset();

	  private:
		friend FrameObservationLease
		SubscribeClientFrames(Client &, FrameObservationOptions, FrameObservationCallback);
		std::shared_ptr<FrameObservationState> State;
	};

	FrameObservationLease SubscribeClientFrames(Client &, FrameObservationOptions, FrameObservationCallback);
	uint64_t ClientFrameObservationGeneration(const Client &);
	bool HasClientFrameSubscription(const Client &);
	void FailClientFrameObservation(const Client &, std::string);
	bool ShouldObserveClientFrame(const Client &, uint64_t);
	bool ObservationEnabled(const Client &, const std::filesystem::path &captureSequence);
	bool BeginClientFrameObservation(Client &);
	void CloseClientFrameObservation(Client &, engine::render::Renderer &);
	void PrepareClientFrameObservation(
		Client &,
		engine::render::Renderer &,
		const engine::render::View &,
		uint64_t,
		nlohmann::json &&,
		std::vector<FrameObjectObservation> &&
	);
	void CompleteClientFrameObservation(Client &, engine::render::Renderer &, bool submitted);
}
