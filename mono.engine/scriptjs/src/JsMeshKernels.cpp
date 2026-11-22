// The `MeshKernels` global, in JavaScript.
//
// Half of a pair with `scriptluau/src/LuauMeshKernels.cpp`. A stream is an
// `ArrayBuffer` or a typed-array view (the bytes it covers), so a
// `Float32Array` an author fills is passed as it is. A wrong argument type
// throws; a stream too short for the count returns false and writes nothing.
//
// @tier L10 · shared

#include "JsBindings.hpp"

#include <engine/script/MeshKernels.hpp>

#include <cmath>
#include <iterator>
#include <optional>

namespace engine::script {

	namespace {
		namespace mk = mesh_kernels;

		// The bytes `value` covers, or `nullopt` with an exception pending.
		std::optional<mk::Bytes> Stream(JSContext *context, JSValueConst value) {
			size_t length = 0;
			if (JS_IsArrayBuffer(value)) {
				uint8_t *data = JS_GetArrayBuffer(context, &length, value);
				if (data == nullptr && length != 0) return std::nullopt;
				return mk::Bytes(reinterpret_cast<std::byte *>(data), length);
			}
			size_t offset = 0;
			size_t element = 0;
			JSValue backing = JS_GetTypedArrayBuffer(context, value, &offset, &length, &element);
			if (JS_IsException(backing)) {
				JS_FreeValue(context, JS_GetException(context));
				JS_ThrowTypeError(context, "expected an ArrayBuffer or a typed array");
				return std::nullopt;
			}
			size_t backingLength = 0;
			uint8_t *base = JS_GetArrayBuffer(context, &backingLength, backing);
			JS_FreeValue(context, backing);
			if (base == nullptr || offset + length > backingLength) {
				JS_ThrowTypeError(context, "expected an ArrayBuffer or a typed array");
				return std::nullopt;
			}
			return mk::Bytes(reinterpret_cast<std::byte *>(base + offset), length);
		}

		// Argument `index`, which may be past `argc`.
		JSValueConst Arg(int argc, JSValueConst *argv, int index) {
			return index < argc ? argv[index] : JS_UNDEFINED;
		}

		bool Count(JSContext *context, JSValueConst value, uint32_t &out) {
			double number = 0;
			if (!JS_IsNumber(value) || JS_ToFloat64(context, &number, value) != 0 || !(number >= 0.0) ||
				number > 4294967295.0 || std::floor(number) != number) {
				JS_ThrowTypeError(context, "expected a whole number from 0 to 2^32 - 1");
				return false;
			}
			out = static_cast<uint32_t>(number);
			return true;
		}

		bool Number(JSContext *context, JSValueConst value, float &out) {
			double number = 0;
			if (!JS_IsNumber(value) || JS_ToFloat64(context, &number, value) != 0) {
				JS_ThrowTypeError(context, "expected a number");
				return false;
			}
			out = static_cast<float>(number);
			return true;
		}

		// Reads every listed stream, or answers with the pending exception.
#define STREAM(name, index)                                                                                  \
	const std::optional<mk::Bytes> name = Stream(context, Arg(argc, argv, index));                           \
	if (!name) return JS_EXCEPTION
#define OPTIONAL_STREAM(name, index)                                                                         \
	std::optional<mk::ConstBytes> name;                                                                      \
	if (!JS_IsUndefined(Arg(argc, argv, index)) && !JS_IsNull(Arg(argc, argv, index))) {                     \
		const std::optional<mk::Bytes> name##Bytes = Stream(context, Arg(argc, argv, index));                \
		if (!name##Bytes) return JS_EXCEPTION;                                                               \
		name = *name##Bytes;                                                                                 \
	}
#define COUNT(name, index)                                                                                   \
	uint32_t name = 0;                                                                                       \
	if (!Count(context, Arg(argc, argv, index), name)) return JS_EXCEPTION
#define NUMBER(name, index)                                                                                  \
	float name = 0;                                                                                          \
	if (!Number(context, Arg(argc, argv, index), name)) return JS_EXCEPTION

		JSValue Axpy(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			STREAM(out, 0);
			NUMBER(a, 1);
			STREAM(x, 2);
			COUNT(count, 3);
			return JS_NewBool(context, mk::Axpy(*out, a, *x, count));
		}

		JSValue WeightedAxpy(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			STREAM(out, 0);
			STREAM(weights, 1);
			STREAM(x, 2);
			COUNT(count, 3);
			COUNT(stride, 4);
			return JS_NewBool(context, mk::WeightedAxpy(*out, *weights, *x, count, stride));
		}

		JSValue Transform(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			STREAM(out, 0);
			STREAM(input, 1);
			STREAM(matrices, 2);
			STREAM(indices, 3);
			STREAM(weights, 4);
			COUNT(count, 5);
			return JS_NewBool(context, mk::Transform(*out, *input, *matrices, *indices, *weights, count));
		}

		JSValue Normals(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			STREAM(out, 0);
			STREAM(positions, 1);
			STREAM(triangles, 2);
			OPTIONAL_STREAM(weld, 3);
			COUNT(count, 4);
			return JS_NewBool(context, mk::Normals(*out, *positions, *triangles, weld, count));
		}

		// `adjacency` is `[offsets, neighbours]` or `{offsets, neighbours}`.
		JSValue Laplacian(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			STREAM(out, 0);
			STREAM(positions, 1);
			const JSValueConst adjacency = Arg(argc, argv, 2);
			if (!JS_IsObject(adjacency)) return JS_ThrowTypeError(context, "expected [offsets, neighbours]");
			const bool array = JS_IsArray(adjacency);
			JSValue offsetsValue = array ? JS_GetPropertyUint32(context, adjacency, 0)
										 : JS_GetPropertyStr(context, adjacency, "offsets");
			JSValue neighboursValue = array ? JS_GetPropertyUint32(context, adjacency, 1)
											: JS_GetPropertyStr(context, adjacency, "neighbours");
			// Both stay owned by `adjacency`, so the spans outlive these values.
			const std::optional<mk::Bytes> offsets = Stream(context, offsetsValue);
			const std::optional<mk::Bytes> neighbours =
				offsets ? Stream(context, neighboursValue) : std::nullopt;
			JS_FreeValue(context, offsetsValue);
			JS_FreeValue(context, neighboursValue);
			if (!offsets || !neighbours) return JS_EXCEPTION;
			OPTIONAL_STREAM(mask, 3);
			COUNT(iterations, 4);
			NUMBER(amount, 5);
			return JS_NewBool(
				context, mk::Laplacian(*out, *positions, *offsets, *neighbours, mask, iterations, amount)
			);
		}

		JSValue Gather(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			STREAM(out, 0);
			STREAM(input, 1);
			STREAM(indices, 2);
			COUNT(width, 3);
			return JS_NewBool(context, mk::Gather(*out, *input, *indices, width));
		}

		JSValue Scatter(JSContext *context, JSValueConst, int argc, JSValueConst *argv) {
			STREAM(out, 0);
			STREAM(input, 1);
			STREAM(indices, 2);
			COUNT(width, 3);
			return JS_NewBool(context, mk::Scatter(*out, *input, *indices, width));
		}

#undef STREAM
#undef OPTIONAL_STREAM
#undef COUNT
#undef NUMBER

		const JSCFunctionListEntry KERNELS[] = {
			JS_CFUNC_DEF("Axpy", 4, Axpy),
			JS_CFUNC_DEF("WeightedAxpy", 5, WeightedAxpy),
			JS_CFUNC_DEF("Transform", 6, Transform),
			JS_CFUNC_DEF("Normals", 5, Normals),
			JS_CFUNC_DEF("Laplacian", 6, Laplacian),
			JS_CFUNC_DEF("Gather", 4, Gather),
			JS_CFUNC_DEF("Scatter", 4, Scatter),
		};
	}

	void InstallJsMeshKernels(JSContext *context, JSValueConst global) {
		JSValue table = JS_NewObject(context);
		JS_SetPropertyFunctionList(context, table, KERNELS, static_cast<int>(std::size(KERNELS)));
		JS_FreezeObject(context, table);
		JS_SetPropertyStr(context, global, "MeshKernels", table);
	}
}
