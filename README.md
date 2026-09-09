# alpmrpc

Exposes an MSYS2-hosted **libalpm** to native mingw/clang/ucrt64 programs.

libalpm only builds under MSYS2, because it needs POSIX facilities Cygwin
emulates and mingw does not (`fork`/`exec` for scriptlets and hooks,
`statvfs`, `pwd.h`, real symlinks and modes). Rather than fork libalpm and
stub those out, the real library keeps running inside MSYS2 and native
callers reach it over a pipe.

    ucrt64 app ──> libalpm-14.dll ──named pipe──> alpmrpcd (MSYS2) ──> libalpm

The client DLL is a drop-in shim exporting the `alpm_*` C ABI. The rule it
keeps is that **no MSYS2 DLL is ever loaded into the calling process** — the
only contact with MSYS2 is the pipe. Ordinary mingw/ucrt64 libraries are
fine, and it links libarchive for the same reason a caller would. That the
import list stays short is a consequence of the architecture, not a target
it is being held to.

## Building

    cmake -B build -G Ninja -DMSYS2_ROOT=C:/msys64
    cmake --build build

Configure with a mingw/clang cmake (ucrt64, mingw64, clang64). The superbuild
drives the MSYS2 compiler for the server through a toolchain file, so
`msys/cmake` is not needed. The client's caches are uthash tables, so the
toolchain's uthash package has to be there -- one header, no runtime
dependency:

    pacman -S --asdeps mingw-w64-x86_64-uthash

## Generated, not written

The wire surface is generated from the *installed* `alpm.h`, so it cannot
drift from the library the server links against.

    alpm.h ──libclang──> api_model.json ──┬──> arpc_dispatch.c   (server)
                                          ├──> arpc_stubs.c      (client)
                                          ├──> alpm.def          (client's exports)
              tools/gen/overlay.json ─────┴──> arpc_handle_tags.h

`tools/gen/overlay.json` is the only hand-maintained input: it records what a
C header cannot express — pointer direction, list ownership, which union
member a callback's tag selects, which strings are paths. String ownership is
*not* in it, because
libalpm's const-ness already determines it (`char *` is caller-owned,
`const char *` is borrowed); `emit.py` asserts that invariant against each
header it parses, and checks the tag mappings against the enums the same way.

The DLL's export table comes from the same model: exactly what `alpm.h`
declares, plus the list API beside it, and nothing of the bridge's own. A
function the generator did not produce and nobody wrote by hand then fails
the link, by name, rather than going quietly missing from the DLL.

`cmake --build build --target coverage` prints what is generated and, for
everything else, why it is not.

## Design notes

- **No registration.** The endpoint name is derived from the MSYS2 root, the
  protocol version and the user's identity. The client starts the server on
  first use and drops the pipe when the caller's last libalpm handle is
  released; the server exits after an idle period. Nothing persists
  anywhere. A server serves one client at a time, so two clients at once
  mean two servers, each with its own instance of the pipe; `alpmrpcd
  --stop` retires all of them.
- **The endpoint is one user's, at one elevation, and both ends check.** A
  bridge that installs packages is a way of running things, so who may
  talk to whom is not left to a name. Identity here is the user *and* the
  integrity level, because an elevated process and an unelevated one of the
  same user share a SID, and the unelevated one must not get to drive the
  elevated one. Both go into the endpoint name, so those worlds never meet.
  The server's pipe carries a DACL that admits that user alone -- not even
  SYSTEM -- and a mandatory label that refuses a lower elevation. And since
  a name is only a rendezvous, and anyone able to create a pipe of that
  name first would be who a client reached, neither side trusts it: the
  client connects with the server allowed no more than an anonymous view of
  it -- a named-pipe server may otherwise impersonate its client, which is
  precisely what a squatter would want -- and checks that the process at
  the other end is this same user at this same elevation before a byte of
  protocol goes out, refusing the endpoint for good if not; the server
  checks every client the same way before serving it, even though the DACL
  should already have kept anyone else out. A server started by the client
  inherits its token, so it is that user at that elevation by construction;
  the checks are for the case where it was not the client that started it.
- **Handles are ids, not pointers.** An `alpm_db_t *` on the client is the
  server's handle id cast to a pointer and is never dereferenced. Ids are
  never reused and are type-tagged, so a stale or wrong-typed id fails a
  lookup instead of reaching libalpm as a bad pointer. The same object
  always gets the same id, so pointer identity survives the wire: the
  package `alpm_db_get_pkg` returns *is* the element in the pkgcache list,
  and `alpm_list_find_ptr` works. Objects form a tree -- a package belongs
  to its db, a db to its handle, a changelog cursor to its package -- and
  an id carries its root handle in its high bits, which is how the client
  knows what to let go of when a handle is released without being told the
  tree in between.
- **libalpm frees objects behind the table's back, and the overlay says
  where.** `alpm_trans_release` frees the packages the transaction owned,
  `alpm_trans_commit` frees the installed version of whatever it replaced or
  removed, `alpm_db_update` throws away a db's package cache. None of that
  is in the header. `overlay.json` names those calls, and the generated
  handler brackets each with a pair of hand-written hooks
  (`src/server/arpc_invalidate.c`): note what is about to die before the
  call, drop its ids after it. A post hook may not call libalpm, because
  every public function resets the handle's errno and the client has not
  read it yet.
- **What `alpm_trans_prepare` puts in `data` depends on why it failed.** The
  header's comment says `alpm_depmissing_t`; that is only the
  unsatisfied-deps case. A conflict puts `alpm_conflict_t` there and a bad
  architecture puts package names, and pacman's own `sync_prepare()`
  switches on `alpm_errno()` to tell which -- so the overlay maps errno to
  element type, the server reads the errno before anything can reset it and
  sends it beside the list, and the client picks the materialiser by it.
  `alpm_trans_commit` is the same, with file conflicts and file names. Every
  errno in the mapping is checked against the enum. The conflict case has a
  wrinkle: libalpm makes copies of the two packages for a conflict it hands
  out, so freeing the conflict on the server once it is serialised would
  free what the client was just given ids for. Such a record is *adopted*
  instead -- kept under the handle, and freed when the handle is released.
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
  ones hash a package file or compute a download size. `alpm_pkg_get_db` is
  a column too, since the same db always gets the same id. The one field a
  setter changes, the install reason, has its column dropped by that
  setter, `alpm_pkg_set_reason`, and by nothing else; a string column is
  never dropped while the handle lives, since a caller may hold a pointer
  into it, and nothing changes what a package's strings say. Columns
  survive `alpm_pkg_free` of one member, since a freed package's entries
  are dead weight rather than a hazard.
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
  what they are. `alpm_siglist_t` is the same shape, and forgetting to
  declare it was caught immediately — a `siglist` of one, rendered as an
  object rather than an array, which is precisely what the entry exists to
  prevent.
- **A field can be declared not to cross, with its reason.** One does:
  `alpm_pgpkey_t.data` is the gpgme key object itself, and a pointer into
  the server's gpgme means nothing here. It is left NULL, the reason is in
  the overlay *and* in the generated source on both sides, and
  `coverage.json` lists every record that has such a field so it cannot
  become a quiet way of calling an awkward record supported. Everything a
  caller reads *about* the key — fingerprint, uid, name, email, created,
  expires, length, revoked, algorithm — is its own field and crosses intact.
- **`alpm_dep_free` and friends never reach the server.** They free a struct
  this client materialised; the server has libalpm's own copy, which is not
  ours to free. The generated stub calls the matching free helper locally.
  `alpm_filelist_contains` stays here for the mirror reason: it must return a
  pointer *into* the caller's own filelist, and a round trip would return one
  into libalpm's copy. It is libalpm's own `bsearch`, in
  `src/client/arpc_local.c`.
- **The callback marshalling is generated too, and the tag mapping is
  checked.** A payload is a struct whose fields the model already has; the
  only thing a header cannot say is which union member a tag value selects.
  That is an overlay table, and `emit.py` requires every value of the tag
  enum to appear in it — mapped, or listed as carrying only its type — and
  fails the build naming any that does not. Written by hand, a variant added
  by a later libalpm would have fallen quietly into `default:` and arrived
  empty; the callbacks were the last place on this wire where that could
  still happen. What stays hand-written is the transport either side of it.
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
- **An opaque `void *` is an object, and gets a handle like anything else.**
  A changelog cursor is a `FILE *` the server is holding part-way through a
  file. The header says `void`, so which kind of object it is — and therefore
  which tag guards it — is an overlay entry; after that it behaves like every
  other handle, and closing it drops the id so a later read misses instead of
  reaching a cursor libalpm has already freed.
- **An mtree is not proxied at all — it is parsed here.** `struct archive` is
  a libarchive object whose entries a caller reads with `archive_entry_*`, so
  there is no accessor for this bridge to sit in front of — the same problem
  `alpm_list_t` has. The server sends the whole listing in one call and the
  client parses it with its own libarchive into a real archive and real
  entries, so anything the caller asks of them works. What travels is what
  libarchive's mtree *writer* makes of what its mtree *reader* read: going out
  through the same format it came in by keeps the mapping between mtree
  keywords and `archive_entry` fields libarchive's business, so nothing here
  enumerates which fields matter. Reading an mtree costs one round trip
  instead of one per entry as a side effect.
- **Bytes are not text.** A signature has NULs in it, so a JSON string would
  carry the first byte and stop. The three functions that deal in raw bytes
  send them base64 (`src/common/arpc_b64.c`), and the length that comes back
  is the decoded length rather than a number that travelled alongside — where
  both exist, as on the input side, they are checked against each other and a
  disagreement fails the call. Nothing else uses it: package names and paths
  are text and go as text.
- **`...` is formatted on whichever side has the arguments.** The wire cannot
  carry a `va_list` in either direction, so neither side tries. `logcb` fires
  on the server, so the server formats it and sends the text;
  `alpm_logaction` is called on the client, so the client formats it and
  sends the text. The receiving side then passes that text as an argument to
  a literal `"%s"` — never as the format, or a `%` that came out of the
  formatting would be read as a conversion.
- **Paths are the server's, unless the caller asks otherwise.** libalpm runs
  in an MSYS2 process, to which `C:\msys64` is `/`. By default paths cross
  unchanged in both directions, which is honest but leaves a native caller
  to translate `fetchcb`'s `localpath` before it can write there -- and the
  translation lives in msys-2.0.dll, which this DLL must never load. So the
  server translates instead, on request: `alpmrpc_win32_paths(1)`, from the
  one header that is this bridge's own (`include/alpmrpc.h`), or
  `ALPMRPC_PATHS=win32` in the environment for a program that cannot be
  changed. Every path the caller then passes is taken as Win32 and every
  path it gets back comes as Win32 -- the root, the cachedirs, a file
  conflict's file, a fetch callback's destination, the files a fetch wrote.
  Which strings are paths is an overlay section, checked against the
  header: a package's filename is a name, a pattern relative to the root
  stays relative, and a URL is a URL, `file://` ones included. The client
  says which form it wants in a hello when it connects, and a connection
  that says nothing gets the server's own form, as every connection did
  before there was anything to say.
- **Borrowed lists are cached against their owner** and looked up *before*
  the call, because libalpm hands back the same pointer for repeated calls
  and a caller may still be holding an earlier one. An empty one is cached
  too, so a package with no optional dependencies is asked once. The key is
  the getter and its arguments, so `alpm_db_get_group` is one group per
  name. If a callback fetches the same borrowed result as the call it
  interrupted, the cache keeps the callback's and the outer call gets it
  too, since a repeat has to be the same pointer even then. Caller-owned
  lists are built fresh and freed by the caller with the usual idiom.
  What libalpm hands back the same pointer for can still change under it: a
  setter replaces the list, an add appends to it, a transaction rewrites
  the package cache. The overlay names the calls that change anything and,
  for each, exactly which cached results it can have changed -- the getter
  named like the setter where the naming allows, stated where it does not,
  and the build fails for a setter that says neither. After one, those are
  *detached*, so the next read fetches afresh -- detached, not freed,
  because after an append the old list is still valid memory natively and a
  caller may be walking it. Detached entries go when the handle does, along
  with everything else cached under it or anything below it, which is why
  naming only what can change matters. A string that comes back unchanged
  takes its old entry back, so its pointer stays stable, as libalpm's would.
  Strings that belong to nobody -- `alpm_strerror`'s -- are static in
  libalpm and are kept for good here.
- **The server is single-threaded**, which keeps libalpm's `fork()` for
  scriptlets and hooks on the path Cygwin actually supports.
- **The client holds one lock for the whole of a call** -- from the cache
  lookup that may answer it to the cache store after it, callbacks included,
  since they arrive inside the call and their own calls nest in the same
  lock. Nothing beneath a stub locks for itself, so there is nothing to
  race. Two threads may call libalpm through this DLL and will take turns,
  one whole call at a time, which is more than native libalpm promises. The
  one difference: a callback that waits on another thread that itself calls
  libalpm will deadlock here, where natively it would not. That is a
  constraint of the design rather than a bug to be fixed -- the frame loop
  that delivers a callback inside the call it interrupts is the only shape a
  synchronous callback can take.
- `ALPMRPC_TRACE=1` dumps every frame. `alpmrpcd --stdio` runs the dispatch
  layer against stdin/stdout with no IPC at all.
- **Rebuilding while a server is up.** Windows will not overwrite a running
  executable, so installing asks every server on this endpoint to exit first
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
`tests/scratch/mkroot.sh` rebuilds before every run: four packages, two of
them conflicting by name, each with a scriptlet, a changelog and an mtree;
three more that exist to be refused -- a missing dependency, a foreign
architecture, a file an installed package owns -- so that each kind of
failed transaction, and the differently typed list each hands back, is
driven for real; a post-transaction hook; a `file://` repo so the download
path runs with no network; and a throwaway GPG key that signs everything,
with its public half in the root's gpgdir. That root also carries its own
`/bin/sh`, because scriptlets and hooks are `chroot`ed into it.

`bench_codec` reports round-trip cost, the codec's share of it, and
throughput on a bulk payload -- run it before and after anything that
touches the wire. `bench_transaction` does the same for the write path; it
rebuilds the fixture itself, so it is not part of `ctest`.

## Status

All 193 functions are on the wire: 189 generated, and four written by hand —
`alpm_filelist_contains` and the three mtree functions. Nothing is refused.
`coverage.json` names the four and the file each lives in, and the generated
export table says the same thing at link time: it lists all 193, so the DLL
cannot quietly lack one.

One *field* does not cross, and that is stated rather than silent:
`alpm_pgpkey_t.data`, the gpgme key object itself. See the design note above.

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

## License

GPL-2.0-or-later. The full text is in [LICENSE](LICENSE).

That is not a free choice. `src/client/vendor/alpm_list.c` is taken verbatim
from pacman and carries "version 2, or (at your option) any later version",
and it is linked into the client DLL — `alpm_list_t` is transparent, so those
functions have to exist here with libalpm's exact semantics. The server links
libalpm itself, which is under the same terms.
