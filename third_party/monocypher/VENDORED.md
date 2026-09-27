# Vendored Monocypher

Monocypher 4.0.3, the library itself only: `src/monocypher.c` and
`src/monocypher.h`. Not the optional Ed25519/SHA-512 files, the tests, the
manual or the build files. Unmodified.

- Source: https://github.com/LoupVaillant/Monocypher/releases/download/4.0.3/monocypher-4.0.3.tar.gz
- SHA-256: 8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66
  (identical to https://monocypher.org/download/monocypher-4.0.3.tar.gz; its
  SHA-512 matches the one published there,
  40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965)
- License: dual 2-clause BSD or CC0 1.0, at the user's choice; see LICENCE.md
  (the tarball's spelling). Either is compatible with this project's GPL.

Used for X25519 and for the two primitives of ChaCha20-Poly1305 under the TCP
link's Noise handshake (`src/core/noise.c`). Monocypher's own AEAD functions
are not used: they take an XChaCha20 nonce or ratchet the key after each
message, and Noise's ChaChaPoly is the plain RFC 8439 AEAD, which noise.c
assembles from `crypto_chacha20_ietf` and `crypto_poly1305`.

Vendored rather than taken from the system for the same reasons as zlib: both
ends run the same code, and the Win32 agent is built with Open Watcom, which
has no system copy. It compiles under Watcom's C99 mode without flags or
changes. Like zlib it is built with its own relaxed flags and is excluded from
this project's warnings, analysis and coverage gates.

`monocypher.c` carries one non-ASCII character (an em-dash in a comment at
line 687). It is left as shipped, so every file here stays byte for byte the
release's:

```
57eb914fc88136119bd41655cccb8c250048bf54d470540625186f8ab16f64be  monocypher.c
c494da712122da7ff679fdcf318a5317e84972b6c950fe9d896212947797facd  monocypher.h
5f8360e4c06ddcc584bdb4b210c6af824c4bb301e6a9a521869b6d90795ca4b3  LICENCE.md
```
