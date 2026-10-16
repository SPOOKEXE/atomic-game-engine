#include <engine/computegraph/Document.hpp>

namespace engine::computegraph {
	const char *Describe(Status status) {
		switch (status) {
		case Status::Ok:
			return "ok";
		case Status::LimitExceeded:
			return "limit exceeded";
		case Status::InvalidName:
			return "invalid name";
		case Status::DuplicateName:
			return "duplicate name";
		case Status::UnknownBuffer:
			return "unknown buffer";
		case Status::UnknownKernel:
			return "unknown kernel";
		case Status::InvalidType:
			return "invalid element layout";
		case Status::InvalidCount:
			return "invalid element count";
		case Status::AccessDenied:
			return "buffer access denied";
		case Status::AliasConflict:
			return "buffer alias conflict";
		case Status::ReflectionMismatch:
			return "shader reflection mismatch";
		case Status::InvalidDispatch:
			return "invalid dispatch";
		case Status::MissingProducer:
			return "missing buffer producer";
		case Status::MultipleProducers:
			return "multiple buffer producers";
		case Status::Cycle:
			return "dependency cycle";
		case Status::OutputOverflow:
			return "output capacity exceeded";
		case Status::StaleOwner:
			return "stale request owner";
		case Status::UnknownTicket:
			return "unknown request ticket";
		case Status::NotReady:
			return "request not complete";
		case Status::AllocationFailed:
			return "allocation failed";
		}
		return "invalid status";
	}
	const char *Describe(ElementType type) {
		switch (type) {
		case ElementType::U32:
			return "u32";
		case ElementType::F32:
			return "f32";
		case ElementType::F32x4:
			return "f32x4";
		case ElementType::EngineInstance:
			return "engineInstanceBuffer";
		}
		return "invalid";
	}
	uint32_t ElementStride(ElementType type) {
		switch (type) {
		case ElementType::U32:
		case ElementType::F32:
			return 4;
		case ElementType::F32x4:
			return 16;
		case ElementType::EngineInstance:
			return 0;
		}
		return 0;
	}
}
