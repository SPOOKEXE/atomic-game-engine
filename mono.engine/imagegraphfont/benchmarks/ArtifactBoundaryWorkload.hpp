#pragma once

#include "../tests/fixtures/FontHostBoundary.hpp"
#include "ArtifactFixture.hpp"

namespace engine::imagegraphfont::testing {
	struct ArtifactBoundaryWorkload {
		enum class Operation { Encode, Decode };
		Operation Kind;
		GraphFontConfiguration Source = font_boundary_fixture::MakeArtifactFixture();
		GraphFontConfiguration Decoded;
		std::string Encoded = "nonempty prior artifact", Failure;
		imagegraph::Diagnostic DiagnosticValue;
		uint64_t InputHash = 0;
		explicit ArtifactBoundaryWorkload(Operation kind) : Kind(kind) {
			if (!font_boundary_fixture::VerifyArtifactConfiguration(Source, Failure)) Fail();
			if (!WriteGraphFontConfiguration(
					Source, Encoded, font_boundary_fixture::ArtifactOperationBytes, DiagnosticValue
				))
				Fail();
			if (kind == Operation::Decode) {
				Decoded = font_boundary_fixture::MakeArtifactFixture();
				Decoded.Context.InitialFont->Data->Frames[0].Hash = 19;
			}
			InputHash = BoundaryHash(Encoded) ^ uint64_t(kind);
		}
		[[noreturn]] void Fail() const {
			throw std::runtime_error("artifact boundary: " + Failure + DiagnosticValue.Message);
		}
		bool Run(uint64_t maximum = font_boundary_fixture::ArtifactOperationBytes) {
			// Admit still-live fixture storage outside the codec's own source/previous/candidate ledger.
			const auto unrelated =
				GraphFontConfigurationRetainedBytes(Kind == Operation::Decode ? Source : Decoded);
			if (!unrelated || *unrelated >= maximum) return false;
			const uint64_t available = maximum - *unrelated;
			return Kind == Operation::Encode
					   ? WriteGraphFontConfiguration(Source, Encoded, available, DiagnosticValue)
					   : ReadGraphFontConfiguration(Encoded, Decoded, available, DiagnosticValue);
		}

		void VerifyCounters(const std::vector<core::Counter> &counters) const {
			const auto value = [&](std::string_view name) {
				for (const auto &counter : counters)
					if (counter.Name.Text() == name) return counter.Value;
				Fail();
			};
			if (Kind == Operation::Encode) {
				if (value("imagegraphfont.artifact.encoded_payload_bytes") != Encoded.size() ||
					value("imagegraphfont.artifact.encoded_backing_bytes") != Encoded.capacity() + 1)
					Fail();
			} else {
				const auto owned = GraphFontConfigurationRetainedBytes(Decoded);
				if (!owned || value("imagegraphfont.artifact.decode_input_bytes") != Encoded.size() ||
					value("imagegraphfont.artifact.decoded_retained_bytes") != *owned)
					Fail();
			}
		}
		uint64_t Verify() {
			if (!font_boundary_fixture::VerifyEncodedArtifact(Encoded, Failure) ||
				!font_boundary_fixture::VerifyArtifactConfiguration(Source, Failure))
				Fail();
			if (Kind == Operation::Decode &&
				!font_boundary_fixture::VerifyArtifactConfiguration(Decoded, Failure))
				Fail();
			return BoundaryHash(Encoded);
		}
	};
}
