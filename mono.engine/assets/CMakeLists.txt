# L8. What a game is made of, as far as the bytes are concerned.
#
# Content addressing, chunking, the hash tree and the manifest - the format both
# halves of content delivery share. The running game uses these to fetch and
# verify; `mono.cdn` uses the same functions to build, sign and serve. Writing
# the format twice is how a format acquires a dialect, and the bug that produces
# is a client rejecting content the origin considers valid.
#
# **This module deliberately does not depend on `scene`.** That is the whole
# reason the content origin can link it alone and remain a program with no
# simulation in it.
#
# Both vendors are VENDOR rather than VENDOR_PUBLIC, so neither `blake3.h` nor
# any Crypto++ header reaches a public header and no consumer of this module
# inherits one. `ContentHash`, `PublicKey` and `SignatureBytes` are plain byte
# arrays of our own and say nothing about what produced them.
#
# Crypto++ supplies Ed25519 - through its flat `Donna` API rather than the
# PK_Signer machinery, because one signature per published manifest needs none
# of that - and the constant-time compare. BLAKE3 supplies the content hash.
mono_add_library(assets
	TIER shared
	DEPS
		Engine::core
	VENDOR
		Vendor::blake3
		Vendor::cryptopp
)
