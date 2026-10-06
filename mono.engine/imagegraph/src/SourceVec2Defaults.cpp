#include "SourceVec2Defaults.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
namespace engine::imagegraph::detail {
	Status ValidateSourceVec2Defaults(const Node &node, Diagnostic &diagnostic) {
		if (!node.SourceVec2Defaults) return Status::Ok;
		const auto fail = [&](Status code, std::string_view port, std::string_view message) {
			diagnostic = {code, node.Id, std::string(port), std::string(message)};
			return code;
		};
		if (node.DynamicInputs.size() > MaximumDynamicInputsForNode(node))
			return fail(Status::LimitExceeded, {}, "source constructor dynamic input count exceeds bounds");
		const auto &inputs = node.SourceVec2Defaults->Inputs;
		if (inputs.empty()) return fail(Status::InvalidValue, {}, "source constructor defaults are empty");
		if (inputs.size() > Limits::MaximumArrayElements ||
			inputs.size() > SourceSeparatedVec2InputCount(node))
			return fail(Status::LimitExceeded, {}, "source constructor default count exceeds controls");
		std::array<std::string_view, Limits::MaximumArrayElements> ports{};
		size_t count = 0;
		for (const auto &input : inputs) {
			if (input.Port.empty() || input.Port.size() > Limits::MaximumTextBytes)
				return fail(Status::LimitExceeded, {}, "source constructor port exceeds text bounds");
			if (!SourceSeparatedVec2Input(node, input.Port))
				return fail(
					Status::UnknownPort, input.Port, "source constructor default needs a two-axis control"
				);
			if (!std::isfinite(input.Data.X) || !std::isfinite(input.Data.Y))
				return fail(Status::InvalidValue, input.Port, "source constructor default must be finite");
			if (std::find(ports.begin(), ports.begin() + count, input.Port) != ports.begin() + count)
				return fail(Status::DuplicateId, input.Port, "source constructor default repeats a port");
			ports[count++] = input.Port;
		}
		return Status::Ok;
	}
	std::optional<uint64_t> SourceVec2DefaultsBytes(const Node &node, bool retained) {
		if (!node.SourceVec2Defaults) return uint64_t{0};
		Diagnostic diagnostic;
		if (ValidateSourceVec2Defaults(node, diagnostic) != Status::Ok) return std::nullopt;
		const auto &inputs = node.SourceVec2Defaults->Inputs;
		const auto slots = retained ? inputs.capacity() : inputs.size();
		if (slots > (UINT64_MAX - sizeof(SourceVec2DefaultsData)) / sizeof(SourceVec2Default))
			return std::nullopt;
		uint64_t bytes = sizeof(SourceVec2DefaultsData) + slots * sizeof(SourceVec2Default);
		for (const auto &input : inputs) {
			const uint64_t text =
				retained ? input.Port.capacity() : std::max(input.Port.size(), std::string{}.capacity());
			if (text > UINT64_MAX - bytes) return std::nullopt;
			bytes += text;
		}
		return bytes;
	}
	std::optional<Vector2> SourceVec2ConstructorDefault(const Node &node, std::string_view port) {
		const auto *input = SourceSeparatedVec2Input(node, port);
		if (!input) return std::nullopt;
		if (node.SourceVec2Defaults)
			for (const auto &value : node.SourceVec2Defaults->Inputs)
				if (value.Port == port)
					return std::isfinite(value.Data.X) && std::isfinite(value.Data.Y)
							   ? std::optional<Vector2>{value.Data}
							   : std::nullopt;
		// preview defaults can substitute project dimensions; they are not constructor provenance.
		if (!input->SourceConstructorConstant) return std::nullopt;
		auto text = input->Default;
		if (!text.starts_with("v ")) return std::nullopt;
		text.remove_prefix(2);
		Vector2 value;
		for (auto *number : {&value.X, &value.Y}) {
			while (!text.empty() && text.front() == ' ')
				text.remove_prefix(1);
			const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *number);
			if (parsed.ec != std::errc{} || !std::isfinite(*number)) return std::nullopt;
			text.remove_prefix(parsed.ptr - text.data());
		}
		while (!text.empty() && text.front() == ' ')
			text.remove_prefix(1);
		return text.empty() ? std::optional<Vector2>{value} : std::nullopt;
	}
}
