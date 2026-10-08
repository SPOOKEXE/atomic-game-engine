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
		std::string &failure,
		size_t sequence
	) try {
		ENGINE_PROFILE("imagegraph pending host observation");
		const auto &request = invocation.Request;
		if (sequence > CaptureSequences.size()) {
			failure = "Pending host capture sequence exceeds bounds";
			return false;
		}
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
		const auto retained = RetainedPayloadBytes();
		if (!retained || *retained < Bytes) {
			failure = "Pending host observation residency is invalid";
			return false;
		}
		const uint64_t backing = *retained - Bytes;
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
		const auto sameContext = [&](const PendingHostInvocationContext &saved) {
			return saved.OutputFormat == invocation.OutputFormat &&
				   saved.Interpolation == invocation.Interpolation &&
				   saved.Timeline.has_value() == bool(invocation.Timeline) &&
				   (!invocation.Timeline || *saved.Timeline == *invocation.Timeline);
		};
		auto found = std::find_if(Captures.begin(), Captures.end(), [&](const auto &v) {
			return v.Authored.Id == invocation.Authored.Id &&
				   CaptureSequences[static_cast<size_t>(&v - Captures.data())] == sequence;
		});
		if (sequence && std::any_of(Captures.begin(), Captures.end(), [&](const auto &value) {
				return CaptureSequences[static_cast<size_t>(&value - Captures.data())] == sequence &&
					   value.Authored.Id != invocation.Authored.Id;
			})) {
			failure = "Pending host callback order changed within an immutable frame";
			return false;
		}
		if (found != Captures.end()) {
			if (!sameContext(CaptureContexts[static_cast<size_t>(&*found - Captures.data())]) ||
				found->Authored != expected.Authored || found->Inputs != expected.Inputs ||
				found->CameraPolicy != expected.CameraPolicy || found->CameraRow != expected.CameraRow ||
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
		if (sequence && PendingInput && PendingSequence == sequence &&
			(!PendingContext || !sameContext(*PendingContext) ||
			 PendingInput->Authored != expected.Authored || PendingInput->Inputs != expected.Inputs ||
			 PendingInput->CameraPolicy != expected.CameraPolicy ||
			 PendingInput->CameraRow != expected.CameraRow ||
			 !sameImages(PendingInput->InputImages, expected.InputImages))) {
			failure = "Pending host callback invocation changed before completion";
			return false;
		}
		uint64_t available = maximum - held - expectedBytes;
		const uint64_t contextCopy =
			invocation.Timeline ? std::max(invocation.Timeline->Playback.size(), std::string{}.capacity()) + 1
								: 0;
		if ((invocation.Timeline && invocation.Timeline->Playback.size() > Limits::MaximumTextBytes) ||
			contextCopy > available) {
			failure = "Pending host invocation context exceeds budget";
			return false;
		}
		PendingHostInvocationContext context{
			invocation.Timeline ? std::optional<TimelineSettings>(*invocation.Timeline) : std::nullopt,
			invocation.OutputFormat,
			invocation.Interpolation
		};
		const uint64_t actualContext = context.Timeline ? context.Timeline->Playback.capacity() + 1 : 0;
		if (actualContext > available) {
			failure = "Pending host invocation context capacity exceeds budget";
			return false;
		}
		available -= actualContext;
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
		if (!provider.Capture(bounded, observed, failure)) {
			if (sequence) {
				PendingInput = std::move(expected);
				PendingContext = std::move(context);
				PendingSequence = sequence;
				Active = true;
				Tick = request.Tick;
				Seed = request.Seed;
				Subframe = request.Subframe;
				NegativeFrame = request.NegativeFrame;
			}
			return false;
		}
		const auto bytes = HostCaptureRetainedPayloadBytes(observed);
		if (!ValidHostSourceFrameObservation(observed) || !bytes || *bytes > bounded.MaximumOperationBytes ||
			observed.Authored != expected.Authored || observed.Inputs != expected.Inputs ||
			observed.CameraPolicy != expected.CameraPolicy || observed.CameraRow != expected.CameraRow ||
			!sameImages(observed.InputImages, expected.InputImages) || observed.Tick != request.Tick ||
			observed.Subframe != request.Subframe || observed.NegativeFrame != request.NegativeFrame) {
			failure = "Pending host result exceeds budget or differs from its resolved receipt";
			return false;
		}
		HostNodeCapture candidate = observed;
		core::Metrics::Count("imagegraph.pending_host.receipt_copies", 2);
		core::Metrics::Count(
			"imagegraph.pending_host.receipt_payload_charge_bytes", static_cast<double>(*bytes)
		);
		if (grown.capacity()) {
			for (auto &v : Captures)
				grown.push_back(std::move(v));
			Captures.swap(grown);
		}
		CaptureSequences[Captures.size()] = sequence;
		CaptureContexts[Captures.size()] = std::move(context);
		Captures.push_back(std::move(observed));
		Bytes += *bytes;
		if (sequence && PendingSequence == sequence) {
			PendingInput.reset();
			PendingContext.reset();
			PendingSequence = 0;
		}
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
	void PendingHostObservations::BeginAttempt() noexcept {
		MessageCursor = CaptureCursor = 0;
	}
	bool PendingHostObservations::CaptureSequenced(
		const HostNodeInvocation &invocation,
		HostNodeProvider &provider,
		HostNodeCapture &output,
		std::string &failure
	) {
		if (CaptureCursor >= CaptureSequences.size()) {
			failure = "Pending host callback count exceeds bounds";
			return false;
		}
		return Capture(invocation, provider, output, failure, ++CaptureCursor);
	}
	std::optional<uint64_t> PendingHostObservations::RetainedPayloadBytes() const {
		if (Captures.size() > 64 || MessageReceipts.size() > 64 || CaptureCursor > 64 ||
			MessageCursor > MessageReceipts.size() || PendingInput.has_value() != PendingContext.has_value())
			return std::nullopt;
		uint64_t bytes = sizeof(*this);
		const auto add = [&](uint64_t amount) {
			if (amount > UINT64_MAX - bytes) return false;
			bytes += amount;
			return true;
		};
		if (Captures.capacity() > UINT64_MAX / sizeof(HostNodeCapture) ||
			MessageReceipts.capacity() > UINT64_MAX / sizeof(PendingHostMessageReceipt) ||
			!add(Captures.capacity() * sizeof(HostNodeCapture)) ||
			!add(MessageReceipts.capacity() * sizeof(PendingHostMessageReceipt)))
			return std::nullopt;
		if (PendingInput) {
			const auto pending = HostCaptureRetainedPayloadBytes(*PendingInput);
			if (!pending || !PendingSequence || PendingSequence > 64 || !PendingContext || !add(*pending) ||
				(PendingContext->Timeline && !add(PendingContext->Timeline->Playback.capacity() + 1)))
				return std::nullopt;
		}
		uint64_t captures = 0;
		for (const auto &capture : Captures) {
			const auto count = HostCaptureRetainedPayloadBytes(capture);
			if (!count || *count > UINT64_MAX - captures || !add(*count)) return std::nullopt;
			captures += *count;
		}
		if (captures != Bytes) return std::nullopt;
		for (const auto &context : CaptureContexts)
			if (context.Timeline && !add(context.Timeline->Playback.capacity() + 1)) return std::nullopt;
		for (const auto &receipt : MessageReceipts) {
			if (receipt.Messages.size() > 64 ||
				receipt.Messages.capacity() > UINT64_MAX / sizeof(PcxMessage) ||
				!add(receipt.Node.capacity() + 1) || !add(receipt.Messages.capacity() * sizeof(PcxMessage)))
				return std::nullopt;
			for (const auto &message : receipt.Messages)
				if (!add(message.Text.capacity() + 1)) return std::nullopt;
		}
		return bytes;
	}
	bool PendingHostObservations::ForwardMessages(
		const EvaluationRequest &request,
		std::string_view node,
		std::span<const PcxMessage> messages,
		HostNodeProvider &provider,
		uint64_t maximumBytes,
		std::string &failure
	) try {
		ENGINE_PROFILE("imagegraph pending host messages");
		const auto fail = [&](const char *message) {
			failure = message;
			return false;
		};
		if (Active && (Tick != request.Tick || Seed != request.Seed || Subframe != request.Subframe ||
					   NegativeFrame != request.NegativeFrame))
			return fail("Pending host message frame changed");
		if (MessageCursor < MessageReceipts.size()) {
			const auto &prior = MessageReceipts[MessageCursor];
			if (prior.Node != node || prior.Messages.size() != messages.size())
				return fail("Pending host message ordering changed");
			for (size_t i = 0; i < messages.size(); ++i)
				if (prior.Messages[i].Text != messages[i].Text ||
					prior.Messages[i].Warning != messages[i].Warning)
					return fail("Pending host messages changed within an immutable frame");
			const auto held = RetainedPayloadBytes();
			if (!held || *held > maximumBytes) return fail("Pending host message residency exceeds budget");
			++MessageCursor;
			failure.clear();
			return true;
		}
		const auto held = RetainedPayloadBytes();
		if (!held || *held > maximumBytes || MessageReceipts.size() >= 64 || messages.size() > 64 ||
			node.size() > Limits::MaximumTextBytes)
			return fail("Pending host message count or residency exceeds budget");
		uint64_t available = maximumBytes - *held;
		const auto admit = [&](uint64_t bytes) {
			if (bytes > available) return false;
			available -= bytes;
			return true;
		};
		if (!admit((MessageReceipts.size() + 1) * sizeof(PendingHostMessageReceipt)) ||
			!admit(std::max(node.size(), std::string{}.capacity()) + 1) ||
			!admit(messages.size() * sizeof(PcxMessage)))
			return fail("Pending host message backing exceeds budget");
		for (const auto &message : messages)
			if (message.Text.size() > Limits::MaximumTextBytes ||
				!admit(std::max(message.Text.size(), std::string{}.capacity()) + 1))
				return fail("Pending host message text exceeds budget");
		PendingHostMessageReceipt candidate{std::string(node), {messages.begin(), messages.end()}};
		std::vector<PendingHostMessageReceipt> replacement;
		replacement.reserve(MessageReceipts.size() + 1);
		uint64_t actual = replacement.capacity() * sizeof(PendingHostMessageReceipt);
		const auto actualAdd = [&](uint64_t bytes) {
			if (bytes > UINT64_MAX - actual) return false;
			actual += bytes;
			return true;
		};
		if (!actualAdd(candidate.Node.capacity() + 1) ||
			!actualAdd(candidate.Messages.capacity() * sizeof(PcxMessage)))
			return fail("Pending host message actual backing exceeds budget");
		for (const auto &message : candidate.Messages)
			if (!actualAdd(message.Text.capacity() + 1))
				return fail("Pending host message actual text exceeds budget");
		if (actual > maximumBytes - *held) return fail("Pending host message actual capacity exceeds budget");
		// All retained slots exist before the visible capability call. A refusal stores no receipt.
		if (!provider.PcxMessages(node, messages, failure)) return false;
		for (auto &old : MessageReceipts)
			replacement.push_back(std::move(old));
		replacement.push_back(std::move(candidate));
		MessageReceipts = std::move(replacement);
		++MessageCursor;
		Active = true;
		Tick = request.Tick;
		Seed = request.Seed;
		Subframe = request.Subframe;
		NegativeFrame = request.NegativeFrame;
		core::Metrics::Count("imagegraph.pending_host.message_batches", 1);
		core::Metrics::Count("imagegraph.pending_host.message_payload_bytes", static_cast<double>(actual));
		failure.clear();
		return true;
	} catch (const std::bad_alloc &) {
		failure = "Pending host message allocation failed";
		return false;
	}

}
