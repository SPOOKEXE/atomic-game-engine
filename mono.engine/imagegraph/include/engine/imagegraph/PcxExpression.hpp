#pragma once
#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	struct PcxMessage {
		std::string Text;
		bool Warning = false;
	};
	class PcxNameResolver {
	  public:
		virtual ~PcxNameResolver() = default;
		virtual bool Resolve(std::string_view name, Value &value, Diagnostic &diagnostic) const = 0;
	};
	enum class PcxDrawBlend { SourceNormal, SourceAlphaAdd, Override };
	struct PcxExecutionContext {
		const EvaluationRequest &Request;
		std::span<const AuthoredValue> Parameters;
		const TimelineSettings *Timeline = nullptr;
		Vector2 ProjectDimension{32, 32};
		std::string_view ProjectName;
		Image *Target = nullptr;
		uint64_t MaximumWork = 1'000'000;
		uint64_t MaximumBytes = Limits::MaximumArrayBytes;
		const PcxNameResolver *Names = nullptr;
		PcxDrawBlend DrawBlend = PcxDrawBlend::SourceNormal;
	};
	struct PcxExecutionResult {
		Value Data = double{0};
		std::vector<PcxMessage> Messages;
	};
	// Compilation and execution keep the previous result unchanged on failure.
	Status CompilePcxExpression(std::string_view source, PcxExpressionValue &output, Diagnostic &diagnostic);
	Status CompilePcxProgram(std::string_view source, PcxExpressionValue &output, Diagnostic &diagnostic);
	Status ExecutePcxExpression(
		const PcxExpressionValue &expression,
		const PcxExecutionContext &context,
		PcxExecutionResult &output,
		Diagnostic &diagnostic
	);
}
