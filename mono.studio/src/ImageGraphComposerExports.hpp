#pragma once
#include "ImageGraphObservations.hpp"

#include <studio/ImageGraph.hpp>
#include <vector>

namespace studio::detail {
	struct ImageGraphComposerExports {
		struct Observation {
			ImageGraphPlayback Playback;
			ImageGraphObservations Pcx;
			uint64_t Revision = 0, InputRevision = 0;
		};
		std::vector<Observation> Pending;
		static constexpr size_t MaximumObservations = 64;
		static constexpr uint64_t MaximumBytes = 1024 * 1024;
		void Invalidate(uint64_t revision, uint64_t inputRevision) {
			if (!Pending.empty() &&
				(Pending.front().Revision != revision || Pending.front().InputRevision != inputRevision))
				Pending.clear();
		}
		static std::optional<uint64_t> Bytes(const Observation &observation) {
			uint64_t bytes = sizeof(observation);
			const auto add = [&](uint64_t amount) {
				if (amount > MaximumBytes - bytes) return false;
				bytes += amount;
				return true;
			};
			if (!add(observation.Pcx.Project.capacity() + 1)) return {};
			for (const auto &value : observation.Pcx.Values) {
				const auto data = engine::imagegraph::ValueClonePayloadBytes(value.Data);
				if (!data || !add(value.Port.capacity() + 1) || !add(*data)) return {};
			}
			return bytes;
		}
		bool Admit(const Observation &observation, std::string &failure) try {
			Invalidate(observation.Revision, observation.InputRevision);
			if (!Pending.empty() &&
				GetImageGraphFrame(Pending.back().Playback) == GetImageGraphFrame(observation.Playback))
				return true;
			if (Pending.size() >= MaximumObservations) {
				failure = "Pending shader preview filled the automatic export "
						  "observation queue";
				return false;
			}
			const auto bytes = Bytes(observation);
			if (!bytes || Pending.capacity() > (MaximumBytes - sizeof(Pending)) / sizeof(Observation)) {
				failure = "Automatic export observation storage exceeds its byte budget";
				return false;
			}
			uint64_t held = sizeof(Pending) + Pending.capacity() * sizeof(Observation);
			for (const auto &entry : Pending) {
				const auto prior = Bytes(entry);
				if (!prior || *prior - sizeof(Observation) > MaximumBytes - held) {
					failure = "Retained automatic export observations exceed their byte budget";
					return false;
				}
				held += *prior - sizeof(Observation);
			}
			const uint64_t newBacking =
				Pending.capacity() < MaximumObservations ? MaximumObservations * sizeof(Observation) : 0;
			if (newBacking > MaximumBytes - held || *bytes > (MaximumBytes - held - newBacking) / 2) {
				failure = "Automatic export observations exceed their queue byte budget";
				return false;
			}
			Observation candidate = observation;
			const auto candidateBytes = Bytes(candidate);
			if (!candidateBytes || *candidateBytes > MaximumBytes - held - newBacking - *bytes) {
				failure = "Automatic export observation clone exceeds its byte budget";
				return false;
			}
			if (newBacking) {
				const auto oldBacking = Pending.capacity() * sizeof(Observation);
				Pending.reserve(MaximumObservations);
				if (Pending.capacity() > (MaximumBytes - held) / sizeof(Observation)) {
					failure = "Automatic export queue growth exceeds its byte budget";
					return false;
				}
				held += Pending.capacity() * sizeof(Observation) - oldBacking;
				if (*bytes > (MaximumBytes - held) / 2) {
					failure = "Automatic export queue capacity leaves no observation budget";
					return false;
				}
			}
			Pending.push_back(std::move(candidate));
			return true;
		} catch (const std::bad_alloc &) {
			failure = "Automatic export observation allocation was refused";
			return false;
		}
	};
} // namespace studio::detail
