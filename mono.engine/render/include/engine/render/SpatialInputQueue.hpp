#pragma once

// Bounded input awaiting the depth and GUI facts of its displayed frame.
#include <engine/gui/Input.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Components.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace engine::render {

	// An invocation-scoped nonblocking depth source. The queue never retains it.
	class SpatialInputDepth {
	  public:
		virtual ~SpatialInputDepth() = default;
		// Return the current state of a retained depth frame.
		virtual SpatialDepthFrameStatus Status(SpatialDepthFrameKey frame) = 0;
		// Return frame geometry when available.
		virtual std::optional<SpatialDepthFrameInfo> Info(SpatialDepthFrameKey frame) = 0;
		// Request a nonblocking sample at a depth pixel.
		virtual uint64_t Queue(SpatialDepthFrameKey frame, uint32_t x, uint32_t y) = 0;
		// Retrieve the result of a queued sample.
		virtual SpatialDepthReadbackResult Take(SpatialDepthFrameKey frame, uint64_t token) = 0;
		// Cancel a queued sample, returning false if it must be retried.
		virtual bool Cancel(SpatialDepthFrameKey frame, uint64_t token) = 0;
	};

	// One retained renderer-wide queue. Call from the renderer's owner thread.
	class SpatialInputQueue {
	  public:
		// Maximum retained frame snapshots.
		static constexpr size_t MAX_FRAMES = 8;
		// Maximum queued input events.
		static constexpr size_t MAX_EVENTS = 8192;
		// Maximum sealed batches.
		static constexpr size_t MAX_BATCHES = 128;
		// Aggregate retained snapshot and payload budget.
		static constexpr size_t MAX_RETAINED_BYTES = 64 * 1024 * 1024;
		// Maximum copied payload size per event.
		static constexpr size_t MAX_PAYLOAD_BYTES = 1024 * 1024;
		// Maximum copied metadata size per frame.
		static constexpr size_t MAX_FRAME_METADATA_BYTES = 1024 * 1024;
		// Maximum time an event may wait in the queue.
		static constexpr auto DEADLINE = std::chrono::seconds(2);

		// Poll states; BatchEnd is an acknowledgement boundary before callbacks.
		enum class Status : uint8_t { Empty, Pending, Ready, Cancelled, BatchEnd };
		// Reason input was cancelled.
		enum class Failure : uint8_t {
			None,
			Capacity,
			StaleFrame,
			DepthUnavailable,
			Timeout,
			InvalidSample,
			ExternalCancel
		};
		// Type of queued input.
		enum class Kind : uint8_t { Pointer, Opaque };
		// Why an admitted opaque event was discarded without delivery.
		enum class DiscardReason : uint8_t {
			Cancellation,
			Revoked,
			HostRetired,
			Reset,
			Borrowed,
			Destruction
		};
		// The payload is borrowed for this call. The observer must not reenter the
		// queue, renderer or host, and must not retain the payload.
		using OpaqueDiscardObserver = void (*)(
			uint64_t hostToken, uint64_t sequence, std::span<const std::byte> payload, DiscardReason reason
		);

		// Camera values used when the GUI's spatial geometry was resolved.
		struct ResolverView {
			// Lens used for spatial GUI resolution.
			scene::Camera Camera;
			// Camera pose used for spatial GUI resolution.
			core::CFrame Frame;
		};

		// Borrowed frame and payload stay valid until the matching acknowledgement.
		struct Result {
			// Poll outcome.
			Status State = Status::Empty;
			// Cancellation reason, when applicable.
			Failure Error = Failure::None;
			// Event type.
			Kind Type = Kind::Pointer;
			// Identifier to acknowledge this result.
			uint64_t Event = 0;
			// Incarnation of the host that owns the event.
			uint64_t HostToken = 0;
			// Displayed snapshot sequence.
			uint64_t Sequence = 0;
			// Store incarnation associated with the batch.
			uint64_t Store = 0;
			// Sealed batch identifier.
			uint64_t Batch = 0;
			// Whether the pointer selects authored GUI.
			bool Authoring = false;
			// Pointer mapped into the selected collector.
			gui::Pointer Pointer;
			// Logical screen extent used by the displayed frame.
			gui::Screen Screen;
			// Depth frame associated with the displayed image.
			SpatialDepthFrameKey Depth;
			// Raster geometry, when required and available.
			std::optional<SpatialDepthFrameInfo> FrameInfo;
			// GUI resolution camera and pose, when captured.
			std::optional<ResolverView> Resolver;
			// Borrowed displayed GUI facts.
			const gui::PointerFrame *Frame = nullptr;
			// Borrowed event payload.
			std::span<const std::byte> Payload;
			// Borrowed metadata copied with the displayed frame.
			std::span<const std::byte> FrameMetadata;
		};

		SpatialInputQueue() = default;
		~SpatialInputQueue();
		SpatialInputQueue(const SpatialInputQueue &) = delete;
		SpatialInputQueue &operator=(const SpatialInputQueue &) = delete;

		// Stage facts and copied metadata for the exact view. Sequences increase monotonically.
		bool Stage(
			uint64_t sequence,
			gui::PointerFrame &&frame,
			uint64_t viewport,
			uint64_t hostToken,
			gui::Screen screen,
			std::optional<ResolverView> resolver = {},
			std::span<const std::byte> metadata = {}
		);
		// Publish only after the host selects the corresponding retained color image
		// for display.
		bool Publish(const FrameResult &result, uint64_t viewport);
		// Metadata remains valid until the next queue mutation; only published frames are visible.
		std::optional<std::span<const std::byte>> PublishedMetadata(uint64_t sequence) const;
		// The published frame is borrowed until the next queue mutation.
		const gui::PointerFrame *PublishedFrame(uint64_t sequence) const;
		// Discard an unsubmitted or unused staged frame.
		bool DiscardStage(uint64_t sequence);
		// Queue one event in host order. Authoring picks carry the original contact generation.
		// Opaque payload is copied, never borrowed.
		bool EnqueuePointer(
			uint64_t sequence,
			gui::Screen screen,
			gui::Pointer pointer,
			uint64_t hostToken,
			bool authoring = false,
			uint64_t *contactGeneration = nullptr,
			uint64_t associatedGeneration = 0
		);
		// requiresFrameInfo waits for raster metadata needed by a world-space pick.
		// Pass the associated pointer generation to revoke a gesture-dependent record.
		// An observer must remain callable until this event is delivered or discarded,
		// including when the queue is destroyed with work still pending.
		bool EnqueueOpaque(
			uint64_t sequence,
			uint64_t hostToken,
			std::span<const std::byte> payload,
			bool requiresFrameInfo = false,
			uint64_t contactGeneration = 0,
			OpaqueDiscardObserver discardObserver = nullptr
		);
		// Seal the contiguous unsealed prefix for one host and Store.
		bool SealBatch(uint64_t hostToken, uint64_t storeIdentity);
		// Acquire immutable prerequisites without blocking or mutating a Store.
		// Retry Pending; readiness does not guarantee a required sample is valid.
		Status Preflight(Renderer &renderer);
		// Preflight using an invocation-scoped depth source.
		Status Preflight(SpatialInputDepth &depth);
		// The head token identifies the host router that owns the next event.
		std::optional<uint64_t> HeadHostToken() const;
		// Request ordered cancellation on the next Poll; fails while a result is
		// borrowed.
		bool RequestCancel();
		// Resolve only the FIFO head; capturedCollector comes from the live router.
		Result Poll(Renderer &renderer, ecs::Entity capturedCollector = {}, bool capturedSpatial = false);
		// Resolve the FIFO head using an invocation-scoped depth source.
		Result
		Poll(SpatialInputDepth &depth, ecs::Entity capturedCollector = {}, bool capturedSpatial = false);
		// Acknowledge the returned event ID, including BatchEnd, before callbacks.
		// Hosts must prevent reentrant pumping and defer teardown during callbacks.
		bool Acknowledge(uint64_t event);
		// Discard only the already borrowed result during host teardown.
		bool DiscardBorrowed();
		// After the host has cancelled or forgotten its routers, release all queued
		// work before renderer shutdown or viewport retirement. A failed reset is
		// retryable.
		bool Reset(Renderer &renderer);
		// Release queued work using an invocation-scoped depth source; retry on failure.
		bool Reset(SpatialInputDepth &depth);
		// Retire one host after its router is cancelled or forgotten; preserves other
		// hosts.
		bool RetireHost(Renderer &renderer, uint64_t hostToken, uint64_t viewport);
		// Retire one host using an invocation-scoped depth source.
		bool RetireHost(SpatialInputDepth &depth, uint64_t hostToken, uint64_t viewport);
		// One physical primary pointer is shared by this queue; after cancellation it
		// cannot start again until its release.
		bool SuppressingContact() const {
			return SuppressContact;
		}
		// When entering queued routing, suppress a contact already physically held.
		bool BeginPhysicalTracking(bool down);
		// Report the actual physical primary level, independently of translated events.
		void ObservePhysicalPrimary(bool down);
		// Whether a physical contact or its ordered release is still owned.
		bool HasContact() const {
			return ContactActive || SuppressContact || ReleasePending;
		}
		// Includes pending cancellation and physical-release suppression.
		bool HasPending() const {
			return !Events.empty() || !Batches.empty() || LastReady || DeferredFailure != Failure::None ||
				   ReleasePending || SuppressContact;
		}

	  private:
		struct Frame {
			uint64_t Sequence = 0, Viewport = 0, HostToken = 0;
			gui::PointerFrame Gui;
			gui::Screen Screen;
			std::optional<ResolverView> Resolver;
			std::vector<std::byte> Metadata;
			size_t RetainedBytes() const {
				return sizeof(Frame) + Gui.RetainedBytes() + Metadata.capacity() +
					   (Metadata.capacity() ? 64 : 0);
			}
			SpatialDepthFrameKey Depth;
			std::optional<SpatialDepthFrameInfo> Projection;
			bool Published = false, Displayed = false;
		};
		struct Event {
			Kind Type = Kind::Pointer;
			bool Authoring = false, RequiresFrameInfo = false, Prefetched = false, Unavailable = false;
			uint64_t PrefetchToken = 0;
			float SampleDepth = 0.0f;
			std::optional<SpatialDepthFrameInfo> Info;
			uint64_t Id = 0, Sequence = 0, HostToken = 0, ContactGeneration = 0;
			gui::Screen Screen;
			gui::Pointer Pointer;
			std::vector<std::byte> Payload;
			OpaqueDiscardObserver DiscardObserver = nullptr;
			std::chrono::steady_clock::time_point Deadline;
		};
		static void NotifyDiscard(const Event &event, DiscardReason reason) noexcept;
		struct Batch {
			uint64_t Id = 0, Host = 0, Store = 0, LastEvent = 0;
			bool Prepared = false, End = false;
		};
		std::deque<Batch> Batches;
		uint64_t NextBatch = 1, SealedThrough = 0;
		std::deque<Frame> Frames;
		std::deque<Event> Events;
		size_t FrameBytes = 0, PayloadBytes = 0;
		uint64_t NextEvent = 1, LastSequence = 0;
		uint64_t EnqueuedGeneration = 0, ActiveGeneration = 0, RevokedGeneration = 0;
		bool EnqueuedDown = false;
		bool SuppressContact = false, PhysicalDown = false, ContactActive = false, ReleasePending = false;
		Failure DeferredFailure = Failure::None;
		uint64_t LastReady = 0, ContactOwner = 0;
		Result ReadyCache;
		Frame *Find(uint64_t sequence);
		const Frame *Find(uint64_t sequence) const;
		bool Admit(Event &&event);
		Result Fail(SpatialInputDepth &depth, Failure reason);
	};
} // namespace engine::render
