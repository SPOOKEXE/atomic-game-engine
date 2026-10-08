#include "SourceArgumentTag.hpp"
#include "SourceChoice.hpp"
#include "SourceRealNumber.hpp"
#include "ValuePayload.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceArgumentHost.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <new>
#include <stdexcept>

namespace engine::imagegraph {
	using detail::NormalizeSourceChoice;
	using detail::RetainedPayloadBytes;
	using detail::SourceChoiceNumber;
	using detail::ValidRuntimeValue;
	namespace {
		std::optional<uint64_t> TableBytes(std::span<const AuthoredValue> values, uint64_t slots) {
			uint64_t bytes = sizeof(std::vector<AuthoredValue>) + slots * sizeof(AuthoredValue);
			for (const auto &value : values) {
				const auto clone = ValueClonePayloadBytes(value.Data);
				const std::optional<uint64_t> payload =
					clone ? std::optional<uint64_t>{std::max(*clone, RetainedPayloadBytes(value.Data))}
						  : std::nullopt;
				const uint64_t text = value.Port.capacity() + 1;
				if (!payload || *payload > UINT64_MAX - text || bytes > UINT64_MAX - text - *payload)
					return {};
				bytes += text + *payload;
			}
			return bytes;
		}
		const Value *Control(const HostNodeInvocation &invocation, std::string_view port) {
			for (const auto &input : invocation.Inputs)
				if (input.Port == port) return &input.Data;
			return nullptr;
		}
		bool SpaceName(std::string_view stored, std::string_view tag) {
			if (stored.size() != tag.size()) return false;
			for (size_t i = 0; i < tag.size(); ++i)
				if (stored[i] != (tag[i] == '_' ? ' ' : tag[i])) return false;
			return true;
		}
	}
	uint64_t SourceArgumentHost::RetainedBytes() const {
		return TableBytes(Arguments, Arguments.capacity()).value_or(UINT64_MAX);
	}
	Status SourceArgumentHost::Prepare(
		std::span<const AuthoredValue> arguments, uint64_t maximumBytes, Diagnostic &diagnostic
	) try {
		ENGINE_PROFILE("imagegraph source arguments prepare");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return code;
		};
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		if (arguments.size() > Limits::MaximumLinks)
			return fail(Status::LimitExceeded, "argument table exceeds count limit");
		uint64_t comparisonWork = 0;
		for (size_t i = 0; i < arguments.size(); ++i) {
			if (arguments[i].Port.size() > Limits::MaximumTextBytes || !ValidRuntimeValue(arguments[i].Data))
				return fail(Status::InvalidValue, "argument table observation is invalid");
			for (size_t j = 0; j < i; ++j) {
				comparisonWork += std::min(arguments[i].Port.size(), arguments[j].Port.size()) + 1;
				if (comparisonWork > (1u << 24))
					return fail(Status::LimitExceeded, "argument name comparison work exceeds limit");
				if (arguments[i].Port == arguments[j].Port)
					return fail(Status::DuplicateId, "argument table repeats a name");
			}
		}
		const auto prospective = TableBytes(arguments, arguments.size());
		const uint64_t previous = RetainedBytes();
		if (!prospective || previous > maximumBytes || *prospective > (maximumBytes - previous) / 2)
			return fail(Status::LimitExceeded, "argument table replacement exceeds byte budget");
		std::vector<AuthoredValue> candidate(arguments.begin(), arguments.end());
		const auto actual = TableBytes(candidate, candidate.capacity());
		if (!actual || *actual > maximumBytes - previous)
			return fail(Status::LimitExceeded, "argument table capacities exceed byte budget");
		Arguments = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "argument table allocation failed"};
		return diagnostic.Code;
	} catch (const std::length_error &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "argument table allocation length exceeded"};
		return diagnostic.Code;
	}
	Status SourceArgumentHost::PrepareOptions(
		const SourceArgumentOptions &options, uint64_t maximumBytes, Diagnostic &diagnostic
	) try {
		ENGINE_PROFILE("imagegraph source argument options");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return code;
		};
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		const std::array groups = {options.Text, options.Boolean, options.Integer, options.Real};
		size_t count = 0;
		uint64_t projected = sizeof(std::vector<AuthoredValue>);
		for (const auto group : groups) {
			if (group.size() > Limits::MaximumLinks - count)
				return fail(Status::LimitExceeded, "argument options exceed count limit");
			count += group.size();
			for (const auto assignment : group) {
				const auto split = assignment.find('=');
				if (split == std::string_view::npos || split == 0)
					return fail(Status::InvalidValue, "argument option requires nonempty name=value");
				if (split > Limits::MaximumTextBytes ||
					assignment.size() - split - 1 > Limits::MaximumTextBytes)
					return fail(Status::LimitExceeded, "argument option exceeds text limit");
				projected += sizeof(AuthoredValue) + 2 * (assignment.size() + 32);
				if (projected > maximumBytes)
					return fail(Status::LimitExceeded, "argument option scratch exceeds byte budget");
			}
		}
		const uint64_t prior = RetainedBytes();
		if (prior > maximumBytes || projected > (maximumBytes - prior) / 3)
			return fail(Status::LimitExceeded, "argument option replacement exceeds byte budget");
		std::vector<AuthoredValue> candidate;
		candidate.reserve(count);
		if (candidate.capacity() > count)
			return fail(Status::LimitExceeded, "argument option slots exceed admitted capacity");
		for (size_t kind = 0; kind < std::size(groups); ++kind)
			for (const auto assignment : groups[kind]) {
				const auto split = assignment.find('=');
				const auto name = assignment.substr(0, split), raw = assignment.substr(split + 1);
				Value value;
				if (kind == 0)
					value = std::string(raw);
				else if (kind == 1) {
					if (raw != "true" && raw != "false")
						return fail(Status::InvalidValue, "boolean argument requires true or false");
					value = raw == "true";
				} else if (kind == 2) {
					int64_t number = 0;
					const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), number);
					if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size())
						return fail(Status::InvalidValue, "integer argument requires a signed decimal int64");
					value = number;
				} else {
					double number = 0;
					const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), number);
					if (parsed.ec != std::errc{} || parsed.ptr != raw.data() + raw.size() ||
						!std::isfinite(number))
						return fail(Status::InvalidValue, "real argument requires a finite decimal number");
					value = number;
				}
				candidate.push_back({std::string(name), std::move(value)});
			}
		const auto scratch = TableBytes(candidate, candidate.capacity());
		if (!scratch || *scratch > maximumBytes)
			return fail(Status::LimitExceeded, "argument option capacities exceed byte budget");
		return Prepare(candidate, maximumBytes - *scratch, diagnostic);
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "argument options allocation failed"};
		return diagnostic.Code;
	} catch (const std::length_error &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "argument options allocation length exceeded"};
		return diagnostic.Code;
	}

	bool SourceArgumentHost::Capture(
		const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure
	) try {
		ENGINE_PROFILE("imagegraph source argument capture");
		const auto refuse = [&](const char *message) {
			failure = message;
			return false;
		};
		if (invocation.Authored.Type != "pc.argument")
			return refuse("argument provider only admits pc.argument");
		const auto *tagValue = Control(invocation, "tag"), *type = Control(invocation, "type"),
				   *defaultValue = Control(invocation, "default_value");
		const auto tag = tagValue ? detail::SourceArgumentTagText(*tagValue) : std::nullopt;
		const auto rawMode = type ? SourceChoiceNumber(*type) : std::nullopt;
		const auto *entry = FindCatalogueEntry("pc.argument");
		const auto *slot = entry ? FindCatalogueInput(*entry, "type") : nullptr;
		const auto mode = rawMode && slot ? NormalizeSourceChoice(*slot, *rawMode, false) : std::nullopt;
		if (!tag || !defaultValue || !mode || (*mode != 0 && *mode != 1))
			return refuse("argument controls need a source primitive tag and resolved String or Number mode");
		const Value *value = nullptr;
		for (const auto &argument : Arguments)
			if (argument.Port == tag->View() && !std::holds_alternative<UndefinedValue>(argument.Data)) {
				value = &argument.Data;
				break;
			}
		if (!value)
			for (const auto &argument : Arguments)
				if (SpaceName(argument.Port, tag->View()) &&
					!std::holds_alternative<UndefinedValue>(argument.Data)) {
					value = &argument.Data;
					break;
				}
		if (!value) value = defaultValue;
		const auto clone = ValueClonePayloadBytes(*value);
		const uint64_t maximum = std::min(invocation.MaximumOperationBytes, Limits::MaximumEvaluationBytes);
		const auto previous = HostCaptureRetainedPayloadBytes(output);
		const uint64_t publication =
			sizeof(AuthoredValue) + std::string{}.capacity() + 1 + clone.value_or(maximum);
		if (!clone || !previous || *previous > maximum || publication > (maximum - *previous) / 2)
			return refuse("argument output replacement exceeds operation budget");
		std::optional<Value> converted;
		if (*mode == 1) {
			converted = detail::SourceRealNumber(*value);
			if (!converted)
				return refuse(
					"argument Number conversion requires a bounded primitive or final source recording"
				);
			value = &*converted;
		}
		HostNodeCapture candidate;
		uint64_t bytes = 0;
		Diagnostic diagnostic;
		if (PrepareResolvedHostCapture(
				invocation, maximum - publication - *previous, candidate, bytes, diagnostic
			) != Status::Ok) {
			failure = diagnostic.Message;
			return false;
		}
		candidate.Outputs.reserve(1);
		if (candidate.Outputs.capacity() != 1)
			return refuse("argument output slots exceed admitted capacity");
		candidate.Outputs.push_back({"value", *value});
		const auto actual = HostCaptureRetainedPayloadBytes(candidate);
		if (!actual || *actual > maximum - *previous - publication)
			return refuse("argument receipt retained capacity exceeds operation budget");
		output = std::move(candidate);
		failure.clear();
		return true;
	} catch (const std::bad_alloc &) {
		failure = "argument receipt allocation failed";
		return false;
	} catch (const std::length_error &) {
		failure = "argument receipt allocation length exceeded";
		return false;
	}
}
