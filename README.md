# alpmrpc

Exposes an MSYS2-hosted **libalpm** to native mingw/clang/ucrt64 programs.

libalpm only builds under MSYS2, because it needs POSIX facilities Cygwin
emulates and mingw does not (`fork`/`exec` for scriptlets and hooks,
`statvfs`, `pwd.h`, real symlinks and modes). Rather than fork libalpm and
stub those out, the real library keeps running inside MSYS2 and native
callers reach it over a pipe.

    ucrt64 app ──> libalpm-14.dll ──named pipe──> alpmrpcd (MSYS2) ──> libalpm

The client DLL is a drop-in shim exporting the `alpm_*` C ABI. It imports
nothing but kernel32, advapi32 and the UCRT — no MSYS2 DLL is ever loaded
into the calling process.

## Building

    cmake -B build -G Ninja -DMSYS2_ROOT=C:/msys64
    cmake --build build

Configure with a mingw/clang cmake (ucrt64, mingw64, clang64). The superbuild
drives the MSYS2 compiler for the server through a toolchain file, so
`msys/cmake` is not needed.

## Generated, not written

The wire surface is generated from the *installed* `alpm.h`, so it cannot
drift from the library the server links against.

    alpm.h ──libclang──> api_model.json ──┬──> arpc_dispatch.c   (server)
                                          ├──> arpc_stubs.c      (client)
              tools/gen/overlay.json ─────┴──> arpc_handle_tags.h

`tools/gen/overlay.json` is the only hand-maintained input: it records what a
C header cannot express — pointer direction and list ownership. String
ownership is *not* in it, because libalpm's const-ness already determines it
(`char *` is caller-owned, `const char *` is borrowed); `emit.py` asserts that
invariant against each header it parses.

`cmake --build build --target coverage` prints what is generated and, for
everything else, why it is not.

## Design notes

- **No registration.** The endpoint name is derived from the MSYS2 root, the
  protocol version and the user's SID. The client starts the server on first
  use and drops the pipe when the caller's last libalpm handle is released;
  the server exits after an idle period. Nothing persists anywhere.
- **Handles are ids, not pointers.** An `alpm_db_t *` on the client is the
  server's handle id cast to a pointer and is never dereferenced. Ids are
  monotonic and type-tagged, so a stale or wrong-typed id fails a lookup
  instead of reaching libalpm as a bad pointer.
- **Lists are real.** `alpm_list_t` is transparent -- callers walk `->next`
  and call `alpm_list_count`/`alpm_list_free` directly -- so it cannot be
  proxied. The server serialises a whole list into one array and the client
  materialises a genuine chain, linking the real `alpm_list.c` (vendored, see
  `src/client/vendor/`). A list therefore costs one round trip, not one per
  element. Transparent element structs are materialised field by field,
  recursively: `alpm_group_t` arrives with its `packages` list attached.
- **Package fields are fetched a column at a time.** Reading eight fields of
  1150 packages one accessor call at a time is 9200 round trips. A
  materialised package list registers itself as a group, and the first read
  of any field fetches that field for the whole group in one call, so the
  1149 reads that follow are memory reads (~50ns). Fields nobody touches are
  never fetched. `alpm_pkg_get_name()` is unchanged from the caller's side --
  this is not a new API, it is the same one not going to the wire.
  Which accessors are cheap enough to batch is an overlay rule: the excluded
  ones hash a package file or compute a download size.
- **Structs cross in both directions.** The server writes a record and the
  client materialises it; the client writes one and the server rebuilds a
  temporary for the duration of the call. As with lists, `const` marks the
  read-only inputs, and a non-const struct param is skipped rather than
  guessed at.
- **A counted array is a record shape, not a struct pointer.**
  `alpm_filelist_t` is a `count` and a pointer to *n* files, which in the
  header looks exactly like a pointer to one — so materialising it field by
  field would silently produce a list of length one. Which field is the
  count and which is the array is an overlay entry; the array then crosses
  as an array, and the elements land in a single allocation because that is
  what they are. `alpm_siglist_t` is the same shape and still refused, for a
  different reason: its element embeds an `alpm_pgpkey_t` whose `data` is
  the gpgme key itself.
- **`alpm_dep_free` and friends never reach the server.** They free a struct
  this client materialised; the server has libalpm's own copy, which is not
  ours to free. The generated stub calls the matching free helper locally.
  `alpm_filelist_contains` stays here for the mirror reason: it must return a
  pointer *into* the caller's own filelist, and a round trip would return one
  into libalpm's copy. It is libalpm's own `bsearch`, in
  `src/client/arpc_local.c`.
- **Callbacks run nested inside the call that triggered them.** libalpm
  calls back from the middle of an operation, and a question has to be
  answered before the transaction that asked it can continue. So the server
  installs its own trampoline with libalpm, sends the arguments up the same
  pipe, and blocks; the client's frame loop delivers it to the caller's
  function pointer and replies before its own call returns. That ordering is
  not an optimisation -- it is the only shape a synchronous callback can
  take -- and it keeps the server single-threaded, which is what keeps
  libalpm's `fork()` on the path Cygwin supports.
- **And a callback may call back.** pacman's own conflict prompt asks
  libalpm the names of the two packages before it can phrase the question,
  so a request can arrive while the server is blocked waiting for an answer.
  It serves those and keeps waiting, which is the same loop the client runs
  in the other direction. The nesting is strict, so each side's reply is
  simply the next frame that is not a fresh request.
- **`...` is formatted on whichever side has the arguments.** The wire cannot
  carry a `va_list` in either direction, so neither side tries. `logcb` fires
  on the server, so the server formats it and sends the text;
  `alpm_logaction` is called on the client, so the client formats it and
  sends the text. The receiving side then passes that text as an argument to
  a literal `"%s"` — never as the format, or a `%` that came out of the
  formatting would be read as a conversion.
- **Paths are the server's.** They cross unchanged in both directions, so a
  caller passes Cygwin paths to `alpm_initialize` and gets them back from
  `alpm_option_get_root`. `fetchcb` is where this becomes visible rather than
  academic: the `localpath` libalpm wants a file written to is a path in the
  server's filesystem, and a native caller has to be able to reach it. No
  translation happens here, because guessing at one would be worse than the
  caller doing it knowingly.
- **Borrowed lists are cached against their owner** and looked up *before*
  the call, because libalpm hands back the same pointer for repeated calls
  and a caller may still be holding an earlier one. Caller-owned lists are
  built fresh and freed by the caller with the usual idiom.
- **The server is single-threaded**, which keeps libalpm's `fork()` for
  scriptlets and hooks on the path Cygwin actually supports.
- `ALPMRPC_TRACE=1` dumps every frame. `alpmrpcd --stdio` runs the dispatch
  layer against stdin/stdout with no IPC at all.
- **Rebuilding while a server is up.** Windows will not overwrite a running
  executable, so installing asks any server on this endpoint to exit first
  (`alpmrpcd --stop`); the next client starts a fresh one on demand. Renaming
  the locked file aside would have been the easier fix and a worse one -- the
  old server would keep its endpoint and later clients would keep reaching
  it, so tests would quietly run against stale code. If a client is still
  connected the server cannot be retired, and the build says so rather than
  installing something that will not be used.

## Tests

`ctest` from the client build directory runs codec round-trip tests and the
end-to-end tests (which launch a server on demand). The transaction test
installs packages for real, so it runs against a throwaway root that
`tests/scratch/mkroot.sh` rebuilds before every run -- two packages that
conflict by name, each with a scriptlet, and a hook. That root carries its
own `/bin/sh`, because scriptlets and hooks are `chroot`ed into it.

`bench_codec` reports round-trip cost, the codec's share of it, and
throughput on a bulk payload -- run it before and after anything that
touches the wire. `bench_transaction` does the same for the write path; it
rebuilds the fixture itself, so it is not part of `ctest`.

## Status

162 of 193 functions are generated, plus 19 written by hand — the eighteen
callback setters, getters and ctx getters, and `alpm_filelist_contains`.
`coverage.json` lists everything else with a reason for each; what is left is
small and specific — the signature buffers, `alpm_siglist_t`'s embedded gpgme
key, and the opaque cursors (changelog, mtree).

All six callbacks are carried — `logcb`, `progresscb`, `eventcb`,
`questioncb`, `dlcb`, `fetchcb` — and every one of them has been seen to
fire. `fetchcb` is the odd one: registering it takes downloading away from
libalpm and asks the caller to do it, so it is the only callback where the
client does work rather than watching it.

Both paths work and are measured. A real transaction has been driven end to
end against the throwaway root, which settled the two things that were
previously written but unproven:

- **A question fires, is answered, and the answer reaches libalpm.** An
  `ALPM_QUESTION_CONFLICT_PKG` arrives with both packages as usable handles
  — the callback calls `alpm_pkg_get_name` on them, which is a request
  travelling *into* a server that is blocked waiting for that same
  callback's answer — and saying yes is what makes libalpm remove one
  package and install the other.
- **libalpm forks and execs a shell, for a scriptlet and again for a hook,
  and the bridge survives it.** That is what the single-threaded server was
  arranged for. The test checks it against the files the shell left inside
  the root rather than against anything libalpm reported.

What it costs: the callback traffic of a two-package transaction is 46
frames and 0.7 ms of a 1.8 s commit, 0.04%. A transaction is `fork`, `exec`
and disk; the bridge is not what it is waiting for. Codecs are therefore
still not worth revisiting — the read path said the codec is 2% of a call,
and the write path does not disagree.
