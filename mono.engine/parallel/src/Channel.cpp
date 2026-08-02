#include <engine/parallel/Channel.hpp>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

namespace engine::parallel {

	const char *Describe(ChannelStatus status) {
		switch (status) {
		case ChannelStatus::Ok:
			return "ok";
		case ChannelStatus::Empty:
			return "empty";
		case ChannelStatus::Full:
			return "full";
		case ChannelStatus::Closed:
			return "closed";
		case ChannelStatus::TooLarge:
			return "frame too large";
		}
		// No default label, so adding a status is a compiler warning here.
		return "?";
	}

	namespace {
		// One direction: a queue of whole frames and the bytes they occupy.
		struct Direction {
			mutable std::mutex Guard;
			std::deque<std::vector<std::byte>> Frames;
			size_t Bytes = 0;
		};

		// What both endpoints share.
		//
		// Refcounted rather than owned by one end, because either may close
		// first and the survivor still has to drain what was queued.
		struct Pipe {
			ChannelSettings Settings;

			// Named for the endpoint that *sends* into them, so the wiring at
			// the call site reads without a diagram.
			Direction FirstToSecond;
			Direction SecondToFirst;

			// One flag per end, not one for the pipe: a host that has closed
			// its end should not stop the driver from reading the last thing it
			// said.
			std::atomic<bool> FirstOpen{true};
			std::atomic<bool> SecondOpen{true};
		};

		// One end of a Pipe.
		class LocalChannel final : public Channel {
		  public:
			LocalChannel(std::shared_ptr<Pipe> pipe, bool first) : Shared(std::move(pipe)), First(first) {}

			~LocalChannel() override {
				Close();
			}

			ChannelStatus Send(std::span<const std::byte> frame) override {
				if (!Open()) {
					return ChannelStatus::Closed;
				}
				if (frame.size() > Shared->Settings.MaximumFrame) {
					// Refused whole rather than truncated: half a frame is
					// worse than none, because the reader cannot tell.
					return ChannelStatus::TooLarge;
				}

				Direction &outbound = First ? Shared->FirstToSecond : Shared->SecondToFirst;
				std::lock_guard lock(outbound.Guard);

				if (outbound.Bytes + frame.size() > Shared->Settings.Capacity) {
					// Never blocks. A send that waited for room would stall a
					// job worker, and with it every other world in the host.
					return ChannelStatus::Full;
				}

				outbound.Frames.emplace_back(frame.begin(), frame.end());
				outbound.Bytes += frame.size();
				return ChannelStatus::Ok;
			}

			ChannelStatus Receive(std::vector<std::byte> &frame) override {
				Direction &inbound = First ? Shared->SecondToFirst : Shared->FirstToSecond;
				std::lock_guard lock(inbound.Guard);

				if (inbound.Frames.empty()) {
					// Closed *and* drained, in that order. A peer that exits
					// cleanly should not strip this end of what it already
					// said.
					return Open() ? ChannelStatus::Empty : ChannelStatus::Closed;
				}

				// Assigned rather than moved into, so the caller's capacity
				// survives and a channel polled every tick stops allocating.
				std::vector<std::byte> &front = inbound.Frames.front();
				frame.assign(front.begin(), front.end());

				inbound.Bytes -= front.size();
				inbound.Frames.pop_front();
				return ChannelStatus::Ok;
			}

			size_t Pending() const override {
				const Direction &inbound = First ? Shared->SecondToFirst : Shared->FirstToSecond;
				std::lock_guard lock(inbound.Guard);
				return inbound.Frames.size();
			}

			size_t PendingBytes() const override {
				const Direction &inbound = First ? Shared->SecondToFirst : Shared->FirstToSecond;
				std::lock_guard lock(inbound.Guard);
				return inbound.Bytes;
			}

			bool Open() const override {
				return Shared->FirstOpen.load(std::memory_order_relaxed) &&
					   Shared->SecondOpen.load(std::memory_order_relaxed);
			}

			void Close() override {
				(First ? Shared->FirstOpen : Shared->SecondOpen).store(false, std::memory_order_relaxed);
			}

		  private:
			std::shared_ptr<Pipe> Shared;
			bool First;
		};
	}

	std::pair<std::unique_ptr<Channel>, std::unique_ptr<Channel>>
	MakeLocalChannel(const ChannelSettings &settings) {
		auto pipe = std::make_shared<Pipe>();
		pipe->Settings = settings;

		return {
			std::make_unique<LocalChannel>(pipe, true),
			std::make_unique<LocalChannel>(pipe, false),
		};
	}
}
