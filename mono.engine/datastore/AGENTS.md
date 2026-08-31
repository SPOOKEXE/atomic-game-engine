# datastore - module invariants

L12 shared. Host-side persistence providers above `world` and `net`.

## Providers adapt copied snapshots

The live DataStore remains in `world`. This module receives and returns complete
copied images through `world::DataStoreAdapter`; it owns no second key/value
table and no ECS state.

## A provider never runs inside a world tick

The adapter API completes a load or save before returning. Network-backed
providers may pump a bounded transport operation, so hosts call them only at
startup, shutdown, or a barrier outside simulation. Do not expose these calls
to a system or script callback directly.

## Remote operations are bounded and replace atomically

Every request has a finite pump budget and every response is bounded before it
is decoded. A failed or malformed load leaves the caller's entries unchanged.
Saving sends one complete portable shared-store image, so a provider endpoint
must replace the named object atomically.

## HTTP means HTTP

The built-in transport is the engine's current plain HTTP client. Do not label
it HTTPS, Supabase, MongoDB, or SQL. Those names require their real TLS,
authentication, and provider protocol implementations.
