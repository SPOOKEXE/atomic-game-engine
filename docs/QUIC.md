# QUIC

**This is the *how*. `docs/DEFERRED.md` D00014 is the *whether* and the
*when*.** That entry holds the decision open behind a named trigger; this
document is what somebody reads on the day the trigger fires, so that the work
starts from a survey rather than from a search engine.

Everything below was checked against the libraries as they are in August 2026,
not as D00014 described them in v0.13. **Three of that entry's conclusions have
moved**, and they are called out where they occur.

---

## 1. What QUIC buys that this engine cannot give itself

Four arguments. The first three are the ones D00014 was filed on; the fourth is
the one that grew teeth when congestion control shipped in v0.15.

- **Per-stream loss recovery without head-of-line blocking across streams.**
  That is the shape `engine::net` arrived at by hand: structure reliable, values
  not. QUIC's is the same shape with the retransmission bookkeeping already
  written and interoperably tested.

- **TLS 1.3.** It subsumes the X25519 agreement in `net/Handshake.hpp`, the
  HKDF and ChaCha20-Poly1305 under `net::Cipher`, and the pinned-identity check
  `replication::ConnectorSettings::ServerIdentity` performs. One of those is a
  key exchange this repository maintains; TLS 1.3 is one the world does.

- **A delivery signal for unreliable traffic**, and this is the one the
  hand-rolled stack structurally cannot have. RFC 9221 DATAGRAM frames are
  *ack-eliciting*: they are never retransmitted, but their containing packet is
  acknowledged, so the sender learns whether each one arrived. `net`'s
  controller sees only the reliable channel's acknowledgements today, so a
  server publishing a still world - deltas unreliable, structure messages
  occasional - starves its own congestion controller of samples. D00014 names
  this gap and says closing it properly means a second acknowledgement path,
  which is QUIC.

- **One dependency answers two consumers.** The game link is one. `ROADMAP.md`'s
  cdn wire streaming is the other, and **HTTP/3 is QUIC**. Note that D00014
  recorded the cdn half as blocked on "`net` growing an `http/` sub-area" -
  **that has since happened**: `mono.engine/net/include/engine/net/http/` holds
  `Client.hpp`, `Message.hpp` and `Server.hpp`, and `delivery::Source` uses
  them. So the second consumer is no longer hypothetical, it is a shipped HTTP/1
  surface that HTTP/3 would sit beside.

**What we would not build and would get anyway:** connection migration, 0-RTT
resumption, path validation. None of these is a reason to adopt QUIC and all of
them arrive with it.

---

## 2. What is here now, and what QUIC replaces

`engine::net` is L2 `shared` and links `Engine::core`, `Vendor::asio` and
`Vendor::cryptopp`. Reading the table below as a delete list is premature: see
§8, which is explicit that nothing is deleted until the replacement is proved
beside it.

| Header | What it does | Under QUIC |
|---|---|---|
| `Transport.hpp` | UDP sockets, send and receive datagrams | **survives**, QUIC is a UDP payload |
| `Endpoint.hpp` | an address and a port | **survives** |
| `Packet.hpp` | our framing, sequence numbers, ack bitfield | replaced by QUIC packets and ACK frames |
| `Reliability.hpp` | `ReliableSender` / `ReliableReceiver` | replaced by QUIC streams |
| `Handshake.hpp` | X25519 key agreement, one message each way | replaced by TLS 1.3 in CRYPTO frames |
| `Cipher.hpp` | HKDF plus ChaCha20-Poly1305 sealing | replaced by TLS 1.3 key schedule |
| `Cookie.hpp` | stateless address-validation challenge | replaced by Retry and stateless-reset tokens |
| `ConnectionId.hpp` | our connection identity | replaced by QUIC connection ids |
| `Congestion.hpp` | Copa, plus a competitive mode | replaced by ngtcp2's (see §9) |
| `Link.hpp` | budget, round-trip estimate, loss observation | mostly replaced, `BytesPerTick` survives |
| `ConnectionStats.hpp` | counters a panel reads | **survives**, refilled from ngtcp2 |
| `LossyTransport.hpp` | a test harness that drops and reorders | **survives**, and is how QUIC gets tested |
| `http/` | HTTP/1 request and response | **survives**, gains an HTTP/3 sibling |

`Cookie.hpp`'s rule has to survive the swap and is worth restating because it is
easy to lose: *nothing is remembered per unanswered attempt*. The cookie is
derived from a secret this end holds plus bytes the peer already sent, and
verified by deriving it again. QUIC's Retry token must be implemented the same
way, or the amplification defence becomes the table it was written to avoid.

---

## 3. The library

### The recommendation is unchanged: ngtcp2

MIT, C, no TLS stack anywhere under its `lib/`, and every entry point takes an
explicit `ngtcp2_tstamp`. That last property is not a convenience, it is the
only shape compatible with this repository's standing rule that **time is passed
in, never read** - the rule `net`, `network`, `render::FlipbookFrameAt` and
`assets::Grant` all keep. A library that called `clock_gettime` internally would
put a non-deterministic input in the subsystem whose failures are hardest to
reproduce, and `just determinism` and `just replay-check` both depend on it not
doing that.

ngtcp2 is transport only. HTTP/3 is **nghttp3**, by the same authors, also MIT,
and it sits on top - which is how the cdn consumer would be served without a
second QUIC stack.

### What was considered and why it lost

| Library | Language | Licence | Why not |
|---|---|---|---|
| **ngtcp2** | C | MIT | **chosen** |
| msquic | C | MIT | Brings its own platform abstraction and event loop; TLS is Schannel on Windows and OpenSSL on Linux, so the TLS question below is answered *for* us and answered differently per platform. Larger surface than the transport we want. |
| quiche | Rust | BSD-2-Clause | Needs Rust 1.88+. A Rust toolchain is a bigger imposition on a fresh clone than any of the TLS build dependencies below, and this repository's build is CMake, Ninja and a C++ compiler. |
| lsquic | C | MIT | Requires BoringSSL, which requires Go. |
| picoquic | C | MIT | Hard-requires picotls by `find_package` or `FetchContent`; D00014's original reason and still true. |
| mvfst | C++ | MIT | Depends on folly. That is not a dependency, it is a second standard library. |
| quicly | C | MIT | picotls again. |
| OpenSSL's own QUIC | C | Apache-2.0 | It is a full stack rather than a library to build one on, and the measurements are poor: reported at up to three times slower than ngtcp2 with as much as twenty times the memory for the same transfer. |

---

## 4. The TLS backend, which is the one real decision

**This is where D00014 is most out of date.** It recorded three options and
named quictls as the interoperable one. Since then: **quictls' ngtcp2 helper is
deprecated**, **OpenSSL 3.5 gained a QUIC TLS API** so vanilla OpenSSL is now a
candidate at all, and **AWS-LC turns out to ship pre-generated build files
precisely so that projects which cannot take a Go or Perl dependency can still
build it**. That last one did not exist as an option when the entry was written
and may be the answer.

ngtcp2's crypto helpers, as of 1.24-DEV: `libngtcp2_crypto_ossl` (OpenSSL 3.5+,
experimental, needs ngtcp2 1.12.0+), `libngtcp2_crypto_boringssl` (**BoringSSL
and AWS-LC**), `libngtcp2_crypto_quictls` (deprecated),
`libngtcp2_crypto_libressl`, `libngtcp2_crypto_gnutls`,
`libngtcp2_crypto_picotls`, `libngtcp2_crypto_wolfssl`.

The constraint that decides this is not performance and not features. It is the
one `mono.vendor/AGENTS.md` and `THIRD_PARTY_NOTICES.md` both state: every
dependency is a **git submodule**, permissive, and **a fresh clone needs CMake,
Ninja and a C++ compiler and nothing else**.

| Backend | Licence | Build needs | Verdict |
|---|---|---|---|
| **AWS-LC** | Apache-2.0 OR ISC, plus some OpenSSL-licensed code | Go 1.20+ and Perl **normally**, but upstream ships generated files for projects that cannot take them | **The candidate to try first.** Permissive, CMake, BoringSSL-derived so it uses the mature `crypto_boringssl` helper rather than the experimental one. The generated-files path is the whole reason it clears the bar. |
| In-tree minimal TLS 1.3 | ours | nothing | **The fallback, and the only option with zero new build dependencies.** Over `D00006`'s primitives with RFC 7250 raw public keys. Smallest, and it serves neither HTTP/3 nor the cdn argument. |
| OpenSSL 3.5+ | Apache-2.0 | Perl to build from source, or a system package | Viable now, and was not before. Costs the fresh-clone property or introduces a system dependency. The helper is still marked experimental and the API has no 0-RTT, which we do not want anyway. |
| BoringSSL | ISC-ish | **Go** | Rejected: Go on every clone. |
| quictls | Apache-2.0 | Perl | Rejected: helper deprecated upstream. |
| LibreSSL | ISC / Apache-2.0 | Perl | Rejected: Perl. |
| GnuTLS | LGPL-2.1+ | autotools, nettle, gmp | Rejected: copyleft beyond MPL-2.0's file scope, and three more submodules. |
| wolfSSL | **GPLv2 or commercial** | - | Rejected on licence. This is a legal incompatibility with MPL-2.0 distribution, not a preference. |
| picotls | MIT | picotls-with-OpenSSL for ngtcp2's helper | Rejected. Its `minicrypto` backend (cifra plus micro-ecc) is the only OpenSSL-free configuration and **it can sign but cannot verify a peer's signature**, which is not a TLS client. |

**So the order to try is: AWS-LC with the generated files, and if that does not
hold, the in-tree TLS 1.3.** Both keep the licence clean. The first keeps
interoperability and HTTP/3; the second keeps the build story and gives them up.

Whichever is chosen, `THIRD_PARTY_NOTICES.md` gains a row and
`mono.vendor/AGENTS.md`'s rule is the one being argued against, so the argument
belongs in the commit rather than in a comment.

---

## 5. The crypto seam, which is where the work actually is

D00014 names three structural mismatches between `net::Cipher` and what a QUIC
crypto callback table needs. They are still the three, and none is a wrapper:

1. **QUIC owns the nonce.** `net::Sealer` holds its nonce privately and only
   ever moves it forward, which is the right design for a stack that owns its
   own packet numbers and the wrong one for a caller that computes the nonce
   from a packet number it chose.

2. **Header protection is a raw ChaCha20 keystream**, applied to the packet
   number field. This engine does not expose a raw keystream anywhere, and
   deliberately: `Cipher.hpp` hands out sealing and opening and nothing else.

3. **AES-128-GCM is mandatory** for Initial packets and Retry integrity whatever
   suite is negotiated afterwards. The engine's suite is ChaCha20-Poly1305
   throughout, so this is a primitive the build does not currently need.

The seam is therefore a new, small, private surface *beside* `net::Cipher`
rather than a widening of it. Widening `Cipher` would put a raw keystream and a
caller-chosen nonce in a public header, which is exactly the shape its own
comments refuse.

---

## 6. Mapping the engine's model onto QUIC

- **`ChannelKind::Reliable`** becomes a QUIC stream. One bidirectional stream
  per channel is the obvious mapping and is probably right: streams are cheap
  and per-stream ordering is what the channel means.
- **Unreliable channels** become DATAGRAM frames (RFC 9221). They are not
  retransmitted, they share the connection's congestion controller, and they
  carry no flow control - which matches what an unreliable channel is for and
  is why the extension exists.
- **`Handshake`** becomes TLS 1.3 in CRYPTO frames. `D00006`'s pinned server
  identity becomes either a raw public key (RFC 7250) or a certificate check,
  and **the pinning must survive**: `ConnectorSettings::ServerIdentity` exists
  because an unauthenticated agreement is safe against a listener and not
  against a relay.
- **`Cookie`** becomes Retry plus stateless-reset tokens, keeping the
  derive-do-not-remember rule from §2.
- **`ConnectionId`** becomes QUIC connection ids, which are longer, negotiable
  and rotatable. `mono.network`'s discovery carries ours today and would carry
  theirs.
- **`Link::BytesPerTick`** survives as a **hard ceiling above** ngtcp2's
  controller. D00014 is right that these answer different questions: a game may
  refuse to spend more than N on one player on a path that would carry ten
  times that, because a hundred players on one host is a hundred of these and
  the operator's bill is not a function of what the path can take.

---

## 7. The clock, which is not negotiable

Every ngtcp2 entry point takes an `ngtcp2_tstamp`. Pass the tick's time in, the
same way `replication::Session` already passes it to `Link`. The expiry timer is
driven from the tick through `ngtcp2_conn_get_expiry` and
`ngtcp2_conn_handle_expiry` rather than from a timer thread.

Two properties depend on this and both are checked by recipes:
`just determinism` and `just replay-check`. Today they are unaffected by network
state twice over - by construction, and because `mono.server` does not call
`ServeClients` on the replay path at all. **Whoever does this work must keep
both of those true and should check the second one has not quietly changed.**

---

## 8. Staging, which is not a preference

**Two overlapping reliability stacks is worse than either.** The order is:

1. Vendor ngtcp2 and the chosen TLS backend. `MonoVendor.cmake` target,
   `THIRD_PARTY_NOTICES.md` row, `.gitmodules` entry.
2. Build the crypto seam (§5) with its own suite. This is the part most likely
   to be subtly wrong and it is testable without a socket.
3. Stand a QUIC session up **beside** the existing one, behind a flag, and prove
   it over `net::LossyTransport` - which is already the harness that drops and
   reorders, and is how the hand-rolled stack was proved.
4. Rewire `replication::Session`, `Listener` and `Connector`, then
   `mono.server`, `mono.client`, `mono.studio`,
   `mono.unified_server_client`, and `mono.network`'s discovery.
5. Update `mono.tools/architecture/expected_graph.json` and let the tier check
   pass.
6. Only then delete, with the suites and benchmarks that go with each deletion.

Every commit green. Not a sweep that leaves the tree with no working link.

---

## 9. What gets deleted, and the one that should not be

On the delete list when the rewiring lands: `Packet`, `Reliability`,
`Handshake`, `Cipher`'s QUIC-shaped parts, `Cookie`, `ConnectionId`.

**`net::CongestionControl` is on that list and deserves a second look before it
goes.** ngtcp2 carries Reno, Cubic and BBR, so keeping ours is redundant on the
face of it. But what is there is Copa with a measured competitive mode, and
D00014 records that the mode-switch predicate was got wrong twice and fixed
against measurements - a solo 250 kB/s path latching into competitive mode and
ratcheting its own standing queue from 9 ms to 190 ms. That is a hundred and
eighty lines carrying a real finding, and a delay-based controller tuned for
input latency is not the same trade as Cubic. **Deleting it is a decision to
take Cubic's latency, and should be made deliberately rather than as
housekeeping.** ngtcp2 does allow a custom congestion controller.

---

## 10. What will bite

- **The crypto seam, not the transport.** ngtcp2 is well-trodden; the three
  mismatches in §5 are where a subtle bug lives, and a wrong nonce or a wrong
  header-protection mask fails as "the handshake does not complete" with nothing
  saying why.
- **AWS-LC's generated-files path is a claim to verify early**, on all three
  target platforms, before anything is built on it. If it does not hold, the
  decision reverts to the in-tree TLS 1.3 and the scope changes materially.
- **The `ossl` helper is experimental** and OpenSSL's QUIC API is reported as
  slower and much hungrier than the alternatives. Do not pick it for
  convenience.
- **Interop is a feature you have to want.** The in-tree TLS 1.3 option gives up
  talking to anything that is not us, which also gives up HTTP/3 and therefore
  the second-consumer argument that justifies the dependency.
- **Determinism.** See §7.

---

## 11. Open questions

- Does AWS-LC's pre-generated build actually configure and link with no Go and
  no Perl, on Linux, Windows and macOS, as a submodule?
- One stream per channel, or one stream per message? Streams are cheap but not
  free, and the answer depends on how many channels a real world uses.
- Does the Copa controller stay (§9)?
- Does `mono.network`'s discovery carry a QUIC connection id, or keep its own
  identity and hand one over at connect time?

---

## Sources

- [ngtcp2](https://github.com/ngtcp2/ngtcp2) and the
  [programmers' guide](https://nghttp2.org/ngtcp2/programmers-guide.html)
- [RFC 9221, An Unreliable Datagram Extension to QUIC](https://datatracker.ietf.org/doc/html/rfc9221)
- [OpenSSL 3.5.0 release discussion](https://github.com/openssl/openssl/discussions/27232)
- [OpenSSL does a QUIC API, daniel.haxx.se](https://daniel.haxx.se/blog/2025/02/16/openssl-does-a-quic-api/)
- [HTTP/3 with curl](https://curl.se/docs/http3.html)
- [AWS-LC BUILDING.md](https://github.com/aws/aws-lc/blob/main/BUILDING.md) and
  [INCORPORATING.md](https://github.com/aws/aws-lc/blob/main/INCORPORATING.md)
- [picotls](https://github.com/h2o/picotls)
- [msquic](https://github.com/microsoft/msquic) and its
  [BUILD.md](https://github.com/microsoft/msquic/blob/main/docs/BUILD.md)
- [cloudflare/quiche](https://github.com/cloudflare/quiche)
- [picoquic building notes](https://github.com/private-octopus/picoquic/blob/master/doc/building_picoquic.md)
