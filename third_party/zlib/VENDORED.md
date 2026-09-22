# Vendored zlib

zlib 1.3.2, the compression core only (no gzip file API, no examples or
build files). Unmodified. gzguts.h is included only because zutil.c needs it.

- Source: https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
- SHA-256: bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
  (identical to the zlib.net copy; signature verified against Mark Adler's key
  5ED46A6721D365587791E2AA783FCD8E58BCAFBA)
- License: zlib license, see LICENSE. Compatible with this project's GPL.

Vendored rather than taken from the system so both ends run the same deflate
code, and because the Win32 agent is built with Open Watcom, which has no
system zlib. It is compiled with its own relaxed flags and excluded from this
project's warnings, analysis and coverage gates.
