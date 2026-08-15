#pragma once

// The field layouts two wire formats in this module share.
//
// An advert and a rendezvous message both carry endpoints and session ids, and
// both are read from datagrams a stranger wrote. Writing the layout twice is
// how a format acquires a dialect - one side gains a field, the other keeps
// reading the old shape, and the mismatch shows up as "discovery works on the
// LAN and not through the point".
//
// Private, in `src/`, because nothing outside this module has a reason to
// encode either format by hand. A module's own tests may reach here; that is
// what the private include directory is for.

#include <engine/core/Bytes.hpp>
#include <engine/net/Endpoint.hpp>

#include <cstddef>
#include <cstdint>
#include <network/Advert.hpp>

namespace network {

	// An endpoint on a wire: family, sixteen address bytes, port.
	//
	// Fixed width rather than "four bytes for v4, sixteen for v6". A layout
	// whose length depends on a field it has already read is one where a
	// hostile family byte moves every subsequent field, and the reader would
	// have to be right about that before it had checked anything.
	constexpr size_t ENDPOINT_BYTES = 1 + 16 + 2;

	// Writes an endpoint.
	//
	// @param writer Where to write.
	// @param value  The endpoint. An invalid one writes as family `None` and
	//        reads back as an invalid one.
	void WriteEndpoint(engine::core::ByteWriter &writer, const engine::net::Endpoint &value);

	// Reads what WriteEndpoint wrote.
	//
	// A family byte outside the list yields an invalid endpoint rather than a
	// failure: an endpoint that means nothing is a field a caller can refuse on
	// its own terms, and refusing the whole datagram would let a future version
	// that added a family break every reader that came before it.
	//
	// @param reader Where to read from.
	// @return The endpoint, or an invalid one.
	engine::net::Endpoint ReadEndpoint(engine::core::ByteReader &reader);

	// Writes a session id.
	//
	// @param writer Where to write.
	// @param value  The id.
	void WriteSessionId(engine::core::ByteWriter &writer, const SessionId &value);

	// Reads a session id.
	//
	// @param reader Where to read from.
	// @return The id, or the null one when the read did not fit.
	SessionId ReadSessionId(engine::core::ByteReader &reader);
}
