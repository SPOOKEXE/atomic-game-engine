#pragma once

#include <engine/imagegraph/HostCapture.hpp>

namespace engine::imagegraph {
	struct SourceArgumentOptions {
		std::span<const std::string_view> Text{};
		std::span<const std::string_view> Boolean{};
		std::span<const std::string_view> Integer{};
		std::span<const std::string_view> Real{};
	};

	inline constexpr std::string_view SOURCE_ARGUMENT_TEXT_OPTION = "graph-argument";
	inline constexpr std::string_view SOURCE_ARGUMENT_BOOLEAN_OPTION = "graph-argument-bool";
	inline constexpr std::string_view SOURCE_ARGUMENT_INTEGER_OPTION = "graph-argument-integer";
	inline constexpr std::string_view SOURCE_ARGUMENT_REAL_OPTION = "graph-argument-real";

	// Owns copied observations. Evaluation never reads process arguments or global state.
	class SourceArgumentHost final : public HostNodeProvider {
	  public:
		SourceArgumentHost() = default;
		SourceArgumentHost(const SourceArgumentHost &) = delete;
		SourceArgumentHost &operator=(const SourceArgumentHost &) = delete;
		SourceArgumentHost(SourceArgumentHost &&) noexcept = default;
		SourceArgumentHost &operator=(SourceArgumentHost &&) noexcept = default;
		Status Prepare(std::span<const AuthoredValue>, uint64_t maximumBytes, Diagnostic &);
		// Each assignment is name=value. Failure preserves the previous table.
		Status PrepareOptions(const SourceArgumentOptions &, uint64_t maximumBytes, Diagnostic &);
		bool Capture(const HostNodeInvocation &, HostNodeCapture &, std::string &failure) override;
		uint64_t RetainedBytes() const;

	  private:
		std::vector<AuthoredValue> Arguments;
	};
}
