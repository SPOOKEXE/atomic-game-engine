#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/PendingHostObservations.hpp>

#include <algorithm>
#include <new>

namespace engine::imagegraph {
	void PendingHostObservations::Clear() {
		*this = {};
	}
	bool PendingHostObservations::Capture(
		const HostNodeInvocation &invocation,
		HostNodeProvider &provider,
		HostNodeCapture &output,
		std::string &failure
	) try {
		ENGINE_PROFILE("imagegraph pending host observation");
		const auto &request = invocation.Request;
		const auto sameImages = [](const auto &a, const auto &b) {
			if (a.size() != b.size()) return false;
			for (size_t i = 0; i < a.size(); ++i)
				if (a[i].Port != b[i].Port || a[i].Hash != b[i].Hash) return false;
			return true;
		};
		if (Active && (Tick != request.Tick || Seed != request.Seed || Subframe != request.Subframe ||
					   NegativeFrame != request.NegativeFrame)) {
			failure = "Pending host observations require their admitted frame and seed";
			return false;
		}
		const uint64_t maximum = invocation.MaximumOperationBytes;
		const auto previous = HostCaptureRetainedPayloadBytes(output);
		const uint64_t backing = sizeof(*this) + Captures.capacity() * sizeof(HostNodeCapture);
		if (!previous || *previous > maximum || backing > maximum - *previous ||
			Bytes > maximum - *previous - backing) {
			failure = "Pending host observations and previous output exceed budget";
			return false;
		}
		const uint64_t held = Bytes + backing + *previous;
		HostNodeCapture expected;
		uint64_t expectedBytes = 0;
		Diagnostic diagnostic;
		if (PrepareResolvedHostCapture(invocation, maximum - held, expected, expectedBytes, diagnostic) !=
			Status::Ok) {
			failure = diagnostic.Message;
			return false;
		}
		auto found = std::find_if(Captures.begin(), Captures.end(), [&](const auto &v) {
			return v.Authored.Id == invocation.Authored.Id;
		});
		if (found != Captures.end()) {
			if (found->Authored != expected.Authored || found->Inputs != expected.Inputs ||
				!sameImages(found->InputImages, expected.InputImages)) {
				failure = "Pending host inputs changed within an immutable observation frame";
				return false;
			}
			const auto copy = HostCaptureRetainedPayloadBytes(*found);
			if (!copy || *copy > maximum - held - expectedBytes) {
				failure = "Pending host replay copy exceeds budget";
				return false;
			}
			HostNodeCapture candidate = *found;
			core::Metrics::Count("imagegraph.pending_host.receipt_copies", 1);
			output = std::move(candidate);
			failure.clear();
			return true;
		}
		if (Captures.size() >= 64) {
			failure = "Pending host observation count exceeds budget";
			return false;
		}
		const uint64_t available = maximum - held - expectedBytes;
		// Two copies and possible old/new vector backing remain alive together.
		const uint64_t growth = (Captures.size() + 1) * sizeof(HostNodeCapture);
		if (growth > available) {
			failure = "Pending host vector growth exceeds budget";
			return false;
		}
		std::vector<HostNodeCapture> grown;
		if (Captures.size() == Captures.capacity()) {
			grown.reserve(Captures.size() + 1);
			if (grown.capacity() * sizeof(HostNodeCapture) > available) {
				failure = "Pending host actual vector capacity exceeds budget";
				return false;
			}
		}
		const uint64_t newBacking = grown.capacity() * sizeof(HostNodeCapture);
		HostNodeInvocation bounded = invocation;
		bounded.MaximumOperationBytes = (available - newBacking) / 2;
		HostNodeCapture observed;
		if (!provider.Capture(bounded, observed, failure)) return false;
		const auto bytes = HostCaptureRetainedPayloadBytes(observed);
		if (!bytes || *bytes > bounded.MaximumOperationBytes || observed.Authored != expected.Authored ||
			observed.Inputs != expected.Inputs || !sameImages(observed.InputImages, expected.InputImages) ||
			observed.Tick != request.Tick || observed.Subframe != request.Subframe ||
			observed.NegativeFrame != request.NegativeFrame) {
			failure = "Pending host result exceeds budget or differs from its resolved receipt";
			return false;
		}
		HostNodeCapture candidate = observed;
		core::Metrics::Count("imagegraph.pending_host.receipt_copies", 2);
		if (grown.capacity()) {
			for (auto &v : Captures)
				grown.push_back(std::move(v));
			Captures.swap(grown);
		}
		Captures.push_back(std::move(observed));
		Bytes += *bytes;
		Active = true;
		Tick = request.Tick;
		Seed = request.Seed;
		Subframe = request.Subframe;
		NegativeFrame = request.NegativeFrame;
		output = std::move(candidate);
		failure.clear();
		return true;
	} catch (const std::bad_alloc &) {
		failure = "Pending host observation allocation failed";
		return false;
	}
}
