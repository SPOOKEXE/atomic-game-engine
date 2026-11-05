#include <engine/render/SpatialCanvas.hpp>
#include <engine/render/SpatialInputQueue.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <new>

namespace engine::render {
	void SpatialInputQueue::NotifyDiscard(const Event &event, DiscardReason reason) noexcept {
		if (event.Type != Kind::Opaque || event.DiscardObserver == nullptr) return;
		try {
			event.DiscardObserver(event.HostToken, event.Sequence, event.Payload, reason);
		} catch (...) {
			std::fputs("spatial input discard observer failed\n", stderr);
		}
	}

	SpatialInputQueue::~SpatialInputQueue() {
		for (const auto &event : Events)
			NotifyDiscard(event, DiscardReason::Destruction);
	}

	namespace {
		SpatialInputQueue::Result
		Pending(SpatialInputQueue::Failure reason = SpatialInputQueue::Failure::None) {
			SpatialInputQueue::Result result;
			result.State = SpatialInputQueue::Status::Pending;
			result.Error = reason;
			return result;
		}
		bool HasSpatial(const gui::PointerFrame &frame, bool authoring = false) {
			for (size_t index = 0; index < frame.SpatialCanvasCount(); ++index) {
				ecs::Entity entity;
				gui::SpatialCanvas canvas;
				if (!frame.SpatialCanvasAt(index, entity, canvas)) return true;
				if (canvas.Visible && (authoring || canvas.Interactive)) return true;
			}
			return false;
		}
		class RendererDepth final : public SpatialInputDepth {
		  public:
			explicit RendererDepth(Renderer &renderer) : Device(renderer) {}
			SpatialDepthFrameStatus Status(SpatialDepthFrameKey key) override {
				return Device.SpatialDepthStatus(key);
			}
			std::optional<SpatialDepthFrameInfo> Info(SpatialDepthFrameKey key) override {
				return Device.SpatialDepthInfo(key);
			}
			uint64_t Queue(SpatialDepthFrameKey key, uint32_t x, uint32_t y) override {
				return Device.QueueSpatialDepth(key, x, y);
			}
			SpatialDepthReadbackResult Take(SpatialDepthFrameKey key, uint64_t token) override {
				return Device.TakeSpatialDepth(key, token);
			}
			bool Cancel(SpatialDepthFrameKey key, uint64_t token) override {
				return Device.CancelSpatialDepth(key, token);
			}

		  private:
			Renderer &Device;
		};
	} // namespace

	SpatialInputQueue::Frame *SpatialInputQueue::Find(uint64_t sequence) {
		const auto found = std::find_if(Frames.begin(), Frames.end(), [sequence](const auto &frame) {
			return frame.Sequence == sequence;
		});
		return found == Frames.end() ? nullptr : &*found;
	}
	const SpatialInputQueue::Frame *SpatialInputQueue::Find(uint64_t sequence) const {
		const auto found = std::find_if(Frames.begin(), Frames.end(), [sequence](const auto &frame) {
			return frame.Sequence == sequence;
		});
		return found == Frames.end() ? nullptr : &*found;
	}

	bool SpatialInputQueue::Stage(
		uint64_t sequence,
		gui::PointerFrame &&frame,
		uint64_t viewport,
		uint64_t hostToken,
		gui::Screen screen,
		std::optional<ResolverView> resolver,
		std::span<const std::byte> metadata
	) {
		if (LastReady || DeferredFailure != Failure::None || !frame.Valid() || !sequence ||
			sequence <= LastSequence)
			return false;
		if (metadata.size() > MAX_FRAME_METADATA_BYTES) {
			DeferredFailure = Failure::Capacity;
			return false;
		}
		std::vector<std::byte> owned;
		try {
			owned.assign(metadata.begin(), metadata.end());
		} catch (const std::bad_alloc &) {
			DeferredFailure = Failure::Capacity;
			return false;
		}
		const size_t bytes =
			sizeof(Frame) + frame.RetainedBytes() + owned.capacity() + (owned.capacity() ? 64 : 0);
		if (bytes > MAX_RETAINED_BYTES || PayloadBytes > MAX_RETAINED_BYTES - bytes) {
			DeferredFailure = Failure::Capacity;
			return false;
		}
		while (Frames.size() >= MAX_FRAMES || FrameBytes > MAX_RETAINED_BYTES - PayloadBytes - bytes) {
			const auto unused = std::find_if(Frames.begin(), Frames.end(), [&](const Frame &entry) {
				return entry.Published && !entry.Displayed &&
					   std::none_of(Events.begin(), Events.end(), [&](const Event &event) {
						   return event.Sequence == entry.Sequence;
					   });
			});
			if (unused == Frames.end()) {
				DeferredFailure = Failure::Capacity;
				return false;
			}
			FrameBytes -= unused->RetainedBytes();
			Frames.erase(unused);
		}
		try {
			Frames.push_back(
				{sequence,
				 viewport,
				 hostToken,
				 std::move(frame),
				 screen,
				 std::move(resolver),
				 std::move(owned),
				 {},
				 {},
				 false,
				 false}
			);
		} catch (const std::bad_alloc &) {
			DeferredFailure = Failure::Capacity;
			return false;
		}
		FrameBytes += bytes;
		LastSequence = sequence;
		return true;
	}

	bool SpatialInputQueue::Publish(const FrameResult &result, uint64_t viewport) {
		if (LastReady || DeferredFailure != Failure::None ||
			result.SpatialSnapshot != SpatialSnapshotStatus::Ready || !result.SpatialSnapshotSequence)
			return false;
		auto *entry = Find(result.SpatialSnapshotSequence);
		if (!entry || entry->Viewport != viewport) return false;
		if (entry->Published && (entry->Viewport != viewport || entry->Depth != result.SpatialDepth))
			return false;
		if (result.SpatialDepthProjection &&
			(!result.SpatialDepth.Frame || result.SpatialDepthProjection->Key != result.SpatialDepth ||
			 result.SpatialDepthProjection->SnapshotSequence != result.SpatialSnapshotSequence))
			return false;
		if (result.SpatialDepth.Frame) {
			if (result.SpatialDepth.World != entry->Gui.StoreIdentity() ||
				result.SpatialDepth.Viewport != viewport)
				return false;
		} else if (HasSpatial(entry->Gui))
			return false;
		for (auto &frame : Frames)
			if (frame.Displayed && frame.Viewport == viewport) frame.Displayed = false;
		entry->Depth = result.SpatialDepth;
		// A sequence is immutable once published, even if the host presents it again.
		if (!entry->Published) entry->Projection = result.SpatialDepthProjection;
		entry->Viewport = viewport;
		entry->Published = entry->Displayed = true;
		return true;
	}

	std::optional<std::span<const std::byte>> SpatialInputQueue::PublishedMetadata(uint64_t sequence) const {
		const auto *frame = Find(sequence);
		if (!frame || !frame->Published) return {};
		return std::span<const std::byte>(frame->Metadata);
	}

	const gui::PointerFrame *SpatialInputQueue::PublishedFrame(uint64_t sequence) const {
		const auto *frame = Find(sequence);
		return frame && frame->Published ? &frame->Gui : nullptr;
	}

	bool SpatialInputQueue::DiscardStage(uint64_t sequence) {
		if (LastReady) return false;
		const auto found = std::find_if(Frames.begin(), Frames.end(), [sequence](const auto &frame) {
			return frame.Sequence == sequence;
		});
		if (found == Frames.end() || found->Published) return false;
		FrameBytes -= found->RetainedBytes();
		Frames.erase(found);
		return true;
	}

	bool SpatialInputQueue::Admit(Event &&event) {
		if (LastReady || DeferredFailure != Failure::None) return false;
		const auto *frame = Find(event.Sequence);
		if (!frame || !frame->Published || frame->HostToken != event.HostToken) {
			DeferredFailure = Failure::StaleFrame;
			return false;
		}
		if (Events.size() >= MAX_EVENTS || event.Payload.size() > MAX_PAYLOAD_BYTES - PayloadBytes ||
			event.Payload.size() > MAX_RETAINED_BYTES - FrameBytes - PayloadBytes ||
			NextEvent >= std::numeric_limits<uint64_t>::max() - 2) {
			DeferredFailure = Failure::Capacity;
			return false;
		}
		event.Id = NextEvent;
		event.Deadline = std::chrono::steady_clock::now() + DEADLINE;
		const size_t bytes = event.Payload.size();
		try {
			Events.push_back(std::move(event));
		} catch (const std::bad_alloc &) {
			DeferredFailure = Failure::Capacity;
			return false;
		}
		++NextEvent;
		PayloadBytes += bytes;
		return true;
	}

	bool SpatialInputQueue::BeginPhysicalTracking(bool down) {
		if (LastReady || !Events.empty() || !Batches.empty() || ContactActive || ReleasePending) return false;
		PhysicalDown = down;
		SuppressContact = down;
		return true;
	}
	void SpatialInputQueue::ObservePhysicalPrimary(bool down) {
		PhysicalDown = down;
		if (SuppressContact && !down) {
			if (ContactActive)
				ReleasePending = true;
			else if (!ReleasePending) {
				SuppressContact = false;
				EnqueuedDown = false;
			}
		}
	}
	bool SpatialInputQueue::EnqueuePointer(
		uint64_t sequence,
		gui::Screen screen,
		gui::Pointer pointer,
		uint64_t hostToken,
		bool authoring,
		uint64_t *contactGeneration,
		uint64_t associatedGeneration
	) {
		if (contactGeneration) *contactGeneration = 0;
		if (SuppressContact) return true;
		const auto *frame = Find(sequence);
		if (!frame || frame->Screen.Width != screen.Width || frame->Screen.Height != screen.Height) {
			DeferredFailure = Failure::StaleFrame;
			return false;
		}
		if ((authoring || pointer.Cancelled) && associatedGeneration > EnqueuedGeneration) return false;
		if (!authoring && !pointer.Cancelled && pointer.Down && !EnqueuedDown) {
			if (EnqueuedGeneration == std::numeric_limits<uint64_t>::max()) {
				DeferredFailure = Failure::Capacity;
				return false;
			}
			++EnqueuedGeneration;
		}
		Event event;
		event.ContactGeneration =
			authoring
				? associatedGeneration
				: (pointer.Cancelled ? (associatedGeneration ? associatedGeneration : EnqueuedGeneration)
									 : ((pointer.Down || EnqueuedDown) ? EnqueuedGeneration : 0));
		const uint64_t generation = event.ContactGeneration;
		if (!authoring && !pointer.Cancelled) EnqueuedDown = pointer.Down;
		event.Type = Kind::Pointer;
		event.Authoring = authoring;
		event.Sequence = sequence;
		event.HostToken = hostToken;
		event.Screen = screen;
		event.Pointer = pointer;
		if (!Admit(std::move(event))) return false;
		if (contactGeneration) *contactGeneration = generation;
		return true;
	}
	bool SpatialInputQueue::EnqueueOpaque(
		uint64_t sequence,
		uint64_t hostToken,
		std::span<const std::byte> payload,
		bool requiresFrameInfo,
		uint64_t contactGeneration,
		OpaqueDiscardObserver discardObserver
	) {
		if (payload.size() > MAX_PAYLOAD_BYTES) {
			DeferredFailure = Failure::Capacity;
			return false;
		}
		Event event;
		event.Type = Kind::Opaque;
		event.ContactGeneration = contactGeneration;
		event.RequiresFrameInfo = requiresFrameInfo;
		event.Sequence = sequence;
		event.HostToken = hostToken;
		event.DiscardObserver = discardObserver;
		try {
			event.Payload.assign(payload.begin(), payload.end());
		} catch (const std::bad_alloc &) {
			DeferredFailure = Failure::Capacity;
			return false;
		}
		return Admit(std::move(event));
	}

	bool SpatialInputQueue::SealBatch(uint64_t hostToken, uint64_t storeIdentity) {
		if (LastReady || DeferredFailure != Failure::None || Batches.size() >= MAX_BATCHES ||
			NextBatch == std::numeric_limits<uint64_t>::max())
			return false;
		uint64_t last = 0;
		for (const auto &event : Events) {
			if (event.Id <= SealedThrough) continue;
			const auto *frame = Find(event.Sequence);
			if (event.HostToken != hostToken || !frame || frame->HostToken != hostToken ||
				frame->Gui.StoreIdentity() != storeIdentity) {
				if (!last) return false;
				break;
			}
			last = event.Id;
		}
		if (!last) return false;
		try {
			Batches.push_back({NextBatch, hostToken, storeIdentity, last, false, false});
		} catch (const std::bad_alloc &) {
			return false;
		}
		++NextBatch;
		SealedThrough = last;
		return true;
	}
	SpatialInputQueue::Status SpatialInputQueue::Preflight(Renderer &renderer) {
		RendererDepth depth(renderer);
		return Preflight(depth);
	}
	SpatialInputQueue::Status SpatialInputQueue::Preflight(SpatialInputDepth &depth) {
		if (LastReady) return Status::Pending;
		if (DeferredFailure != Failure::None || ReleasePending) return Status::Ready;
		if (Batches.empty()) return Status::Empty;
		auto &batch = Batches.front();
		if (batch.End || batch.Prepared) return Status::Ready;
		size_t inflight = 0;
		for (const auto &event : Events)
			if (event.PrefetchToken) ++inflight;
		bool pending = false;
		for (auto &event : Events) {
			if (event.Id > batch.LastEvent) break;
			if (event.Prefetched) continue;
			const auto *frame = Find(event.Sequence);
			const bool expired = std::chrono::steady_clock::now() >= event.Deadline;
			if (expired || !frame || !frame->Published || frame->HostToken != batch.Host ||
				frame->Gui.StoreIdentity() != batch.Store) {
				if (event.PrefetchToken &&
					!depth.Cancel(frame ? frame->Depth : SpatialDepthFrameKey{}, event.PrefetchToken)) {
					pending = true;
					continue;
				}
				if (event.PrefetchToken) --inflight;
				event.PrefetchToken = 0;
				event.Unavailable = event.Prefetched = true;
				continue;
			}
			if (event.Type == Kind::Opaque && !event.RequiresFrameInfo) {
				event.Prefetched = true;
				continue;
			}
			if (!frame->Depth.Frame) {
				event.Unavailable = event.RequiresFrameInfo;
				event.Prefetched = true;
				continue;
			}
			if (!event.Info && frame->Projection) event.Info = frame->Projection;
			if (!event.Info) {
				const auto status = depth.Status(frame->Depth);
				if (status == SpatialDepthFrameStatus::Pending) {
					pending = true;
					continue;
				}
				if (status != SpatialDepthFrameStatus::Ready) {
					event.Unavailable = event.Prefetched = true;
					continue;
				}
				auto info = depth.Info(frame->Depth);
				if (!info || info->Key != frame->Depth || info->SnapshotSequence != frame->Sequence) {
					event.Unavailable = event.Prefetched = true;
					continue;
				}
				event.Info = std::move(info);
			}
			if (event.Type == Kind::Opaque || !event.Pointer.Inside || event.Pointer.Cancelled ||
				!HasSpatial(frame->Gui, event.Authoring)) {
				event.Prefetched = true;
				continue;
			}
			if (!event.PrefetchToken) {
				const auto sampleStatus = depth.Status(frame->Depth);
				if (sampleStatus == SpatialDepthFrameStatus::Pending) {
					pending = true;
					continue;
				}
				if (sampleStatus != SpatialDepthFrameStatus::Ready) {
					event.Unavailable = event.Prefetched = true;
					continue;
				}
				uint32_t x = 0, y = 0;
				if (!SpatialDepthPixel(event.Screen, event.Pointer, *event.Info, x, y)) {
					event.Unavailable = event.Prefetched = true;
					continue;
				}
				if (inflight >= 8) {
					pending = true;
					continue;
				}
				event.PrefetchToken = depth.Queue(frame->Depth, x, y);
				if (!event.PrefetchToken) {
					pending = true;
					continue;
				}
				++inflight;
			}
			const auto sample = depth.Take(frame->Depth, event.PrefetchToken);
			if (sample.Status == SpatialDepthReadbackStatus::Pending) {
				pending = true;
				continue;
			}
			event.PrefetchToken = 0;
			--inflight;
			if (sample.Status != SpatialDepthReadbackStatus::Ready || sample.Frame != frame->Depth ||
				(!std::isfinite(sample.Depth) || sample.Depth < 0.0f || sample.Depth > 1.0f))
				event.Unavailable = true;
			else
				event.SampleDepth = sample.Depth;
			event.Prefetched = true;
		}
		if (pending) return Status::Pending;
		batch.Prepared = true;
		return Status::Ready;
	}

	std::optional<uint64_t> SpatialInputQueue::HeadHostToken() const {
		if (LastReady) return ReadyCache.HostToken;
		if (DeferredFailure != Failure::None)
			return ContactActive ? ContactOwner : (Events.empty() ? 0 : Events.front().HostToken);
		if (!Batches.empty() && Batches.front().End) return Batches.front().Host;
		if (ReleasePending) return ContactOwner;
		if (Events.empty()) return {};
		return Events.front().HostToken;
	}

	bool SpatialInputQueue::RequestCancel() {
		if (LastReady) return false;
		DeferredFailure = Failure::ExternalCancel;
		return true;
	}

	SpatialInputQueue::Result SpatialInputQueue::Fail(SpatialInputDepth &depth, Failure reason) {
		DeferredFailure = reason;
		const uint64_t last =
			Batches.empty() ? std::numeric_limits<uint64_t>::max() : Batches.front().LastEvent;
		for (auto &event : Events) {
			if (event.Id > last) break;
			if (!event.PrefetchToken) continue;
			const auto *frame = Find(event.Sequence);
			if (!frame || !depth.Cancel(frame->Depth, event.PrefetchToken)) return Pending(reason);
			event.PrefetchToken = 0;
		}
		if (ContactActive) RevokedGeneration = std::max(RevokedGeneration, ActiveGeneration);
		if (!ContactActive && Events.empty() && PhysicalDown)
			RevokedGeneration = std::max(RevokedGeneration, EnqueuedGeneration);
		for (const auto &event : Events) {
			if (event.Id > last) break;
			RevokedGeneration = std::max(RevokedGeneration, event.ContactGeneration);
		}
		SuppressContact = PhysicalDown && EnqueuedGeneration <= RevokedGeneration;
		const uint64_t owner = ContactActive ? ContactOwner : (Events.empty() ? 0 : Events.front().HostToken);
		while (!Events.empty() && Events.front().Id <= last) {
			NotifyDiscard(Events.front(), DiscardReason::Cancellation);
			PayloadBytes -= Events.front().Payload.size();
			Events.pop_front();
		}
		if (!Batches.empty()) Batches.front().End = true;
		ReadyCache = {};
		ReadyCache.State = Status::Cancelled;
		ReadyCache.Error = reason;
		ReadyCache.HostToken = owner;
		ReadyCache.Batch = Batches.empty() ? 0 : Batches.front().Id;
		ReadyCache.Event = std::numeric_limits<uint64_t>::max();
		ReadyCache.Pointer.Cancelled = true;
		ReadyCache.Pointer.Inside = false;
		ReadyCache.Pointer.Down = SuppressContact;
		LastReady = ReadyCache.Event;
		return ReadyCache;
	}

	SpatialInputQueue::Result
	SpatialInputQueue::Poll(Renderer &renderer, ecs::Entity captured, bool capturedSpatial) {
		RendererDepth depth(renderer);
		return Poll(depth, captured, capturedSpatial);
	}
	SpatialInputQueue::Result
	SpatialInputQueue::Poll(SpatialInputDepth &depth, ecs::Entity captured, bool capturedSpatial) {
		if (LastReady) return ReadyCache;
		if (DeferredFailure != Failure::None) return Fail(depth, DeferredFailure);
		if (!Batches.empty() && Batches.front().End) {
			ReadyCache = {};
			ReadyCache.State = Status::BatchEnd;
			ReadyCache.Event = std::numeric_limits<uint64_t>::max() - 2;
			ReadyCache.Batch = Batches.front().Id;
			ReadyCache.HostToken = Batches.front().Host;
			ReadyCache.Store = Batches.front().Store;
			LastReady = ReadyCache.Event;
			return ReadyCache;
		}
		if (ReleasePending) {
			ReadyCache = {};
			ReadyCache.State = Status::Cancelled;
			ReadyCache.Event = std::numeric_limits<uint64_t>::max() - 1;
			ReadyCache.HostToken = ContactOwner;
			ReadyCache.Pointer.Cancelled = true;
			ReadyCache.Pointer.Inside = false;
			ReadyCache.Pointer.Down = false;
			LastReady = ReadyCache.Event;
			return ReadyCache;
		}
		if (Batches.empty()) return {};
		if (!Batches.front().Prepared) return Pending();
		while (!Events.empty() && Events.front().Id <= Batches.front().LastEvent &&
			   Events.front().ContactGeneration && Events.front().ContactGeneration <= RevokedGeneration) {
			NotifyDiscard(Events.front(), DiscardReason::Revoked);
			PayloadBytes -= Events.front().Payload.size();
			Events.pop_front();
		}
		if (Events.empty() || Events.front().Id > Batches.front().LastEvent) {
			Batches.front().End = true;
			return Poll(depth, captured, capturedSpatial);
		}
		auto &event = Events.front();
		if (std::chrono::steady_clock::now() >= event.Deadline) return Fail(depth, Failure::Timeout);
		const auto *frame = Find(event.Sequence);
		if (!frame || !frame->Published) return Fail(depth, Failure::StaleFrame);
		// A physical contact cannot move to another viewport's router midway.
		if (ContactActive && event.Type == Kind::Pointer && event.HostToken != ContactOwner)
			return Fail(depth, Failure::ExternalCancel);
		if (event.Authoring) {
			captured = {};
			capturedSpatial = false;
		}
		Result result;
		result.State = Status::Ready;
		result.Type = event.Type;
		result.Event = event.Id;
		result.HostToken = event.HostToken;
		result.Sequence = event.Sequence;
		result.Store = frame->Gui.StoreIdentity();
		result.Batch = Batches.front().Id;
		result.Authoring = event.Authoring;
		result.Depth = frame->Depth;
		result.FrameInfo = event.Info;
		result.Screen = frame->Screen;
		result.Resolver = frame->Resolver;
		result.FrameMetadata = frame->Metadata;
		result.Frame = &frame->Gui;
		result.Payload = event.Payload;
		result.Pointer = event.Pointer;
		const auto ready = [&]() {
			if (!event.Authoring && event.Type == Kind::Pointer && result.Pointer.Cancelled)
				result.Pointer.Down = PhysicalDown && EnqueuedGeneration <= event.ContactGeneration;
			LastReady = event.Id;
			ReadyCache = result;
			return result;
		};
		if (event.Type == Kind::Opaque) {
			if (event.Unavailable) return Fail(depth, Failure::StaleFrame);
			return ready();
		}
		result.Pointer.Collector = {};
		result.Pointer.ScreenOnly = true;
		if (!event.Pointer.Inside || event.Pointer.Cancelled) {
			if (capturedSpatial) {
				result.Pointer.Collector = captured;
				result.Pointer.ScreenOnly = false;
				result.Pointer.Cancelled = true;
			}
			return ready();
		}
		if (captured != ecs::NULL_ENTITY && !capturedSpatial) return ready();
		if (!capturedSpatial) {
			if (event.Authoring) {
				if (frame->Gui.VisibleAt(result.Pointer) != ecs::NULL_ENTITY) return ready();
			} else {
				const auto screen = frame->Gui.PointerStatus(result.Pointer);
				if (screen == gui::PointerFrameStatus::Invalid) return Fail(depth, Failure::InvalidSample);
				if (screen == gui::PointerFrameStatus::Consume) return ready();
			}
		}
		if (!capturedSpatial && !HasSpatial(frame->Gui, event.Authoring)) return ready();
		if (!event.Info) return Fail(depth, Failure::StaleFrame);
		const auto &info = event.Info;
		SpatialPickRay ray;
		if (!MakeSpatialPickRay(event.Screen, event.Pointer, *info, ray))
			return Fail(depth, Failure::InvalidSample);
		SpatialPickSelection overlay;
		bool normalCandidate = false, capturedFound = false;
		const auto visit = [&](std::optional<float> sceneDepth, SpatialPickSelection &selection) {
			for (size_t index = 0; index < frame->Gui.SpatialCanvasCount(); ++index) {
				SpatialCollector collector;
				if (!frame->Gui.SpatialCanvasAt(index, collector.Collector, collector.Canvas)) return false;
				if (capturedSpatial && collector.Collector != captured) continue;
				SpatialPickCandidate candidate;
				if (event.Authoring) collector.Canvas.Interactive = true;
				if (!ProjectSpatialCollector(collector, *info, ray, capturedSpatial, candidate)) continue;
				capturedFound = capturedFound || capturedSpatial;
				auto projected = event.Pointer;
				projected.Position = candidate.Position;
				projected.Collector = candidate.Collector;
				projected.ScreenOnly = false;
				if (!capturedSpatial) {
					if (event.Authoring) {
						if (frame->Gui.VisibleAt(projected) == ecs::NULL_ENTITY) continue;
					} else {
						const auto accepts = frame->Gui.PointerStatus(projected);
						if (accepts == gui::PointerFrameStatus::Invalid) return false;
						if (accepts != gui::PointerFrameStatus::Consume) continue;
					}
				}
				if (!candidate.AlwaysOnTop && !capturedSpatial) normalCandidate = true;
				ConsiderSpatialCandidate(
					selection,
					candidate,
					event.Authoring ? frame->Gui.VisiblePaintOrder(projected)
									: frame->Gui.PaintOrder(projected),
					sceneDepth,
					capturedSpatial
				);
			}
			return true;
		};
		if (!visit({}, overlay)) return Fail(depth, Failure::InvalidSample);
		if (capturedSpatial && !capturedFound) return Fail(depth, Failure::InvalidSample);
		if (overlay.HasCandidate || !normalCandidate) {
			if (overlay.HasCandidate) {
				result.Pointer.Position = overlay.Candidate.Position;
				result.Pointer.Collector = overlay.Candidate.Collector;
				result.Pointer.ScreenOnly = false;
			}
			return ready();
		}
		if (event.Unavailable || !event.Prefetched) return Fail(depth, Failure::DepthUnavailable);
		SpatialPickSelection selected;
		if (!visit(event.SampleDepth, selected)) return Fail(depth, Failure::InvalidSample);
		if (selected.HasCandidate) {
			result.Pointer.Position = selected.Candidate.Position;
			result.Pointer.Collector = selected.Candidate.Collector;
			result.Pointer.ScreenOnly = false;
		}
		return ready();
	}

	bool SpatialInputQueue::Acknowledge(uint64_t event) {
		if (!LastReady || LastReady != event) return false;
		if (LastReady == std::numeric_limits<uint64_t>::max() - 2) {
			if (Batches.empty() || !Batches.front().End) return false;
			Batches.pop_front();
		} else if (LastReady == std::numeric_limits<uint64_t>::max() - 1) {
			ReleasePending = false;
			ContactActive = false;
			ContactOwner = 0;
			if (!PhysicalDown) {
				SuppressContact = false;
				EnqueuedDown = false;
			}
		} else if (LastReady != std::numeric_limits<uint64_t>::max()) {
			if (Events.empty() || Events.front().Id != event) return false;
			if (Events.front().Type == Kind::Pointer && !Events.front().Authoring) {
				if (ReadyCache.Pointer.Cancelled) {
					RevokedGeneration = std::max(RevokedGeneration, Events.front().ContactGeneration);
					SuppressContact = ReadyCache.Pointer.Down;
					ContactOwner = SuppressContact ? Events.front().HostToken : 0;
					ActiveGeneration = SuppressContact ? Events.front().ContactGeneration : 0;
					ContactActive = SuppressContact;
					if (!PhysicalDown) EnqueuedDown = false;
				} else if (Events.front().Pointer.Down) {
					ContactOwner = Events.front().HostToken;
					ActiveGeneration = Events.front().ContactGeneration;
					ContactActive = true;
				} else {
					ContactOwner = 0;
					ActiveGeneration = 0;
					ContactActive = false;
				}
			}
			PayloadBytes -= Events.front().Payload.size();
			Events.pop_front();
			if (!Batches.empty() && (Events.empty() || Events.front().Id > Batches.front().LastEvent))
				Batches.front().End = true;
		} else {
			DeferredFailure = Failure::None;
			if (!SuppressContact) {
				if (!PhysicalDown) EnqueuedDown = false;
				ContactOwner = 0;
				ContactActive = false;
			}
		}
		LastReady = 0;
		ReadyCache = {};
		return true;
	}
	bool SpatialInputQueue::DiscardBorrowed() {
		if (LastReady && !Events.empty() && Events.front().Id == LastReady)
			NotifyDiscard(Events.front(), DiscardReason::Borrowed);
		return !LastReady || Acknowledge(LastReady);
	}

	bool SpatialInputQueue::RetireHost(Renderer &renderer, uint64_t hostToken, uint64_t viewport) {
		RendererDepth depth(renderer);
		return RetireHost(depth, hostToken, viewport);
	}
	bool SpatialInputQueue::RetireHost(SpatialInputDepth &depth, uint64_t hostToken, uint64_t viewport) {
		if (LastReady || DeferredFailure != Failure::None) return false;
		for (auto &event : Events) {
			if (event.HostToken != hostToken || !event.PrefetchToken) continue;
			const auto *frame = Find(event.Sequence);
			if (!frame || !depth.Cancel(frame->Depth, event.PrefetchToken)) return false;
			event.PrefetchToken = 0;
		}
		for (auto batch = Batches.begin(); batch != Batches.end();) {
			if (batch->Host == hostToken)
				batch = Batches.erase(batch);
			else
				++batch;
		}
		for (auto event = Events.begin(); event != Events.end();) {
			if (event->HostToken == hostToken) {
				NotifyDiscard(*event, DiscardReason::HostRetired);
				PayloadBytes -= event->Payload.size();
				event = Events.erase(event);
			} else
				++event;
		}
		if (ContactActive && ContactOwner == hostToken) {
			SuppressContact = PhysicalDown && EnqueuedGeneration <= ActiveGeneration;
			ContactActive = false;
			ContactOwner = 0;
			ReleasePending = false;
		}
		for (auto frame = Frames.begin(); frame != Frames.end();) {
			if (frame->Viewport != viewport || frame->HostToken != hostToken ||
				std::any_of(Events.begin(), Events.end(), [&](const Event &event) {
					return event.Sequence == frame->Sequence;
				})) {
				++frame;
				continue;
			}
			FrameBytes -= frame->RetainedBytes();
			frame = Frames.erase(frame);
		}
		return true;
	}

	bool SpatialInputQueue::Reset(Renderer &renderer) {
		RendererDepth depth(renderer);
		return Reset(depth);
	}
	bool SpatialInputQueue::Reset(SpatialInputDepth &depth) {
		if (LastReady) return false;
		for (auto &event : Events) {
			if (!event.PrefetchToken) continue;
			const auto *frame = Find(event.Sequence);
			if (!frame || !depth.Cancel(frame->Depth, event.PrefetchToken)) return false;
			event.PrefetchToken = 0;
		}
		for (const auto &event : Events)
			NotifyDiscard(event, DiscardReason::Reset);
		Batches.clear();
		Events.clear();
		Frames.clear();
		FrameBytes = PayloadBytes = 0;
		DeferredFailure = Failure::None;
		SuppressContact = PhysicalDown;
		ContactActive = false;
		ContactOwner = 0;
		ReleasePending = false;
		return true;
	}
} // namespace engine::render
