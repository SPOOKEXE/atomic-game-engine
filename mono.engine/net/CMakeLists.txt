# L11. The wire between two machines, and who is authoritative across it.
#
# repo_layout.md §5.2 splits this module four ways: `upstream/` is client to
# server and every field of it is hostile until validated, `downstream/` is
# server to clients and authoritative at its source, `predict/` is the client's
# run-ahead and its reconciliation, and `http/`/`websocket/` is userland
# networking plus the server's own asset serving.
#
# **This is the transport underneath all four.** Connection lifecycle, framing
# and channels — what v0.3's first roadmap item asks for. Nothing here knows
# what a component is, and it must not: replication is a *reader* of this, one
# layer up, and a transport that knows about entities cannot also carry a
# script's remote call or a group of content bytes.
#
# `shared` tier, because both halves run the same code. The server encodes and
# the client decodes and then they swap — repo_layout.md §16.6 goes further and
# has single-player ride a loopback with **real encoding**, so there is no
# configuration in which this path is skipped.
#
# asio is VENDOR rather than VENDOR_PUBLIC: no public header here names a
# socket, an io_context or an error_code. That is what keeps a transport
# swappable and what stops asio's headers reaching every module that links this.
mono_add_library(net
	TIER shared
	DEPS
		Engine::core
	VENDOR
		Vendor::asio
)
