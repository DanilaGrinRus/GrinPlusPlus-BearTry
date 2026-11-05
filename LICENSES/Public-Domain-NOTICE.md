# Public-domain components — exact statements

Several distributed components are placed in the public domain. There is no license
text to reproduce for these components; the exact statements from their source files
are quoted below. Their copyright/attribution lines are also listed in
[`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

## ChaCha20 — `deps/chachapoly/chacha.h`

```
chacha-merged.c version 20080118
D. J. Bernstein
Public domain.
```

## Poly1305 — `deps/chachapoly/poly1305.cpp`

```
poly1305 implementation using 32 bit * 32 bit = 64 bit multiplication and 64 bit addition
public domain
```

(The Poly1305 code is the poly1305-donna implementation, as stated in
`deps/chachapoly/README.md`; the AEAD wrapper around it, `chachapoly.cpp`/`.h`, is a
separate MIT-licensed work by Grigori Goronzy — see `MIT.txt` and the notices file.)

## SQLite 3.28.0 — `deps/libsqlite3/sqlite3.h` (and `sqlite3.c`)

```
The author disclaims copyright to this source code.  In place of
a legal notice, here is a blessing:

   May you do good and not evil.
   May you find forgiveness for yourself and forgive others.
   May you share freely, never taking more than you give.
```

## JsonCpp — `deps/jsoncpp/json/json.h`

JsonCpp is offered under a dual Public Domain / MIT arrangement. The embedded
statement says, in part:

```
Baptiste Lepilleur and The JsonCpp Authors explicitly disclaim copyright in all
jurisdictions which recognize such a disclaimer. In such jurisdictions,
this software is released into the Public Domain.

In jurisdictions which do not recognize Public Domain property (e.g. Germany as of
2010), this software is Copyright (c) 2007-2010 by Baptiste Lepilleur and
The JsonCpp Authors, and is released under the terms of the MIT License (see below).
```

The full embedded text (including the MIT grant) is in `deps/jsoncpp/json/json.h`;
the MIT text is also available as `MIT.txt`.

## portable_endian.h — `src/PoW/portable_endian.h`

```
// "License": Public Domain
// I, Mathias Panzenböck, place this file hereby into the public domain. Use it at your own risk for whatever you like.
// In case there are jurisdictions that don't support putting things in the public domain you can also consider it to
// be "dual licensed" under the BSD, MIT and Apache licenses, if you want to. This code is trivial anyway. Consider it
// an example on how to get the endian conversion functions on different platforms.
```

The source file contains a legacy encoding artifact in the author's surname; the
statement is otherwise reproduced as published. If a recipient does not accept the
public-domain dedication, the stated fallback is a choice of the BSD, MIT or Apache
license; the corresponding texts are `BSD-2-Clause.txt`, `MIT.txt` and
`Apache-2.0.txt`.
