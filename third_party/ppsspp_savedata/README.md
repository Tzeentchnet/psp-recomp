# PPSSPP savedata crypto subset

This directory contains the savedata encryption primitives used for PSP/PPSSPP-compatible
`sceUtilitySavedata` files.

- **Origin:** PPSSPP v1.20.4, commit `fa50bb1`
- **PPSSPP files:** `Core/HLE/sceChnnlsv.cpp` and `sceChnnlsv.h`
- **libkirk files:** `ext/libkirk/{AES,SHA1,bn,ec,kirk_engine}.*`
- **Local changes:** removed PPSSPP memory/HLE registration wrappers, retained only the
  Chnnlsv MAC/cipher operations, and added the small C ABI in
  `src/rt/savedata_crypto.cpp`.

The Chnnlsv subset is GPL-2.0-or-later as part of PPSSPP. libkirk is
GPL-3.0-or-later; see `COPYING.GPLv3` and the source headers. Consequently,
binaries linking this savedata crypto must be distributed under
GPL-3.0-or-later-compatible terms.
