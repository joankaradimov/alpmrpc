#!/usr/bin/env python3
"""api_model.json + overlay.json  ->  generated server dispatch + client stubs.

Nothing here is alpm-specific beyond the overlay: the emitter works off the
model's type kinds. Putting another libalpm function on the wire is a matter
of the header containing it, not of editing this file.

Lists are the interesting part. alpm_list_t is a transparent doubly-linked
list that callers walk with ->next, so the client cannot proxy it -- it has
to hand back a real chain. The server serialises a whole list into one JSON
array and the client materialises it, which also means a list costs one
round trip rather than one per element.
"""
import argparse
import json
import os
import re
import sys

OVERLAY = {}
RECORDS = {}

# --- ownership and type inference ------------------------------------------


def ret_string_owned(fn):
    """libalpm returns caller-owned strings as 'char *' and borrowed ones as
    'const char *'. This holds for every string-returning function in the
    header; main() asserts it rather than trusting it blindly."""
    return "const" not in fn["ret"]["c_type"]


def list_ownership(name):
    spec = OVERLAY["functions"].get(name, {})
    if "list_own" in spec and "ret" in spec["list_own"]:
        return spec["list_own"]["ret"]
    for rule in OVERLAY["rules"]["list_ownership"]:
        if rule.get("match") and re.match(rule["match"], name):
            return rule["own"]
    return "borrowed"


def handle_tag(c_type):
    """alpm_db_t * and const alpm_db_t * must land on the same tag."""
    base = c_type.replace("struct ", "").replace("const", "").replace("*", "")
    base = base.strip().lstrip("_")
    if base.startswith("alpm_"):
        base = base[len("alpm_"):]
    if base.endswith("_t"):
        base = base[:-2]
    return "ARPC_H_" + re.sub(r"[^A-Za-z0-9]", "_", base).upper()


def short(c_type):
    """alpm_depend_t * -> depend; used to name generated helpers."""
    base = c_type.replace("const", "").replace("*", "").strip().lstrip("_")
    if base.startswith("alpm_"):
        base = base[len("alpm_"):]
    if base.endswith("_t"):
        base = base[:-2]
    return re.sub(r"[^A-Za-z0-9]", "_", base)


def find_record(c_type):
    # clang spells an elaborated type "union _alpm_event_t *"; the tag word
    # is not part of the name.
    base = c_type.replace("const", "").replace("*", "")
    base = base.replace("union ", "").replace("struct ", "").strip()
    for cand in (base, "_" + base):
        if cand in RECORDS:
            if cand in OVERLAY.get("records_unsupported", {}):
                return None     # refuse rather than materialise it wrongly
            return RECORDS[cand]
    return None


def record_unsupported_reason(c_type):
    base = c_type.replace("const", "").replace("*", "").strip()
    for cand in (base, "_" + base):
        why = OVERLAY.get("records_unsupported", {}).get(cand)
        if why:
            return why
    return None


def struct_own(name):
    return OVERLAY.get("struct_own", {}).get(name)


def client_local_record(name):
    """Functions that operate on a struct this client materialised: they are
    a local free, not a call."""
    t = OVERLAY.get("client_local", {}).get(name)
    return elem_of(t) if t else None


def client_local_clear_record(name):
    """Releases the contents of a struct the caller owns, rather than the
    struct itself: it was filled in place through an out-param."""
    t = OVERLAY.get("client_local_clear", {}).get(name)
    return elem_of(t) if t else None


def uncarried(rec_name, field_name):
    """A field that does not cross, and the stated reason it does not."""
    return OVERLAY.get("record_fields_uncarried", {}).get(
        rec_name + "." + field_name)


def carried_fields(rec):
    return [f for f in rec["fields"] if not uncarried(rec["name"], f["name"])]


def libalpm_clear_fn(c_type):
    """The libalpm function that releases this record's contents, for the
    server's own copy of a struct it filled in place."""
    want = elem_of(c_type)
    if not want:
        return None
    for fname, t in OVERLAY.get("client_local_clear", {}).items():
        if fname.startswith("_"):
            continue
        e = elem_of(t)
        if e and e["name"] == want["name"]:
            return fname
    return None


def libalpm_free_fn(c_type):
    """The libalpm function that frees this record. client_local already
    pairs the two, so the fact is stated once rather than twice."""
    want = elem_of(c_type)
    if not want:
        return None
    for fname, t in OVERLAY.get("client_local", {}).items():
        if fname.startswith("_"):
            continue
        e = elem_of(t)
        if e and e["name"] == want["name"]:
            return fname
    return None


def elem_of(type_str):
    """Classify a list element type into how it crosses the wire."""
    if type_str is None:
        return None
    t = type_str.strip()
    if t in ("char *", "const char *"):
        return {"kind": "string", "c_type": "char *", "name": "str"}
    rec = find_record(t)
    if rec is not None:
        # A record reached through a by-value field arrives without its
        # star. The helpers are all written against a pointer, and it is
        # the same record either way, so normalise here rather than in
        # every caller.
        ct = t.replace("const ", "").strip()
        if not ct.endswith("*"):
            ct += " *"
        return {"kind": "record", "c_type": ct,
                "record": rec, "name": short(t)}
    if handle_tag(t) in HANDLE_TAGS:
        return {"kind": "handle", "c_type": t.replace("const ", ""),
                "tag": handle_tag(t), "name": short(t)}
    return None


HANDLE_TAGS = set()


def ret_elem(name):
    return elem_of(OVERLAY.get("list_elem", {}).get(name))


def param_elem(fname, pname):
    return elem_of(OVERLAY.get("param_list_elem", {}).get(
        "%s.%s" % (fname, pname)))


def field_list_elem(recname, fname):
    return elem_of(OVERLAY.get("record_list_elem", {}).get(
        "%s.%s" % (recname, fname)))


def is_batchable(fn):
    """A pkg accessor cheap enough to fetch for a whole list in one call.

    Reading one field of 1150 packages one call at a time is 1150 round
    trips; reading it for all of them is one. The client fetches a column on
    first use, so a field nobody touches is never fetched at all."""
    rule = OVERLAY["rules"].get("batch_pkg_fields")
    if not rule:
        return False
    n = fn["name"]
    if n in rule.get("exclude", []):
        return False
    if not re.match(rule["match"], n):
        return False
    if fn.get("variadic") or len(fn["params"]) != 1:
        return False
    p = fn["params"][0]
    if p["kind"] != "handle" or "alpm_pkg_t" not in p["c_type"]:
        return False
    # A handle too -- alpm_pkg_get_db -- now that the same object always
    # gets the same id, so a column of them is as stable as one of names.
    return fn["ret"]["kind"] in ("string", "scalar", "enum", "handle")


def counted_array(rec):
    """A record that is a count plus a pointer to N elements.

    The field kinds cannot express that -- an array field looks exactly like
    a pointer to one struct -- so the overlay names the two fields, and this
    checks they are really there. A typo here would otherwise produce code
    that materialises exactly one element, which is the bug the whole entry
    exists to prevent."""
    ca = OVERLAY.get("records_counted_array", {}).get(rec["name"])
    if not ca:
        return None
    names = {f["name"] for f in rec["fields"]}
    for key in ("count", "items"):
        if ca[key] not in names:
            raise SystemExit(
                "overlay: %s has no field %r for records_counted_array.%s"
                % (rec["name"], ca[key], key))
    return ca


def opaque_handle(fn, pname):
    """A `void *` that is an object the server holds, not data.

    The header says void and nothing more, so which kind of object it is --
    and therefore which tag guards it -- can only be stated."""
    return OVERLAY.get("opaque_handles", {}).get(fn["name"] + "." + pname)


def out_buffer(fn, pname):
    """A caller-provided buffer the callee fills, sized by another parameter,
    with the bytes written as the return value. Returns that parameter."""
    ln = OVERLAY.get("out_buffers", {}).get(fn["name"] + "." + pname)
    if ln and not any(p["name"] == ln for p in fn["params"]):
        raise SystemExit("overlay: %s has no size param %r for "
                         "out_buffers.%s" % (fn["name"], ln, pname))
    return ln


def byte_buffer(fn, pname):
    """n bytes and a length, rather than text.

    `const unsigned char *` parses as a string and `unsigned char **` as a
    string out-param, so nothing in the header separates a signature from a
    name. What does is that the length travels in another parameter, and the
    overlay names it. Returns that parameter's name, or None."""
    ln = OVERLAY.get("byte_buffers", {}).get(fn["name"] + "." + pname)
    if ln and not any(p["name"] == ln for p in fn["params"]):
        raise SystemExit("overlay: %s has no length param %r for "
                         "byte_buffers.%s" % (fn["name"], ln, pname))
    return ln


def variadic_fmt(fn):
    """A variadic function whose `...` the client can format away.

    The wire has no way to carry `...`, but the client is where the arguments
    are, so it formats them and the text travels. The server then calls
    libalpm with a literal "%s" and that text -- never with the text as the
    format, which would read a % that came out of the formatting as one.

    This is the mirror of what the server does for logcb, which consumes a
    va_list and sends what it produced."""
    p = OVERLAY.get("variadic_format", {}).get(fn["name"])
    if not p:
        return None
    if not any(f["name"] == p and f["kind"] == "string"
               for f in fn["params"]):
        raise SystemExit("overlay: %s has no string param %r for "
                         "variadic_format" % (fn["name"], p))
    return p


def field_ctype(rec, fname):
    for f in rec["fields"]:
        if f["name"] == fname:
            return f["c_type"]
    return None


def base_type(c_type):
    """alpm_file_t * -> alpm_file_t, for naming the pointee."""
    return c_type.replace("const", "").rstrip(" *").strip()


# --- callbacks -------------------------------------------------------------


def callbacks_of(model):
    """The callbacks to generate, paired with what the overlay says about
    them. Order is the wire's: an index into this list is the kind id both
    sides use, so it comes from the model and not from a dict."""
    spec = OVERLAY.get("callbacks", {})
    out = []
    for cb in model.get("callbacks", []):
        if cb["name"] in spec:
            out.append((cb, spec[cb["name"]]))
        elif callback_setter(model, cb["name"]):
            out.append((cb, {}))
    if len(out) > 32:
        raise SystemExit("emit: more callbacks than the registry's bitmask "
                         "and arrays hold")
    return out


def callback_setter(model, cbname):
    for fn in model["functions"]:
        if not fn["name"].startswith("alpm_option_set_"):
            continue
        if any(p["kind"] == "callback" and p["c_type"] == cbname
               for p in fn["params"]):
            return fn
    return None


def callback_trio(model, cbname):
    """Setter, getter and ctx getter, found by type rather than restated in
    the overlay: the setter is the alpm_option_set_* that takes this callback,
    the getter the alpm_option_get_* that returns it, and the ctx getter the
    one named after the getter."""
    setter = callback_setter(model, cbname)
    getter = ctxgetter = None
    for fn in model["functions"]:
        if fn["name"].startswith("alpm_option_get_") \
                and fn["ret"]["kind"] == "callback" \
                and fn["ret"]["c_type"] == cbname:
            getter = fn
    if getter:
        want = getter["name"] + "_ctx"
        ctxgetter = next((f for f in model["functions"]
                          if f["name"] == want), None)
    return setter, getter, ctxgetter


def cb_wire_name(setter):
    """alpm_option_set_logcb -> log. The two sides derive it the same way, so
    it is not something either of them has to be told."""
    n = setter["name"][len("alpm_option_set_"):]
    return n[:-2] if n.endswith("cb") else n


def pretty_ctype(ct):
    """clang spells an elaborated type "union _alpm_event_t *"; libalpm's own
    name for it is the typedef, which is what generated code should say."""
    for kw in ("union ", "struct "):
        if ct.startswith(kw):
            rest = ct[len(kw):]
            return rest[1:] if rest.startswith("_") else rest
    return ct


def cb_union_record(cb, spec):
    """The union a callback carries, and the parameter carrying it."""
    pname = spec.get("union_param")
    if not pname:
        return None, None
    p = next(x for x in cb["params"] if x["name"] == pname)
    return p, find_record(p["c_type"])


def cb_variant(member_c_type):
    """A union member's own record."""
    return find_record(member_c_type)


def cb_check_tags(cb, spec, model, union_rec):
    """Every value of the tag enum must be accounted for, and every mapping
    must name something that exists. This is the whole point of stating the
    mapping rather than writing the switch by hand: a variant added by a
    later libalpm fails the build here instead of arriving empty.

    The tag is a field of the union, or a sibling argument when the payload
    is a bare void * -- the check is the same either way."""
    if union_rec:
        tag = union_rec["fields"][0]
    else:
        tag = next(x for x in cb["params"]
                   if x["name"] == spec["tag_param"])
    enum_name = tag.get("extra", {}).get("enum")
    values = next((e["values"] for e in model["enums"]
                   if e["name"] == enum_name), None)
    if values is None:
        raise SystemExit("emit: %s: nothing behind its tag %r"
                         % (cb["name"], tag["name"]))

    mapped = spec.get("members", spec.get("types", {}))
    known = set(mapped) | set(spec.get("type_only", []))
    missing = [v["name"] for v in values if v["name"] not in known]
    if missing:
        raise SystemExit(
            "emit: %s: %d value(s) of %s are in neither the mapping nor "
            "type_only in the overlay: %s.\nlibalpm has grown a variant; say "
            "which payload it selects, or that it carries only its type."
            % (cb["name"], len(missing), enum_name, ", ".join(missing)))

    for val, m in mapped.items():
        ok = ({f["name"] for f in union_rec["fields"]} if union_rec
              else set(RECORDS) | {"_" + x for x in RECORDS})
        if union_rec and m not in ok:
            raise SystemExit("emit: %s: %s maps to %r, which is not a member "
                             "of %s" % (cb["name"], val, m, union_rec["name"]))
        if not union_rec and find_record(m) is None:
            raise SystemExit("emit: %s: %s maps to %r, which is not a record"
                             % (cb["name"], val, m))
    return tag


def cb_skipped_fields(spec, variant, union_rec):
    """The fields of a variant that are not payload: the tag, which travels
    once at the top, and the answer, which travels back rather than out.

    The answer's position is derived and then checked, not assumed: libalpm
    documents `any` as always safe to read, which is only true because every
    variant begins with the same two fields."""
    skip = {variant["fields"][0]["name"]}
    ans = spec.get("answer")
    if not ans:
        return skip
    anyrec = cb_variant(union_rec["fields"][1]["c_type"])
    idx = next(i for i, f in enumerate(anyrec["fields"]) if f["name"] == ans)
    if len(variant["fields"]) <= idx:
        raise SystemExit("emit: %s has no field at the answer's position"
                         % variant["name"])
    slot = variant["fields"][idx]
    if slot["kind"] != "scalar":
        raise SystemExit("emit: %s.%s is where the answer aliases, but it is "
                         "%s, not a scalar -- the aliasing libalpm documents "
                         "no longer holds"
                         % (variant["name"], slot["name"], slot["kind"]))
    skip.add(slot["name"])
    return skip


def out_params_of(name):
    return OVERLAY["functions"].get(name, {}).get("out_params", [])


# Must match ARPC_RET_KEY in src/common/arpc_wire.h. The generator never
# writes the key itself -- arpc_ret_* does -- but it has to know it to check
# that no out-parameter can be mistaken for it.
RET_KEY = "@ret"


def reply_keys_validate(gen):
    """No two members of a reply may share a key.

    A reply carries the call's return value under RET_KEY and every
    out-parameter under its own name. Out-parameter names are C identifiers
    and RET_KEY deliberately is not one, so those two namespaces cannot meet
    -- but that is a property of RET_KEY's spelling, not something the code
    enforces, and it was not always true. alpm_db_search's third parameter is
    named "ret", which is exactly what the return value used to travel as:
    the results and the return value went out as two members of one object
    under one key, and the reader took whichever came first.

    So the spelling is checked rather than trusted, and duplicate
    out-parameters -- which the overlay could still ask for -- with it.
    """
    for fn in gen:
        n = fn["name"]
        seen = set()
        for op in out_params_of(n):
            if op == RET_KEY:
                raise SystemExit(
                    "emit: %s has an out-parameter named %r, which is the key "
                    "the return value travels under. One of them would be "
                    "lost. Give RET_KEY a spelling no C identifier can have."
                    % (n, op))
            if op in seen:
                raise SystemExit("emit: %s has two out-parameters named %r; "
                                 "a reply cannot carry both" % (n, op))
            seen.add(op)


def out_list_by_errno(fname, pname):
    """An out-list whose element type depends on why the call failed.

    alpm_trans_prepare's `data` holds depmissings for unsatisfied deps,
    conflicts for conflicting ones and package names for a bad arch, and
    alpm_trans_commit's holds file conflicts or file names. The header doc
    names one of them; the reference reader, pacman's sync_prepare(),
    switches on alpm_errno() to tell. So does the generated code: the server
    reads the errno after the call and sends it beside the list, and the
    client picks the materialiser by it. Returns [(errno, elem)] or None."""
    m = OVERLAY.get("out_list_elem_by_errno", {}).get(fname + "." + pname)
    if not m:
        return None
    out = []
    for err, t in m.items():
        e = elem_of(t)
        if e is None:
            raise SystemExit("overlay: out_list_elem_by_errno.%s.%s: %r is "
                             "not a type that crosses" % (fname, pname, t))
        out.append((err, e))
    return out


def out_list_elems(fname, pname):
    """Every element type an out-list parameter can carry."""
    by_err = out_list_by_errno(fname, pname)
    if by_err:
        return [e for _, e in by_err]
    e = param_elem(fname, pname)
    return [e] if e else []


def errno_handle_param(fn):
    """The handle whose errno says what an errno-typed list carries."""
    for p in fn["params"]:
        if p["kind"] == "handle" and handle_tag(p["c_type"]) == "ARPC_H_HANDLE":
            return p["name"]
    raise SystemExit("emit: %s has an errno-typed out-list but no "
                     "alpm_handle_t to read the errno from" % fn["name"])


def server_hooks(fn):
    """Calls that make libalpm free objects the handle table still points
    at. The header cannot say which, so the overlay lists them, and the
    generated handler brackets the call with a pre and a post hook written
    by hand in arpc_invalidate.c."""
    return fn["name"] in OVERLAY.get("server_hooks", {})


# --- paths -----------------------------------------------------------------
#
# libalpm's paths are the server's, and a connection may ask to speak Win32
# paths instead; the server then converts every string the overlay names as
# a path, in the direction it is travelling, and nothing else. Which strings
# those are is the overlay's to say -- a package's filename is a name, a
# pattern relative to the root is relative, a URL is a URL -- and every name
# it gives is checked against the model.


def _paths(section):
    return OVERLAY.get("paths", {}).get(section, [])


def path_param(fn, pname):
    return fn["name"] + "." + pname in _paths("params")


def path_return(fn):
    return fn["name"] in _paths("returns")


def path_out_list(fn, pname):
    return fn["name"] + "." + pname in _paths("returns")


def path_field(recname, fname):
    return recname is not None and recname + "." + fname in _paths("fields")


def path_cb_param(cb, pname):
    return cb["name"] + "." + pname in _paths("callback_params")


def paths_validate(model):
    """Every name in the overlay's paths section has to be a string in the
    model, so that a typo is a build failure rather than a path that
    quietly stays in the wrong form."""
    by_name = {f["name"]: f for f in model["functions"]}

    def param_of(fname, pname):
        fn = by_name.get(fname)
        return next((p for p in fn["params"] if p["name"] == pname),
                    None) if fn else None

    def is_str_list(e):
        return e is not None and e["kind"] == "string"

    for key in _paths("params"):
        fname, _, pname = key.partition(".")
        p = param_of(fname, pname)
        ok = p and (p["kind"] == "string" or
                    (p["kind"] == "list" and is_str_list(param_elem(fname, pname))))
        if not ok:
            raise SystemExit("overlay: paths.params: %s is not a string or "
                             "string-list parameter" % key)
    for key in _paths("returns"):
        fname, _, pname = key.partition(".")
        fn = by_name.get(fname)
        if fn and pname:
            p = param_of(fname, pname)
            ok = p and p["kind"] == "ptr_list" and all(
                is_str_list(e) for e in out_list_elems(fname, pname))
        elif fn:
            rk = fn["ret"]["kind"]
            ok = rk == "string" or (rk == "list" and is_str_list(ret_elem(fname)))
        else:
            ok = False
        if not ok:
            raise SystemExit("overlay: paths.returns: %s does not return a "
                             "string, a string list or a string out-list" % key)
    for key in _paths("fields"):
        rname, _, fname = key.partition(".")
        rec = RECORDS.get(rname)
        f = next((f for f in rec["fields"] if f["name"] == fname),
                 None) if rec else None
        if not f or f["kind"] != "string":
            raise SystemExit("overlay: paths.fields: %s is not a string field"
                             % key)
    cbs = {c["name"]: c for c in model.get("callbacks", [])}
    for key in _paths("callback_params"):
        cname, _, pname = key.partition(".")
        cb = cbs.get(cname)
        p = next((p for p in cb["params"] if p["name"] == pname),
                 None) if cb else None
        if not p or p["kind"] != "string":
            raise SystemExit("overlay: paths.callback_params: %s is not a "
                             "string parameter of a callback" % key)


GENERATED = {}          # name -> function, for every function emitted


def invalidation_of(fn):
    """What a call detaches from the client's caches, or None.

    libalpm hands back the same list for repeated calls, so the client
    caches them; a setter replaces one, an add appends to it, a transaction
    rewrites the package cache. The overlay's rules.invalidates says which
    calls change anything and, for each, exactly which cached results -- by
    the getter's name, derived from the setter's where the naming allows and
    stated where it does not -- and under which owner. Naming only what can
    change is what keeps the pile of detached entries small, since they live
    until the handle goes. Detached, not freed: after an add the old pointer
    is still valid memory natively, and a caller may be holding it."""
    spec = OVERLAY["rules"].get("invalidates", {})
    n = fn["name"]
    if not re.match(spec["match"], n):
        if n in spec:
            raise SystemExit("overlay: rules.invalidates names %s, which does "
                             "not match its own `match`" % n)
        return None

    entry = spec.get(n)
    derived = False
    if entry is None:
        for rule in spec.get("derive", []):
            m = re.match(rule["match"], n)
            if m:
                entry = {"keys": [m.expand(k) for k in rule["keys"]]}
                derived = True
                break
    if entry is None:
        raise SystemExit("overlay: %s changes something, and rules.invalidates "
                         "does not say what it detaches" % n)

    keys = []
    for k in entry.get("keys", []):
        if k.startswith("^"):
            found = sorted(g for g in GENERATED if re.match(k, g))
        else:
            found = [k] if k in GENERATED else []
        if not found and not derived:
            raise SystemExit("overlay: rules.invalidates.%s: %r names no "
                             "generated function" % (n, k))
        keys.extend(f for f in found if f not in keys)
    if entry.get("keys") and not keys:
        raise SystemExit("overlay: %s: none of %s is a generated getter; the "
                         "naming rule does not fit it, so say what it detaches"
                         % (n, entry["keys"]))

    columns = entry.get("columns", [])
    for c in columns:
        if c not in GENERATED or not is_batchable(GENERATED[c]):
            raise SystemExit("overlay: rules.invalidates.%s: %r is not a "
                             "batched package field" % (n, c))
        if GENERATED[c]["ret"]["kind"] not in ("scalar", "enum"):
            raise SystemExit("overlay: rules.invalidates.%s: %r is a %s "
                             "column, which cannot be dropped: a caller may "
                             "hold a pointer into it"
                             % (n, c, GENERATED[c]["ret"]["kind"]))

    under = entry.get("under", "self")
    if under not in ("self", "root"):
        p = next((p for p in fn["params"] if p["name"] == under), None)
        pe = param_elem(n, under) if p else None
        if not p or p["kind"] != "list" or not pe or pe["kind"] != "handle":
            raise SystemExit("overlay: rules.invalidates.%s: `under` must be "
                             "self, root or a handle-list parameter, not %r"
                             % (n, under))
    return {"under": under, "keys": keys, "columns": columns}


def cache_key_code(fn, first_handle):
    """The cache key for a borrowed result: the function's name, and if it
    takes anything but its handle, those arguments too -- alpm_db_get_group
    is one group per name, not one per db. Returns (setup code, key)."""
    extra = [p for p in fn["params"] if p["name"] != first_handle]
    if not extra:
        return "", qq(fn["name"])
    fmt, args = [], []
    for p in extra:
        if p["kind"] == "string":
            fmt.append("%s")
            args.append('%s ? %s : ""' % (p["name"], p["name"]))
        elif p["kind"] in ("scalar", "enum"):
            fmt.append("%lld")
            args.append("(long long)%s" % p["name"])
        elif p["kind"] == "handle":
            fmt.append("%llu")
            args.append("(unsigned long long)ARPC_ID(%s)" % p["name"])
        else:
            raise SystemExit("emit: %s: a borrowed result cannot be keyed on "
                             "its %s parameter %s"
                             % (fn["name"], p["kind"], p["name"]))
    code = ("\t/* One result per argument, not one per handle. */\n"
            "\tchar arpc_key[512];\n"
            "\tsnprintf(arpc_key, sizeof(arpc_key), \"%s:%s\", %s);\n"
            % (fn["name"], ":".join(fmt), ", ".join(args)))
    return code, "arpc_key"


def emit_def(model):
    """The DLL's export table, from the model: every function alpm.h
    declares and the alpm_list API beside it, and nothing else. A function
    the generator did not produce and nobody wrote by hand then fails the
    link, by name, rather than going quietly missing from the DLL."""
    o = ["; GENERATED by tools/gen/emit.py from %s -- DO NOT EDIT.\n"
         % model["header"],
         ";\n"
         "; Exactly libalpm's exports: every function alpm.h declares, and the\n"
         "; alpm_list API. One that is missing from the build fails the link\n"
         "; here, by name, rather than going quietly missing from the DLL.\n"
         "EXPORTS\n"]
    for fn in model["functions"]:
        o.append("    %s\n" % fn["name"])
    for name in model.get("list_functions", []):
        o.append("    %s\n" % name)
    o.append("    ; Not libalpm's: the bridge's own cache count, for its tests.\n"
             "    arpc_stats_cached\n")
    return "".join(o)


def elem_tag(e):
    return e["tag"] if e and e["kind"] == "handle" else None


# Which handle a new object is filed under. Objects form a tree -- a package
# belongs to its db, a db to its handle, a changelog cursor to its package --
# and releasing a node releases what is under it.
PARENT_TAG = {
    "ARPC_H_PKG": "ARPC_H_DB",
    "ARPC_H_DB": "ARPC_H_HANDLE",
    "ARPC_H_CHANGELOG": "ARPC_H_PKG",
}


def owner_expr(fn, for_tag):
    """The server-side expression for the owner of an object this call
    produces, of tag `for_tag` (None for a record, which may hold packages).

    Its natural parent when the call has one: a package returned by
    alpm_db_get_pkg() goes under that db. Failing that, walk up from the
    first handle parameter to the parent's level -- a package from
    alpm_pkg_load() goes under the handle, a db from alpm_pkg_get_db() under
    the package's handle. Failing that, the same from the first element of
    the first handle-list parameter, which is all alpm_find_group_pkgs()
    has. Failing that, nobody's, and it lives until the server exits."""
    want = PARENT_TAG.get(for_tag or "ARPC_H_PKG")
    handles = [(i, handle_tag(p["c_type"]))
               for i, p in enumerate(fn["params"]) if p["kind"] == "handle"]
    lists = []
    for i, p in enumerate(fn["params"]):
        if p["kind"] == "list":
            e = param_elem(fn["name"], p["name"])
            if e and e["kind"] == "handle":
                lists.append((i, e["tag"]))
    for i, tag in handles:
        if tag == want:
            return "arpc_arg_id(rq, %d)" % i
    for i, tag in lists:
        if tag == want:
            return "arpc_list_owner(rq, %d)" % i
    level = want or "ARPC_H_NONE"
    if handles:
        return "arpc_ancestor(arpc_arg_id(rq, %d), %s)" % (handles[0][0], level)
    if lists:
        return ("arpc_ancestor(arpc_list_owner(rq, %d), %s)"
                % (lists[0][0], level))
    return "0"


def record_has_handles(rec):
    return any(f["kind"] == "handle" for f in rec["fields"])


def free_elems_code(e, var, indent, owner):
    """Release the elements of a caller-owned list the server has put on
    the wire: strings with free(), handles not at all, being libalpm's own
    objects, and a record with the libalpm function that frees it -- which
    client_local already pairs with the record.

    Except a record that holds handles. A conflict's packages are copies
    libalpm made for its caller, and the client now holds ids into them, so
    freeing the conflict here would free what it was just handed. Such a
    record is adopted instead: filed under `owner`, and freed with it."""
    t = "\t" * indent
    if e["kind"] == "string":
        return "%salpm_list_free_inner(%s, free);\n" % (t, var)
    if e["kind"] == "record":
        ff = libalpm_free_fn(e["c_type"])
        if not ff:
            raise SystemExit("emit: no libalpm free function known for a "
                             "caller-owned list of %s" % e["c_type"])
        if record_has_handles(e["record"]):
            return ("%s/* holds copies of packages the client now has ids\n"
                    "%s * for: kept, under the handle, until it goes */\n"
                    "%sfor (alpm_list_t *l = %s; l; l = l->next)\n"
                    "%s\tarpc_handle_adopt(l->data, %s, adopt_free_%s);\n"
                    % (t, t, t, var, t, owner, e["name"]))
        return ("%sfor (alpm_list_t *l = %s; l; l = l->next)\n"
                "%s\t%s((%s)l->data);\n" % (t, var, t, ff, e["c_type"]))
    return ""


def emit_adopt_wrappers(need):
    """alpm_list_fn_free takes void*, and calling a record's free function
    through that type would be undefined -- so an adopted record's release
    is a wrapper, one per record that can be adopted."""
    o = []
    for e in need:
        ff = libalpm_free_fn(e["c_type"])
        if ff and record_has_handles(e["record"]):
            o.append("ARPC_MAYBE_UNUSED static void adopt_free_%s(void *p)\n"
                     "{\n\t%s((%s)p);\n}\n\n" % (e["name"], ff, e["c_type"]))
    return "".join(o)


def emit_cb_record_handle_drops(spec, urec, pn):
    """Handles reached through a record in a callback's payload are
    libalpm's temporaries -- a conflict's packages are copies made for the
    question -- and are freed once the callback returns. Their ids go with
    it, so a later use misses instead of reaching freed memory."""
    by_member = {}
    for val, member in spec.get("members", {}).items():
        by_member.setdefault(member, []).append(val)
    cases = []
    for member, vals in by_member.items():
        variant = cb_variant(next(f["c_type"] for f in urec["fields"]
                                  if f["name"] == member))
        drops = []
        for f in variant["fields"]:
            if f["kind"] != "struct_ptr":
                continue
            sub = elem_of(f["c_type"])
            if not sub or sub["kind"] != "record":
                continue
            for hf in sub["record"]["fields"]:
                if hf["kind"] == "handle":
                    drops.append((f["name"], hf["name"],
                                  handle_tag(hf["c_type"])))
        if drops:
            cases.append((sorted(vals), member, drops))
    if not cases:
        return ""
    o = ["\t/* Handles reached through a record in the payload are\n"
         "\t * libalpm's temporaries -- a conflict's packages are copies\n"
         "\t * made for the question -- and are freed once this returns.\n"
         "\t * Their ids go now, so a later use misses instead of reaching\n"
         "\t * freed memory. */\n"]
    o.append("\tswitch (%s->%s) {\n" % (pn, urec["fields"][0]["name"]))
    for vals, member, drops in cases:
        for v in vals:
            o.append("\tcase %s:\n" % v)
        for fname, hname, tag in drops:
            o.append("\t\tif (%s->%s.%s)\n"
                     "\t\t\tarpc_handle_drop_ptr(%s->%s.%s->%s, %s);\n"
                     % (pn, member, fname, pn, member, fname, hname, tag))
        o.append("\t\tbreak;\n")
    o.append("\tdefault:\n\t\tbreak;\n\t}\n")
    return "".join(o)


# --- selection -------------------------------------------------------------


def select(model):
    """Split the API into generated / skipped, with a reason for every skip."""
    allow = set(OVERLAY.get("spike", {}).get("include", []))
    skip_map = {k: v for k, v in OVERLAY.get("skip", {}).items()
                if not k.startswith("_")}

    cb_api = callback_api_functions(model)

    gen, skipped = [], []
    for fn in model["functions"]:
        n = fn["name"]
        if n in cb_api:
            # Emitted by the callback emitter, from the same model. Not an
            # ordinary call: registering one never reaches the server as
            # itself, it goes as arpc.set_callback.
            continue
        if n in skip_map:
            skipped.append((n, skip_map[n]))
            continue

        outs = set(out_params_of(n))
        why = None

        if fn.get("variadic") and not variadic_fmt(fn):
            why = "variadic: the wire has no way to carry ..."

        if fn["ret"]["kind"] == "list" and ret_elem(n) is None:
            why = "list return with no element type in the overlay"

        # A struct this client materialised is freed locally; nothing about
        # it needs to reach the server.
        if not why and (client_local_record(n) or client_local_clear_record(n)):
            gen.append(fn)
            continue

        if not why and fn["ret"]["kind"] == "struct_ptr":
            r = record_unsupported_reason(fn["ret"]["c_type"])
            if r:
                why = "unsupported record: " + r
            elif struct_own(n) is None:
                why = "struct return with no ownership in the overlay"

        for p in fn["params"]:
            if why:
                break
            k = p["kind"]
            if k == "ptr_string":
                # unsigned char ** is a byte buffer or it is nothing this
                # generator can name: as text it would stop at the first NUL.
                if not byte_buffer(fn, p["name"]):
                    why = "not yet generated: ptr_string"
                elif p["name"] not in outs:
                    why = "pointer param with undeclared direction: " + p["name"]
            elif k in ("ptr_enum", "ptr_scalar", "ptr_handle"):
                if p["name"] not in outs:
                    why = "pointer param with undeclared direction: " + p["name"]
            elif k == "ptr_list":
                # libalpm fills these and hands ownership to the caller, so
                # they need an element type like any other list -- or one
                # per errno, where what is in it depends on what went wrong.
                if p["name"] not in outs:
                    why = "list out-param not declared: " + p["name"]
                elif not out_list_elems(n, p["name"]):
                    why = "list out-param with no element type: " + p["name"]
            elif k == "struct_ptr":
                r = record_unsupported_reason(p["c_type"])
                if r:
                    why = "unsupported record: " + r
                elif p["name"] in outs:
                    pass        # filled in place; see the out-param code
                elif "const" not in p["c_type"]:
                    # As with lists, const marks the read-only inputs. A
                    # non-const struct param may be written through, and
                    # guessing would be worse than skipping.
                    why = ("non-const struct param, direction unclear: "
                           + p["name"])
            elif k == "list":
                pe = param_elem(n, p["name"])
                if pe is None:
                    why = "list param with no element type: " + p["name"]
                # A record-element list is fine now: the server can read a
                # record back off the wire, so take_list_* can build one.
            elif k == "opaque_void":
                # void * is a server-held object or a buffer to fill, and
                # the overlay is the only thing that can say which.
                if not opaque_handle(fn, p["name"]) \
                        and not out_buffer(fn, p["name"]):
                    why = "unsupported type kind: " + k
            elif k in ("callback", "unsupported", "foreign_handle"):
                why = "unsupported type kind: " + k
            elif k not in ("void", "scalar", "enum", "string", "handle"):
                why = "not yet generated: " + k

        if not why and fn["ret"]["kind"] not in (
                "void", "scalar", "enum", "string", "handle", "list",
                "struct_ptr") \
                and not (fn["ret"]["kind"] == "opaque_void"
                         and opaque_handle(fn, "@return")):
            why = "not yet generated: " + fn["ret"]["kind"]

        if why:
            skipped.append((n, why))
            continue
        if allow and n not in allow:
            skipped.append((n, "not in spike allowlist"))
            continue
        gen.append(fn)

    if allow:
        missing = allow - set(f["name"] for f in gen)
        if missing:
            raise SystemExit("emit: allowlist names functions that were not "
                             "generatable: " + ", ".join(sorted(missing)))
    return gen, skipped, sorted(cb_api)


def _close_over_records(queue):
    """Expand a set of records to everything they reference, transitively."""
    need = {}
    while queue:
        e = queue.pop()
        rec = e["record"]
        if rec["name"] in need:
            continue
        need[rec["name"]] = e
        for f in carried_fields(rec):
            if f["kind"] in ("struct_ptr", "record_value"):
                sub = elem_of(f["c_type"])
                if sub and sub["kind"] == "record":
                    queue.append(sub)
            elif f["kind"] == "list":
                sub = field_list_elem(rec["name"], f["name"])
                if sub and sub["kind"] == "record":
                    queue.append(sub)
    return list(need.values())


def records_input(gen):
    """Records the client sends and the server rebuilds: the mirror of
    records_needed. Kept separate so the emitter only generates the direction
    each record is actually used in."""
    queue = []
    for fn in gen:
        if client_local_record(fn["name"]) or \
                client_local_clear_record(fn["name"]):
            continue        # a local free or clear serialises nothing
        outs = set(out_params_of(fn["name"]))
        for p in fn["params"]:
            if p["kind"] == "struct_ptr":
                if p["name"] in outs:
                    continue        # comes back filled; never goes out
                e = elem_of(p["c_type"])
                if e and e["kind"] == "record":
                    queue.append(e)
            elif p["kind"] == "list":
                pe = param_elem(fn["name"], p["name"])
                if pe and pe["kind"] == "record":
                    queue.append(pe)
    return _close_over_records(queue)


def records_needed(gen, extra=()):
    """Every record the server writes and the client reads, transitively."""
    queue = list(extra)
    for fn in gen:
        e = ret_elem(fn["name"])
        if e and e["kind"] == "record":
            queue.append(e)
        if fn["ret"]["kind"] == "struct_ptr":
            e = elem_of(fn["ret"]["c_type"])
            if e and e["kind"] == "record":
                queue.append(e)
        cl = (client_local_record(fn["name"])
              or client_local_clear_record(fn["name"]))
        if cl:
            queue.append(cl)        # its free helper has to exist
        for p in fn["params"]:
            elems = []
            if p["kind"] == "list":
                elems = [param_elem(fn["name"], p["name"])]
            elif p["kind"] == "ptr_list":
                elems = out_list_elems(fn["name"], p["name"])
            elif p["kind"] == "struct_ptr":
                elems = [elem_of(p["c_type"])]
            queue.extend(e for e in elems if e and e["kind"] == "record")
    return _close_over_records(queue)


# --- emission --------------------------------------------------------------

BANNER = ("/* GENERATED by tools/gen/emit.py from %s -- DO NOT EDIT.\n"
          " * Regenerate with the 'generate' build target. */\n\n")

Q = '"'


def qq(s):
    return Q + s + Q


# ---- server ----


def srv_put_value(o, expr, kind, c_type, owner, indent, recname=None,
                  fname=None):
    """Emit one value of the given kind into the writer."""
    t = "\t" * indent
    if kind == "string" and path_field(recname, fname):
        o.append("%sarpc_ajw_path(w, %s);\n" % (t, expr))
    elif kind == "string":
        o.append("%sajw_str(w, %s);\n" % (t, expr))
    elif kind in ("scalar", "enum"):
        o.append("%sajw_i64(w, (long long)%s);\n" % (t, expr))
    elif kind == "handle":
        o.append("%sajw_i64(w, (long long)arpc_handle_put(%s, %s, owner));\n"
                 % (t, expr, handle_tag(c_type)))
    elif kind == "struct_ptr":
        sub = elem_of(c_type)
        o.append("%sput_%s(w, %s, owner);\n" % (t, sub["name"], expr))
    elif kind == "record_value":
        # The same record as a struct_ptr field, held by value rather than
        # through a pointer -- one address-of away.
        sub = elem_of(c_type)
        o.append("%sput_%s(w, &%s, owner);\n" % (t, sub["name"], expr))
    elif kind == "list":
        sub = field_list_elem(recname, fname)
        o.append("%sput_list_%s(w, %s, owner);\n" % (t, sub["name"], expr))
    else:
        o.append("%sajw_null(w);\t/* unsupported field kind: %s */\n"
                 % (t, kind))


def emit_server_helpers(need, elems):
    o = []
    # forward declarations: records reference each other, and a record field
    # can be a list, so both families need declaring up front
    for e in elems:
        o.append("static void put_list_%s(aj_w *w, const alpm_list_t *l, "
                 "uint64_t owner);\n" % e["name"])
    for e in need:
        # A record reached only through a client-local free may be used in
        # neither direction on the wire; that is legitimate, not dead code.
        o.append("ARPC_MAYBE_UNUSED static void put_%s(aj_w *w, const %s v, "
                 "uint64_t owner);\n" % (e["name"], e["c_type"]))
    o.append("\n")

    for e in need:
        rec = e["record"]
        o.append("static void put_%s(aj_w *w, const %s v, uint64_t owner)\n{\n"
                 % (e["name"], e["c_type"]))
        o.append("\tif (!v) {\n\t\tajw_null(w);\n\t\treturn;\n\t}\n")
        o.append("\t(void)owner;\n")
        ca = counted_array(rec)
        if ca:
            sub = elem_of(field_ctype(rec, ca["items"]))
            o.append("\t/* A count and an array, not one struct: it goes as\n"
                     "\t * the array, whose length is the count. */\n")
            o.append("\tajw_arr_begin(w);\n")
            o.append("\tfor (size_t i = 0; i < v->%s; i++)\n" % ca["count"])
            o.append("\t\tput_%s(w, &v->%s[i], owner);\n"
                     % (sub["name"], ca["items"]))
            o.append("\tajw_arr_end(w);\n}\n\n")
            continue
        o.append("\tajw_obj_begin(w);\n")
        for f in rec["fields"]:
            why = uncarried(rec["name"], f["name"])
            if why:
                o.append("\t/* %s does not cross: %s */\n" % (f["name"], why))
                continue
            o.append("\tajw_key(w, %s);\n" % qq(f["name"]))
            srv_put_value(o, "v->" + f["name"], f["kind"], f["c_type"],
                          "owner", 1, rec["name"], f["name"])
        o.append("\tajw_obj_end(w);\n}\n\n")
    return "".join(o)


def collect_elems(gen, need):
    """Every list element type that appears anywhere in the generated set."""
    elems, seen = [], set()

    def add(e):
        if e and e["name"] not in seen:
            seen.add(e["name"])
            elems.append(e)

    for fn in gen:
        add(ret_elem(fn["name"]))
        for p in fn["params"]:
            # ptr_list is an out-param, but it needs the same writer and
            # builder as any other list of that element type -- one per
            # errno, where what it carries depends on what went wrong.
            if p["kind"] == "list":
                add(param_elem(fn["name"], p["name"]))
            elif p["kind"] == "ptr_list":
                for e in out_list_elems(fn["name"], p["name"]):
                    add(e)
    for e in need:
        for f in e["record"]["fields"]:
            if f["kind"] == "list":
                add(field_list_elem(e["record"]["name"], f["name"]))
    return elems


def emit_server_readers(rin):
    """Rebuild a struct the client sent, for the duration of one call.

    Every libalpm function taking one of these takes it const, so the server
    owns the temporary outright and frees it afterwards."""
    o = []
    for e in rin:
        o.append("static %s read_%s(arpc_req *rq, int n);\n"
                 % (e["c_type"], e["name"]))
        o.append("static void drop_%s(%s v);\n" % (e["name"], e["c_type"]))
    if rin:
        o.append("\n")

    for e in rin:
        rec, ct = e["record"], e["c_type"]
        o.append("static %s read_%s(arpc_req *rq, int n)\n{\n" % (ct, e["name"]))
        o.append("\tif (n < 0 || arpc_node_is_null(rq, n))\n\t\treturn NULL;\n")
        o.append("\t%s v = (%s)calloc(1, sizeof(*v));\n" % (ct, ct))
        o.append("\tif (!v)\n\t\treturn NULL;\n")
        for f in rec["fields"]:
            node = "arpc_node_member(rq, n, " + qq(f["name"]) + ")"
            if f["kind"] == "string":
                o.append("\tv->%s = arpc_node_strdup(rq, %s);\n"
                         % (f["name"], node))
            elif f["kind"] in ("scalar", "enum"):
                o.append("\tv->%s = (%s)arpc_node_i64(rq, %s);\n"
                         % (f["name"], f["c_type"], node))
            elif f["kind"] == "handle":
                o.append("\tv->%s = (%s)arpc_handle_get(\n"
                         "\t\t\t(uint64_t)arpc_node_i64(rq, %s), %s);\n"
                         % (f["name"], f["c_type"], node,
                            handle_tag(f["c_type"])))
            elif f["kind"] == "struct_ptr":
                sub = elem_of(f["c_type"])
                o.append("\tv->%s = read_%s(rq, %s);\n"
                         % (f["name"], sub["name"], node))
            else:
                # Rather than leave the field zeroed and hope: a record that
                # goes in with a list or a by-value struct in it needs a
                # reader written for that, and the build should say so.
                raise SystemExit("emit: %s.%s is a %s field, which cannot "
                                 "be read back off the wire yet"
                                 % (rec["name"], f["name"], f["kind"]))
        o.append("\treturn v;\n}\n\n")

        o.append("static void drop_%s(%s v)\n{\n" % (e["name"], ct))
        o.append("\tif (!v)\n\t\treturn;\n")
        for f in rec["fields"]:
            if f["kind"] == "string":
                o.append("\tfree(v->%s);\n" % f["name"])
            elif f["kind"] == "struct_ptr":
                sub = elem_of(f["c_type"])
                o.append("\tdrop_%s(v->%s);\n" % (sub["name"], f["name"]))
            # handle fields point at libalpm's own objects; not ours to free
        o.append("\tfree(v);\n}\n\n")
    return "".join(o)


def emit_client_writers(rin):
    """Serialise a struct the caller handed us, field for field."""
    o = []
    for e in rin:
        o.append("static void put_%s(arpc_call *c, const %s v);\n"
                 % (e["name"], e["c_type"]))
    if rin:
        o.append("\n")

    for e in rin:
        rec, ct = e["record"], e["c_type"]
        o.append("static void put_%s(arpc_call *c, const %s v)\n{\n"
                 % (e["name"], ct))
        o.append("\tif (!v) {\n\t\tarpc_put_null(c);\n\t\treturn;\n\t}\n")
        o.append("\tarpc_obj_begin(c);\n")
        for f in rec["fields"]:
            o.append("\tarpc_key(c, " + qq(f["name"]) + ");\n")
            if f["kind"] == "string":
                o.append("\tarpc_put_str(c, v->%s);\n" % f["name"])
            elif f["kind"] in ("scalar", "enum"):
                o.append("\tarpc_put_i64(c, (long long)v->%s);\n" % f["name"])
            elif f["kind"] == "handle":
                o.append("\tarpc_put_handle(c, ARPC_ID(v->%s));\n" % f["name"])
            elif f["kind"] == "struct_ptr":
                sub = elem_of(f["c_type"])
                o.append("\tput_%s(c, v->%s);\n" % (sub["name"], f["name"]))
            else:
                raise SystemExit("emit: %s.%s is a %s field, which cannot "
                                 "be sent yet" % (rec["name"], f["name"],
                                                  f["kind"]))
        o.append("\tarpc_obj_end(c);\n}\n\n")
    return "".join(o)


def param_elems_of(gen):
    """Element types that arrive as parameters, so only these need the
    inbound half (take_list_*/drop_list_*). A record used only in returns has
    no server-side reader, and emitting a take_list_* for it would not
    compile."""
    out, seen = [], set()
    for fn in gen:
        for p in fn["params"]:
            if p["kind"] != "list":
                continue
            e = param_elem(fn["name"], p["name"])
            if e and e["name"] not in seen:
                seen.add(e["name"])
                out.append(e)
    return out


def emit_server_list_writers(elems, pelems):
    """One array writer per element type actually used."""
    o = []
    for e in elems:
        o.append("static void put_list_%s(aj_w *w, const alpm_list_t *l, "
                 "uint64_t owner)\n{\n" % e["name"])
        o.append("\tif (!l) {\n\t\tajw_null(w);\n\t\treturn;\n\t}\n")
        o.append("\t(void)owner;\n\tajw_arr_begin(w);\n")
        o.append("\tfor (; l; l = l->next) {\n")
        if e["kind"] == "string":
            o.append("\t\tajw_str(w, (const char *)l->data);\n")
        elif e["kind"] == "handle":
            o.append("\t\tajw_i64(w, (long long)arpc_handle_put(l->data, "
                     "%s, owner));\n" % e["tag"])
        else:
            o.append("\t\tput_%s(w, (const %s)l->data, owner);\n"
                     % (e["name"], e["c_type"]))
        o.append("\t}\n\tajw_arr_end(w);\n}\n\n")

    # rebuilding an incoming list, for list-typed parameters
    for e in pelems:
        if e["kind"] == "record":
            # alpm_list_fn_free takes void*, and casting a function pointer
            # to call it through a different type is undefined -- so wrap.
            o.append("static void drop_%s_v(void *p)\n{\n"
                     "\tdrop_%s((%s)p);\n}\n\n"
                     % (e["name"], e["name"], e["c_type"]))
        o.append("/* Caller-owned temporary: every libalpm function taking a\n"
                 " * list either copies it or takes it const, so the server\n"
                 " * frees this after the call. */\n")
        o.append("static alpm_list_t *take_list_%s(arpc_req *rq, int i)\n{\n"
                 % e["name"])
        o.append("\tint arr = arpc_arg_node(rq, i);\n")
        o.append("\tif (arr < 0 || arpc_node_is_null(rq, arr))\n\t\treturn NULL;\n")
        o.append("\talpm_list_t *out = NULL;\n")
        o.append("\tfor (int e = arpc_node_first(rq, arr); e >= 0;\n"
                 "\t     e = arpc_node_next(rq, e)) {\n")
        if e["kind"] == "string":
            o.append("\t\tchar *s = arpc_node_strdup(rq, e);\n")
            o.append("\t\talpm_list_append(&out, s);\n")
        elif e["kind"] == "record":
            o.append("\t\talpm_list_append(&out, read_%s(rq, e));\n"
                     % e["name"])
        else:
            o.append("\t\tvoid *p = arpc_handle_get("
                     "(uint64_t)arpc_node_i64(rq, e), %s);\n" % e["tag"])
            o.append("\t\tif (!p) {\n\t\t\tarpc_req_mark_bad(rq);\n"
                     "\t\t\tbreak;\n\t\t}\n")
            o.append("\t\talpm_list_append(&out, p);\n")
        o.append("\t}\n\treturn out;\n}\n\n")
        o.append("static void drop_list_%s(alpm_list_t *l)\n{\n" % e["name"])
        if e["kind"] == "string":
            o.append("\talpm_list_free_inner(l, free);\n")
        elif e["kind"] == "record":
            o.append("\talpm_list_free_inner(l, drop_%s_v);\n" % e["name"])
        o.append("\talpm_list_free(l);\n}\n\n")
    return "".join(o)


def emit_pkg_batch(gen):
    """One method that returns a single field for many packages at once."""
    NL = "\n"
    fields = [f for f in gen if is_batchable(f)]
    if not fields:
        return ""

    o = []
    o.append("/* ---- batched package fields ----" + NL)
    o.append(" *" + NL)
    o.append(" * The field is resolved to an index once per call, not once per" + NL)
    o.append(" * package, and each case calls the accessor directly rather than" + NL)
    o.append(" * through a function pointer -- the accessors do not share a" + NL)
    o.append(" * signature, and casting between them would be undefined." + NL)
    o.append(" */" + NL)
    o.append("static int pkg_field_index(const char *f)" + NL + "{" + NL)
    for i, f in enumerate(fields):
        o.append("	if (!strcmp(f, " + qq(f["name"]) + "))" + NL
                 + "		return %d;" % i + NL)
    o.append("	return -1;" + NL + "}" + NL + NL)

    o.append("static void pkg_field_put(aj_w *w, alpm_pkg_t *p, uint64_t id, "
             "int idx)" + NL + "{" + NL + "	(void)id;" + NL
             + "	switch (idx) {" + NL)
    for i, f in enumerate(fields):
        if f["ret"]["kind"] == "string":
            o.append("	case %d: ajw_str(w, %s(p)); break;" % (i, f["name"]) + NL)
        elif f["ret"]["kind"] == "handle":
            # Filed where a call returning it would file it: under the
            # package's ancestor of the right kind.
            tag = handle_tag(f["ret"]["c_type"])
            o.append("	case %d: ajw_i64(w, (long long)arpc_handle_put(%s(p), %s,"
                     % (i, f["name"], tag) + NL
                     + "			arpc_ancestor(id, %s))); break;"
                     % PARENT_TAG.get(tag, "ARPC_H_NONE") + NL)
        else:
            o.append("	case %d: ajw_i64(w, (long long)%s(p)); break;"
                     % (i, f["name"]) + NL)
    o.append("	default: ajw_null(w); break;" + NL + "	}" + NL + "}" + NL + NL)

    o.append("static int h_arpc_pkg_fields(arpc_req *rq, arpc_res *rs)"
             + NL + "{" + NL)
    o.append("	int ids = arpc_arg_node(rq, 0);" + NL)
    o.append("	const char *field = arpc_arg_str(rq, 1);" + NL)
    o.append("	int idx = field ? pkg_field_index(field) : -1;" + NL)
    o.append("	if (arpc_req_bad(rq) || ids < 0 || idx < 0)" + NL)
    o.append("		return arpc_fail(rs, ARPC_E_INVALID_PARAMS," + NL)
    o.append("			" + qq("arpc.pkg_fields: unknown or missing field")
             + ");" + NL)
    o.append("	arpc_ret_begin(rs);" + NL)
    o.append("	aj_w *w = arpc_res_writer(rs);" + NL)
    o.append("	ajw_arr_begin(w);" + NL)
    o.append("	/* One walk: indexing restarts from the head each time, and" + NL)
    o.append("	 * a column is as long as the package list. */" + NL)
    o.append("	for (int e = arpc_node_first(rq, ids); e >= 0;" + NL
             + "	     e = arpc_node_next(rq, e)) {" + NL)
    o.append("		uint64_t id = (uint64_t)arpc_node_i64(rq, e);" + NL)
    o.append("		alpm_pkg_t *p = (alpm_pkg_t *)arpc_handle_get(id, "
             "ARPC_H_PKG);" + NL)
    o.append("		if (!p)" + NL + "			ajw_null(w);" + NL)
    o.append("		else" + NL + "			pkg_field_put(w, p, id, idx);" + NL)
    o.append("	}" + NL + "	ajw_arr_end(w);" + NL + "	return 0;" + NL
             + "}" + NL + NL)
    return "".join(o)


def cb_seed_records(model):
    """Records a callback payload refers to but nothing else does.

    Not the variants themselves -- their fields are emitted inline, so a
    materialiser for one would go unused. What is needed is whatever their
    fields point at: the alpm_depend_t inside a conflict question, say."""
    seeds = []
    for cb, spec in callbacks_of(model):
        up, urec = cb_union_record(cb, spec)
        variants = []
        if urec:
            for member in set(spec.get("members", {}).values()):
                variants.append(cb_variant(
                    next(f["c_type"] for f in urec["fields"]
                         if f["name"] == member)))
        for tname in spec.get("types", {}).values():
            variants.append(find_record(tname))
        for v in variants:
            for f in (v["fields"] if v else []):
                if f["kind"] == "struct_ptr":
                    seeds.append(elem_of(f["c_type"]))
                elif f["kind"] == "list":
                    seeds.append(field_list_elem(v["name"], f["name"]))
    return [s for s in seeds if s and s["kind"] == "record"]


def callback_api_functions(model):
    """The setters, getters and ctx getters. They are generated -- by the
    callback emitter rather than the ordinary one -- so they are neither
    emitted as normal calls nor reported as skipped."""
    names = set()
    for cb, _ in callbacks_of(model):
        for fn in callback_trio(model, cb["name"]):
            if fn:
                names.add(fn["name"])
    return names


def cb_validate(model):
    """Check every tag value is accounted for before emitting anything, so a
    libalpm that grew a variant fails here with a name rather than later with
    a silence."""
    for cb, spec in callbacks_of(model):
        up, urec = cb_union_record(cb, spec)
        if urec or spec.get("tag_param"):
            cb_check_tags(cb, spec, model, urec)


def emit_cb_server(model):
    """The trampolines libalpm actually calls, and the table that installs
    them. Written here rather than by hand because a payload is a struct
    whose fields the model already has -- the only thing that was ever
    hand-knowledge is which member a tag selects, and that is in the overlay
    now, where it can be checked against the enum."""
    cbs = callbacks_of(model)
    o = ["\n/* ---- callback trampolines ----\n"
         " *\n"
         " * libalpm calls these from inside a call the client is waiting on."
         "\n * They serialise their arguments and hand them to the transport,"
         "\n * which sends them up the same pipe and blocks for the answer.\n"
         " */\n\n"]

    for i, (cb, spec) in enumerate(cbs):
        o.append("#define ARPC_CB_%s %d\n"
                 % (cb_wire_name(callback_setter(model, cb["name"])).upper(), i))
    o.append("\n")

    for cb, spec in cbs:
        setter = callback_setter(model, cb["name"])
        wire = cb_wire_name(setter)
        rk = cb["ret"]["kind"]
        rct = cb["ret"]["c_type"]
        dflt = "0"
        if rk != "void":
            # A callback the caller answers: a dead pipe must give libalpm
            # the answer that fails safe, and for a fetch that is an error
            # rather than a download that did not happen.
            dflt = "-1"

        sig = ", ".join("%s %s" % (pretty_ctype(p["c_type"]), p["name"])
                        for p in cb["params"])
        o.append("static %s tr_%s(%s)\n{\n" % (rct, wire, sig))

        ctxp = cb["params"][0]["name"]
        o.append("\tuint64_t owner = (uint64_t)(uintptr_t)%s;\n" % ctxp)
        o.append("\tif (!arpc_cb_wanted(owner, ARPC_CB_%s))\n\t\treturn%s;\n"
                 % (wire.upper(), "" if rk == "void" else " " + dflt))

        fmt = spec.get("format", {})
        if fmt:
            o.append("\t/* The va_list is consumed here; the text is what\n"
                     "\t * travels, since a va_list cannot be marshalled. */\n")
            o.append("\tchar arpc_msg[4096];\n")
            for tgt, src in fmt.items():
                o.append("\tvsnprintf(arpc_msg, sizeof(arpc_msg), %s, %s);\n"
                         % (tgt, src))

        # Named w, and a pointer, because that is what the field marshallers
        # shared with the ordinary handlers write into.
        o.append("\n\taj_w wbuf;\n\taj_w *w = &wbuf;\n"
                 "\tajw_init(w);\n\tajw_obj_begin(w);\n")

        up, urec = cb_union_record(cb, spec)
        for p in cb["params"][1:]:
            pn = p["name"]
            if pn in fmt.values():
                continue                # the va_list itself does not travel
            if up and pn == up["name"]:
                o.append(emit_cb_union_put(cb, spec, urec, pn))
                continue
            if pn == spec.get("payload_param"):
                o.append(emit_cb_void_put(spec, pn))
                continue
            expr = "arpc_msg" if pn in fmt else pn
            o.append("\tajw_key(w, %s);\n" % qq(pn))
            if p["kind"] == "string" and path_cb_param(cb, pn):
                o.append("\tarpc_ajw_path(w, %s);\n" % expr)
            else:
                srv_put_value(o, expr, p["kind"], p["c_type"], "owner", 1)

        o.append("\tajw_obj_end(w);\n\n")
        o.append("\tlong long r = arpc_cb_send(ARPC_CB_%s, owner, w, %s);\n"
                 % (wire.upper(), dflt))
        o.append("\tajw_free(w);\n")

        ans = spec.get("answer")
        if ans and up:
            anym = urec["fields"][1]["name"]
            o.append("\t/* Whatever the caller set is readable here whichever\n"
                     "\t * variant arrived: the aliasing libalpm documents. */\n")
            o.append("\t%s->%s.%s = (int)r;\n" % (up["name"], anym, ans))
        if up:
            o.append(emit_cb_record_handle_drops(spec, urec, up["name"]))
        if rk == "void":
            o.append("\t(void)r;\n")
        else:
            o.append("\treturn (%s)r;\n" % rct)
        o.append("}\n\n")

    for cb, spec in cbs:
        wire = cb_wire_name(callback_setter(model, cb["name"]))
        o.append("static int install_%s(void *h, int on, void *ctx)\n{\n"
                 % wire)
        o.append("\treturn %s((alpm_handle_t *)h, on ? tr_%s : NULL, ctx);\n}\n\n"
                 % (callback_setter(model, cb["name"])["name"], wire))

    o.append("const arpc_cb_kind arpc_cb_kinds[] = {\n")
    for cb, spec in cbs:
        wire = cb_wire_name(callback_setter(model, cb["name"]))
        o.append("\t{ " + qq(wire) + ", install_%s },\n" % wire)
    o.append("\t{ NULL, NULL }\n};\n\n")
    return "".join(o)


def emit_cb_union_put(cb, spec, urec, pn):
    """The tag, then whatever the tag says is live."""
    o = []
    tag = urec["fields"][0]["name"]
    o.append("\tajw_key(w, %s);\n\tajw_i64(w, (long long)%s->%s);\n"
             % (qq(tag), pn, tag))
    o.append("\tswitch (%s->%s) {\n" % (pn, tag))

    by_member = {}
    for val, member in spec.get("members", {}).items():
        by_member.setdefault(member, []).append(val)

    for member, vals in by_member.items():
        variant = cb_variant(next(f["c_type"] for f in urec["fields"]
                                  if f["name"] == member))
        for v in sorted(vals):
            o.append("\tcase %s:\n" % v)
        skip = cb_skipped_fields(spec, variant, urec)
        for f in variant["fields"]:
            if f["name"] in skip:
                continue
            o.append("\t\tajw_key(w, %s);\n" % qq(f["name"]))
            srv_put_value(o, "%s->%s.%s" % (pn, member, f["name"]),
                          f["kind"], f["c_type"], "owner", 2,
                          variant["name"], f["name"])
        o.append("\t\tbreak;\n")
    o.append("\tdefault:\n\t\tbreak;\t/* carries only its type */\n\t}\n")
    return "".join(o)


def emit_cb_void_put(spec, pn):
    """A payload whose type is named by a sibling argument rather than by a
    tag inside it, so there is nothing to work out -- libalpm says which."""
    o = ["\tswitch (%s) {\n" % spec["tag_param"]]
    for val, tname in spec.get("types", {}).items():
        rec = find_record(tname)
        o.append("\tcase %s: {\n" % val)
        o.append("\t\tconst %s *v = (const %s *)%s;\n" % (tname, tname, pn))
        o.append("\t\tif (v) {\n")
        for f in rec["fields"]:
            o.append("\t\t\tajw_key(w, %s);\n" % qq(f["name"]))
            srv_put_value(o, "v->" + f["name"], f["kind"], f["c_type"],
                          "owner", 3, rec["name"], f["name"])
        o.append("\t\t}\n\t\tbreak;\n\t}\n")
    o.append("\tdefault:\n\t\tbreak;\n\t}\n")
    return "".join(o)


# ---- one server handler ----
#
# Five steps, each appending C: read the arguments and declare the
# out-params; refuse the call if any argument failed to cross; make the call
# and put its return on the wire; put the out-params on the wire; drop what
# was made along the way. What a later step needs from an earlier one -- the
# arguments in call order, the temporaries, the out-params by kind -- travels
# in one dict.


def srv_args(fn, o):
    """Read the arguments off the request, in order, and declare the
    out-params. Returns the pieces the rest of the handler needs."""
    n = fn["name"]
    h = {"args": [], "temps": [], "struct_temps": [], "byte_temps": [],
         "path_temps": [], "byte_checks": [], "out_buf": None,
         "out_lists": [], "out_handles": [], "out_bytes": [],
         "out_structs": []}
    args = h["args"]
    # length parameter -> the buffer it measures, so the call is handed
    # the length that was actually decoded
    blen_owner = {byte_buffer(fn, p["name"]): p["name"]
                  for p in fn["params"] if byte_buffer(fn, p["name"])}
    outs_here = set(out_params_of(n))
    for i, p in enumerate(fn["params"]):
        k, pn, ct = p["kind"], p["name"], p["c_type"]
        if pn in outs_here:
            # Declared and passed by the out-param block below. Only a
            # struct_ptr can reach here, being the one kind that is a
            # legitimate input as well.
            continue
        bb = byte_buffer(fn, pn)
        if bb and k == "string":
            # An input buffer: base64 in, malloc'd bytes out, freed after
            # the call. The decoded length is what libalpm is told, and
            # the length that travelled alongside is checked against it
            # rather than believed.
            o.append("\tsize_t %s_n = 0;\n" % pn)
            o.append("\tunsigned char *%s = arpc_arg_bytes(rq, %d, "
                     "&%s_n);\n" % (pn, i, pn))
            args.append("(%s)%s" % (ct, pn))
            h["byte_temps"].append(pn)
            h["byte_checks"].append((pn, bb))
            continue
        if k == "string" and path_param(fn, pn):
            # A path in the caller's form: converted, so a copy, and one
            # to free after the call.
            o.append("\tchar *%s = arpc_path_in(arpc_arg_str(rq, %d));\n"
                     % (pn, i))
            args.append(pn)
            h["path_temps"].append(pn)
        elif k == "string":
            o.append("\tconst char *%s = arpc_arg_str(rq, %d);\n" % (pn, i))
            args.append(pn)
        elif k in ("scalar", "enum"):
            o.append("\tlong long %s = arpc_arg_i64(rq, %d);\n" % (pn, i))
            args.append("%s_n" % blen_owner[pn] if pn in blen_owner
                        else "(%s)%s" % (ct, pn))
        elif k == "handle":
            o.append("\t%s %s = (%s)arpc_arg_handle(rq, %d, %s);\n"
                     % (ct, pn, ct, i, handle_tag(ct)))
            args.append(pn)
        elif k == "struct_ptr":
            e = elem_of(ct)
            o.append("\t%s %s = read_%s(rq, arpc_arg_node(rq, %d));\n"
                     % (e["c_type"], pn, e["name"], i))
            args.append(pn)
            h["struct_temps"].append((pn, e["name"]))
        elif k == "list":
            e = param_elem(n, pn)
            o.append("\talpm_list_t *%s = take_list_%s(rq, %d);\n"
                     % (pn, e["name"], i))
            if path_param(fn, pn):
                o.append("\tarpc_paths_in_list(%s);\n" % pn)
            args.append(pn)
            h["temps"].append((pn, e["name"]))
        elif k == "opaque_void" and opaque_handle(fn, pn):
            o.append("\tvoid *%s = arpc_arg_handle(rq, %d, %s);\n"
                     % (pn, i, opaque_handle(fn, pn)))
            args.append(pn)
        elif k == "opaque_void" and out_buffer(fn, pn):
            # Declared after the loop: it is sized by a parameter that
            # has not been read yet at this point.
            h["out_buf"] = (pn, out_buffer(fn, pn))
            args.append(pn)

    if h["out_buf"]:
        bn, sn = h["out_buf"]
        o.append("\t/* The caller's buffer is on the other side of the\n"
                 "\t * pipe, so libalpm fills one here and the bytes go\n"
                 "\t * back with the count. */\n")
        o.append("\tunsigned char *%s = (%s > 0 && %s < (1 << 24))\n"
                 "\t\t\t? (unsigned char *)malloc((size_t)%s) : NULL;\n"
                 % (bn, sn, sn, sn))

    for op in out_params_of(n):
        for p in fn["params"]:
            if p["name"] != op:
                continue
            if p["kind"] == "ptr_string":
                # A byte buffer libalpm allocates for its caller, and the
                # server is that caller: it goes out base64 and is freed
                # here, because nothing on the client can free it.
                o.append("\tunsigned char *%s_v = NULL;\n" % op)
                args.append("&%s_v" % op)
                h["out_bytes"].append((op, byte_buffer(fn, op)))
            elif p["kind"] == "ptr_list":
                o.append("\talpm_list_t *%s_v = NULL;\n" % op)
                args.append("&%s_v" % op)
                if not out_list_by_errno(n, op):
                    h["out_lists"].append((op, param_elem(n, op)))
            elif p["kind"] == "ptr_handle":
                # alpm_pkg_t ** -> alpm_pkg_t *: one level of
                # indirection off, not every trailing star.
                inner = p["c_type"][:-1].strip()
                o.append("\t%s %s_v = NULL;\n" % (inner, op))
                args.append("&%s_v" % op)
                h["out_handles"].append((op, handle_tag(inner)))
            elif p["kind"] == "struct_ptr":
                # A struct libalpm fills in place. The caller's copy is
                # on the other side of the pipe, so one is provided here
                # and serialised afterwards.
                o.append("\t%s %s_v;\n"
                         % (base_type(p["c_type"]), op))
                o.append("\tmemset(&%s_v, 0, sizeof(%s_v));\n" % (op, op))
                args.append("&%s_v" % op)
                h["out_structs"].append((op, elem_of(p["c_type"]),
                                         libalpm_clear_fn(p["c_type"])))
            else:
                inner = p["c_type"].rstrip(" *")
                o.append("\t%s %s_v = 0;\n" % (inner, op))
                args.append("&%s_v" % op)
    return h


def srv_check(fn, o, h):
    """Refuse the call if any argument failed to cross, letting go of what
    was made so far."""
    o.append("\tif (arpc_req_bad(rq)")
    if h["out_buf"]:
        o.append("\n\t    || (%s > 0 && !%s)" % (h["out_buf"][1], h["out_buf"][0]))
    for bn, ln in h["byte_checks"]:
        # The length that travelled and the length that decoded have to
        # agree. They do by construction, so a disagreement means the
        # frame is not what it claims and libalpm is not told about it.
        o.append("\n\t    || (size_t)%s != %s_n" % (ln, bn))
    o.append(") {\n")
    for tn, te in h["temps"]:
        o.append("\t\tdrop_list_%s(%s);\n" % (te, tn))
    for tn, te in h["struct_temps"]:
        o.append("\t\tdrop_%s(%s);\n" % (te, tn))
    for tn in h["byte_temps"] + h["path_temps"]:
        o.append("\t\tfree(%s);\n" % tn)
    if h["out_buf"]:
        o.append("\t\tfree(%s);\n" % h["out_buf"][0])
    o.append("\t\treturn arpc_fail(rs, ARPC_E_INVALID_PARAMS,\n\t\t\t"
             + qq(fn["name"] + ": bad arguments") + ");\n\t}\n")


def srv_call(fn, o, h):
    """The call, its return on the wire, and any out-list whose element
    type the errno decides."""
    n = fn["name"]
    # The client already formatted the `...` away, so what arrived is
    # text. It goes to libalpm as an argument to a literal "%s", never as
    # the format itself -- a % that came out of the formatting is data.
    vfmt = variadic_fmt(fn)
    args = h["args"]
    if vfmt:
        args = [('"%s", ' + a) if a == vfmt else a for a in args]

    call = "%s(%s)" % (n, ", ".join(args))
    rk, rct = fn["ret"]["kind"], fn["ret"]["c_type"]
    hooked = h["hooked"] = server_hooks(fn)
    err_lists = [(op, out_list_by_errno(n, op)) for op in out_params_of(n)
                 if out_list_by_errno(n, op)]
    if (hooked or err_lists) and rk not in ("scalar", "enum"):
        raise SystemExit("emit: %s: hooks and errno-typed lists need an "
                         "int return to judge the call by" % n)

    if hooked:
        o.append("\t/* libalpm is about to free objects the table points\n"
                 "\t * at; the hooks note them first and drop them after.\n"
                 "\t * See arpc_invalidate.c. */\n")
        o.append("\tvoid *arpc_hook = arpc_hook_%s_pre(rq);\n" % n)

    if rk == "opaque_void":
        owner = owner_expr(fn, opaque_handle(fn, "@return"))
        o.append("\tvoid *r = %s;\n" % call)
        o.append("\t/* Not data: an object this server is holding open,\n"
                 "\t * so it goes back as an id like any other handle. */\n")
        o.append("\tarpc_ret_handle(rs, arpc_handle_put(r, %s, %s));\n"
                 % (opaque_handle(fn, "@return"), owner))
    elif rk == "void":
        o.append("\t%s;\n\tarpc_ret_null(rs);\n" % call)
    elif rk in ("scalar", "enum"):
        if h["out_buf"]:
            bn, _ = h["out_buf"]
            o.append("\tsize_t r = (size_t)%s;\n" % call)
            o.append("\tarpc_ret_i64(rs, (long long)r);\n")
            o.append("\tarpc_out_bytes(rs, " + qq(bn) + ", %s, r);\n" % bn)
            o.append("\tfree(%s);\n" % bn)
        elif hooked or err_lists:
            o.append("\tlong long arpc_rc = (long long)%s;\n" % call)
            o.append("\tarpc_ret_i64(rs, arpc_rc);\n")
        else:
            o.append("\tarpc_ret_i64(rs, (long long)%s);\n" % call)
    elif rk == "string":
        ret = "arpc_ret_path" if path_return(fn) else "arpc_ret_str"
        if ret_string_owned(fn):
            o.append("\tchar *r = %s;\n" % call)
            o.append("\t%s(rs, r);\n" % ret)
            o.append("\tfree(r);\t/* header says char*: caller-owned */\n")
        else:
            o.append("\t%s(rs, %s);\n" % (ret, call))
    elif rk == "handle":
        owner = owner_expr(fn, handle_tag(rct))
        o.append("\t%s r = %s;\n" % (rct, call))
        o.append("\tarpc_ret_handle(rs, arpc_handle_put(r, %s, %s));\n"
                 % (handle_tag(rct), owner))
    elif rk == "struct_ptr":
        e = elem_of(rct)
        o.append("\t%s r = %s;\n" % (rct, call))
        o.append("\tarpc_ret_begin(rs);\n")
        o.append("\tput_%s(arpc_res_writer(rs), r, %s);\n"
                 % (e["name"], owner_expr(fn, None)))
        if struct_own(n) == "caller":
            ff = libalpm_free_fn(rct)
            o.append("\t/* overlay says caller-owned; the server is the\n"
                     "\t * caller that made it, so it frees it here */\n")
            o.append("\t%s(r);\n" % (ff or "free"))
    elif rk == "list":
        e = ret_elem(n)
        own = list_ownership(n)
        o.append("\talpm_list_t *r = %s;\n" % call)
        o.append("\tarpc_ret_begin(rs);\n")
        if path_return(fn):
            o.append("\tarpc_put_path_list(arpc_res_writer(rs), r);\n")
        else:
            o.append("\tput_list_%s(arpc_res_writer(rs), r, %s);\n"
                     % (e["name"], owner_expr(fn, elem_tag(e))))
        if own == "caller":
            o.append("\t/* overlay says caller-owned: the server is that "
                     "caller */\n")
            o.append(free_elems_code(e, "r", 1,
                                     owner_expr(fn, elem_tag(e))))
            o.append("\talpm_list_free(r);\n")

    # An out-list whose element type depends on the errno. The errno is
    # read before anything else can reset it, and travels alongside.
    for op, by_err in err_lists:
        hp = errno_handle_param(fn)
        o.append("\t/* What %s holds depends on why the call failed.\n"
                 "\t * pacman's own reader switches on the errno, and so\n"
                 "\t * does the client, so it travels beside the list. */\n"
                 % op)
        o.append("\talpm_errno_t %s_err = alpm_errno(%s);\n" % (op, hp))
        o.append("\tarpc_out_i64(rs, " + qq(op + ".errno")
                 + ", (long long)%s_err);\n" % op)
        o.append("\tswitch (%s_err) {\n" % op)
        for err, e in by_err:
            own = owner_expr(fn, elem_tag(e))
            o.append("\tcase %s:\n" % err)
            o.append("\t\tput_list_%s(arpc_out_writer(rs, " % e["name"]
                     + qq(op) + "), %s_v, %s);\n" % (op, own))
            o.append(free_elems_code(e, op + "_v", 2, own))
            o.append("\t\tbreak;\n")
        o.append("\tdefault:\n")
        o.append("\t\t/* Empty, or an errno this list was not said to\n"
                 "\t\t * carry anything for: not ours to interpret. */\n")
        o.append("\t\tajw_null(arpc_out_writer(rs, " + qq(op) + "));\n")
        o.append("\t\tbreak;\n\t}\n")
        o.append("\talpm_list_free(%s_v);\n" % op)


def srv_outs(fn, o, h):
    """The out-params on the wire, then the post hook."""
    n = fn["name"]
    out_list_names = [x[0] for x in h["out_lists"]]
    out_handle_tags = dict(h["out_handles"])
    out_byte_len = dict(h["out_bytes"])
    out_struct_map = {x[0]: x for x in h["out_structs"]}
    for op in out_params_of(n):
        if op in out_list_names or out_list_by_errno(n, op):
            continue
        if op in out_struct_map:
            _, e, clearfn = out_struct_map[op]
            o.append("\tput_%s(arpc_out_writer(rs, " % e["name"]
                     + qq(op) + "), &%s_v, %s);\n"
                     % (op, owner_expr(fn, None)))
            o.append("\t/* libalpm filled it for its caller to release,\n"
                     "\t * and here that caller is the server. */\n")
            o.append("\t%s(&%s_v);\n" % (clearfn or "(void)", op))
            continue
        if op in out_byte_len.values():
            # The buffer's own length says how long it is; sending it
            # twice would only create something to disagree with.
            continue
        if op in out_byte_len:
            o.append("\tarpc_out_bytes(rs, " + qq(op)
                     + ", %s_v, %s_v);\n" % (op, out_byte_len[op]))
            o.append("\tfree(%s_v);\t/* libalpm says the caller frees "
                     "it, and that is us */\n" % op)
            continue
        if op in out_handle_tags:
            # A handle handed back through a pointer is filed exactly
            # like a returned one: an id under the same owner, so it
            # dies with that owner and cannot be mistaken for a pointer.
            o.append("\tarpc_out_i64(rs, " + qq(op)
                     + ", (long long)arpc_handle_put(%s_v, %s, %s));\n"
                     % (op, out_handle_tags[op],
                        owner_expr(fn, out_handle_tags[op])))
            continue
        o.append("\tarpc_out_i64(rs, " + qq(op) + ", (long long)%s_v);\n"
                 % op)
    for op, e in h["out_lists"]:
        own = owner_expr(fn, elem_tag(e))
        if path_out_list(fn, op):
            o.append("\tarpc_put_path_list(arpc_out_writer(rs, " + qq(op)
                     + "), %s_v);\n" % op)
        else:
            o.append("\tput_list_%s(arpc_out_writer(rs, " % e["name"]
                     + qq(op) + "), %s_v, %s);\n" % (op, own))
        o.append("\t/* libalpm filled this for us to own, so it goes once\n"
                 "\t * it is on the wire. */\n")
        o.append(free_elems_code(e, op + "_v", 1, own))
        o.append("\talpm_list_free(%s_v);\n" % op)

    if h["hooked"]:
        o.append("\tarpc_hook_%s_post(rq, arpc_hook, arpc_rc);\n" % n)


def srv_finish(fn, o, h):
    """Drop the temporaries, and the ids this call retired."""
    spec = OVERLAY["functions"].get(fn["name"], {})
    for tn, te in h["temps"]:
        o.append("\tdrop_list_%s(%s);\n" % (te, tn))
    for tn, te in h["struct_temps"]:
        o.append("\tdrop_%s(%s);\n" % (te, tn))
    for tn in h["byte_temps"] + h["path_temps"]:
        o.append("\tfree(%s);\n" % tn)

    if spec.get("destroys"):
        o.append("\tarpc_handle_drop_owner(arpc_arg_id(rq, 0));\n")
    if spec.get("closes"):
        ci = next(i for i, p in enumerate(fn["params"])
                  if p["name"] == spec["closes"])
        o.append("\t/* libalpm has closed it, so the id goes too: a later\n"
                 "\t * use then misses instead of reaching a freed "
                 "cursor. */\n")
        o.append("\tarpc_handle_drop(arpc_arg_id(rq, %d));\n" % ci)

    o.append("\treturn 0;\n}\n\n")


def emit_server(model, gen, need, rin, src_header):
    o = [BANNER % src_header]
    o.append('#include "arpc_server.h"\n')
    o.append("#include <alpm.h>\n#include <alpm_list.h>\n"
             "#include <stdlib.h>\n#include <string.h>\n\n")
    o.append("#if defined(__GNUC__) || defined(__clang__)\n"
             "#  define ARPC_MAYBE_UNUSED __attribute__((unused))\n"
             "#else\n#  define ARPC_MAYBE_UNUSED\n#endif\n\n")
    elems = collect_elems(gen, need)
    o.append(emit_server_helpers(need, elems))
    o.append(emit_adopt_wrappers(need))
    o.append(emit_server_readers(rin))
    o.append(emit_server_list_writers(elems, param_elems_of(gen)))

    for fn in gen:
        n = fn["name"]
        if client_local_record(n) or client_local_clear_record(n):
            continue    # handled entirely on the client; never reaches here
        o.append("static int h_%s(arpc_req *rq, arpc_res *rs)\n{\n" % n)
        h = srv_args(fn, o)
        srv_check(fn, o, h)
        srv_call(fn, o, h)
        srv_outs(fn, o, h)
        srv_finish(fn, o, h)

    o.append(emit_cb_server(model))
    o.append(emit_pkg_batch(gen))

    methods = []
    if any(is_batchable(f) for f in gen):
        # Not a libalpm function: the one composite method, which returns a
        # single field for many packages at once.
        methods.append(("arpc.pkg_fields", "h_arpc_pkg_fields"))
    for fn in gen:
        if client_local_record(fn["name"]) or \
                client_local_clear_record(fn["name"]):
            continue
        methods.append((fn["name"], "h_" + fn["name"]))
    # Sorted as strcmp sorts, by bytes, because the dispatcher bsearches it.
    methods.sort(key=lambda m: m[0].encode())
    o.append("/* Sorted by name: the dispatcher bsearches it. */\n")
    o.append("const arpc_method arpc_methods[] = {\n")
    for name, handler in methods:
        o.append("\t{ " + qq(name) + ", %s },\n" % handler)
    o.append("\t{ NULL, NULL }\n};\n")
    o.append("const size_t arpc_method_count = %d;\n" % len(methods))
    return "".join(o)


# ---- client ----


def cli_get_value(o, target, kind, c_type, node, indent, recname=None,
                  fname=None):
    t = "\t" * indent
    if kind == "string":
        o.append("%s%s = arpc_dup(aj_str(d, %s, NULL));\n" % (t, target, node))
    elif kind in ("scalar", "enum"):
        o.append("%s%s = (%s)aj_i64(d, %s, 0);\n" % (t, target, c_type, node))
    elif kind == "handle":
        o.append("%s%s = (%s)(uintptr_t)aj_i64(d, %s, 0);\n"
                 % (t, target, c_type, node))
    elif kind == "struct_ptr":
        sub = elem_of(c_type)
        o.append("%s%s = (%s)get_%s(d, %s);\n"
                 % (t, target, c_type, sub["name"], node))
    elif kind == "record_value":
        o.append("%sfill_%s(&%s, d, %s);\n"
                 % (t, elem_of(c_type)["name"], target, node))
    elif kind == "list":
        sub = field_list_elem(recname, fname)
        o.append("%s%s = build_list_%s(d, %s);\n" % (t, target, sub["name"], node))


def emit_client_helpers(need, elems):
    o = []
    for e in need:
        base = base_type(e["c_type"])
        # fill/clear work on a struct that already exists, which is what a
        # counted array's elements are: they live in one allocation, not one
        # each. get/free are those two plus the allocation.
        o.append("ARPC_MAYBE_UNUSED static void fill_%s(%s *v, "
                 "const aj_doc *d, int n);\n" % (e["name"], base))
        o.append("ARPC_MAYBE_UNUSED static void clear_%s(%s *v);\n"
                 % (e["name"], base))
        o.append("ARPC_MAYBE_UNUSED static void *get_%s(const aj_doc *d, "
                 "int n);\n" % e["name"])
        # A record that only ever appears in caller-owned lists has no
        # caching path, so its free helper can legitimately go unused.
        o.append("ARPC_MAYBE_UNUSED static void free_%s(void *p);\n"
                 % e["name"])
    for e in elems:
        o.append("static alpm_list_t *build_list_%s(const aj_doc *d, int arr);\n"
                 % e["name"])
    o.append("\n")

    for e in need:
        rec = e["record"]
        ct = e["c_type"]
        base = base_type(ct)
        ca = counted_array(rec)

        o.append("static void fill_%s(%s *v, const aj_doc *d, int n)\n{\n"
                 % (e["name"], base))
        if ca:
            sub = elem_of(field_ctype(rec, ca["items"]))
            o.append("\t/* One allocation for the whole array, filled in\n"
                     "\t * place: the elements are values here, not\n"
                     "\t * pointers, so there is nothing per-element to own."
                     "\n\t * The count is whatever actually arrived, not what"
                     "\n\t * was promised. */\n")
            o.append("\tsize_t cnt = (size_t)aj_count(d, n);\n")
            o.append("\tif (!cnt)\n\t\treturn;\n")
            o.append("\tv->%s = (%s *)calloc(cnt, sizeof(*v->%s));\n"
                     % (ca["items"], base_type(sub["c_type"]), ca["items"]))
            o.append("\tif (!v->%s)\n\t\treturn;\n" % ca["items"])
            o.append("\tsize_t i = 0;\n")
            o.append("\tfor (int e = aj_first(d, n); e >= 0 && i < cnt;\n"
                     "\t     e = aj_next(d, e), i++)\n")
            o.append("\t\tfill_%s(&v->%s[i], d, e);\n"
                     % (sub["name"], ca["items"]))
            o.append("\tv->%s = i;\n" % ca["count"])
        else:
            for f in rec["fields"]:
                why = uncarried(rec["name"], f["name"])
                if why:
                    # Left as it was found, which for a fresh record is
                    # zeroed. Saying so here is the point: the reason is in
                    # the generated source, not only in the overlay.
                    o.append("\t/* %s does not cross: %s */\n"
                             % (f["name"], why))
                    continue
                node = "aj_member(d, n, %s)" % qq(f["name"])
                cli_get_value(o, "v->" + f["name"], f["kind"], f["c_type"],
                              node, 1, rec["name"], f["name"])
        o.append("}\n\n")

        o.append("static void *get_%s(const aj_doc *d, int n)\n{\n" % e["name"])
        o.append("\tif (n < 0 || aj_is_null(d, n))\n\t\treturn NULL;\n")
        o.append("\t%s v = (%s)calloc(1, sizeof(*v));\n" % (ct, ct))
        o.append("\tif (!v)\n\t\treturn NULL;\n")
        o.append("\tfill_%s(v, d, n);\n" % e["name"])
        o.append("\treturn v;\n}\n\n")

        o.append("static void clear_%s(%s *v)\n{\n" % (e["name"], base))
        o.append("\tif (!v)\n\t\treturn;\n")
        if ca:
            sub = elem_of(field_ctype(rec, ca["items"]))
            o.append("\tfor (size_t i = 0; i < v->%s; i++)\n" % ca["count"])
            o.append("\t\tclear_%s(&v->%s[i]);\n"
                     % (sub["name"], ca["items"]))
            o.append("\tfree(v->%s);\n" % ca["items"])
        else:
            for f in carried_fields(rec):
                if f["kind"] == "string":
                    o.append("\tfree(v->%s);\n" % f["name"])
                elif f["kind"] == "struct_ptr":
                    sub = elem_of(f["c_type"])
                    o.append("\tfree_%s(v->%s);\n" % (sub["name"], f["name"]))
                elif f["kind"] == "record_value":
                    sub = elem_of(f["c_type"])
                    o.append("\tclear_%s(&v->%s);\n"
                             % (sub["name"], f["name"]))
                elif f["kind"] == "list":
                    sub = field_list_elem(rec["name"], f["name"])
                    o.append("\tarpc_free_list(v->%s, %s);\n"
                             % (f["name"], elem_free_fn(sub)))
                # handle fields hold ids, not memory
        o.append("}\n\n")

        o.append("static void free_%s(void *p)\n{\n" % e["name"])
        o.append("\t%s v = (%s)p;\n\tif (!v)\n\t\treturn;\n" % (ct, ct))
        o.append("\tclear_%s(v);\n\tfree(v);\n}\n\n" % e["name"])

    for e in elems:
        o.append("static alpm_list_t *build_list_%s(const aj_doc *d, int arr)\n"
                 "{\n" % e["name"])
        o.append("\tif (arr < 0 || aj_is_null(d, arr))\n\t\treturn NULL;\n")
        o.append("\talpm_list_t *out = NULL;\n")
        # aj_elem() restarts the sibling walk on every call, so indexing a
        # whole array is quadratic. Walk it once.
        o.append("\tfor (int e = aj_first(d, arr); e >= 0; "
                 "e = aj_next(d, e)) {\n")
        if e["kind"] == "string":
            o.append("\t\talpm_list_append(&out, arpc_dup(aj_str(d, e, NULL)));\n")
        elif e["kind"] == "handle":
            o.append("\t\talpm_list_append(&out, "
                     "(void *)(uintptr_t)aj_i64(d, e, 0));\n")
        else:
            o.append("\t\talpm_list_append(&out, get_%s(d, e));\n" % e["name"])
        o.append("\t}\n")
        if e["kind"] == "handle" and e["tag"] == "ARPC_H_PKG":
            o.append("\t/* Register the whole set, so the first read of any\n"
                     "\t * field can fetch that field for all of them. */\n")
            o.append("\tarpc_pkg_group_register(out);\n")
        o.append("\treturn out;\n}\n\n")
        # How the cache releases a borrowed list of these, when its owner
        # goes: the same shape as a record's free_*, so the cache need not
        # know lists from structs.
        o.append("ARPC_MAYBE_UNUSED static void free_list_%s(void *p)\n{\n"
                 "\tarpc_free_list((alpm_list_t *)p, %s);\n}\n\n"
                 % (e["name"], elem_free_fn(e)))
    return "".join(o)


def elem_free_fn(e):
    if e["kind"] == "string":
        return "free"
    if e["kind"] == "handle":
        return "NULL"          # ids, nothing to release
    return "free_" + e["name"]


def emit_cb_client(model):
    """The caller's side: the six setters, their getters and ctx getters, and
    one dispatcher per callback that rebuilds the payload and calls the
    function pointer this process is holding."""
    cbs = callbacks_of(model)
    o = ["\n/* ---- callbacks ----\n"
         " *\n"
         " * A callback fires on the server, inside a call this process is\n"
         " * waiting on. The frame loop hands it here, where the payload is\n"
         " * rebuilt and the caller's own function pointer is called.\n"
         " */\n\n"]

    for i, (cb, spec) in enumerate(cbs):
        o.append("#define ARPC_CB_%s %d\n"
                 % (cb_wire_name(callback_setter(model, cb["name"])).upper(), i))
    o.append("\n")

    for cb, spec in cbs:
        setter, getter, ctxgetter = callback_trio(model, cb["name"])
        wire = cb_wire_name(setter)
        K = "ARPC_CB_" + wire.upper()
        cbt = cb["name"]

        o.append("int %s(alpm_handle_t * handle, %s cb, void * ctx)\n{\n"
                 % (setter["name"], cbt))
        o.append("\treturn arpc_cb_register(ARPC_ID(handle), %s, " % K
                 + qq(wire) + ",\n\t\t\t\t(void (*)(void))cb, ctx);\n}\n\n")

        if getter:
            o.append("%s %s(alpm_handle_t * handle)\n{\n"
                     % (cbt, getter["name"]))
            o.append("\treturn (%s)arpc_cb_fn(ARPC_ID(handle), %s);\n}\n\n"
                     % (cbt, K))
        if ctxgetter:
            o.append("/* Never asked the server: the pointer is the caller's\n"
                     " * own and has not left this process. */\n")
            o.append("void * %s(alpm_handle_t * handle)\n{\n"
                     % ctxgetter["name"])
            o.append("\treturn arpc_cb_ctx(ARPC_ID(handle), %s);\n}\n\n" % K)

    for cb, spec in cbs:
        o.append(emit_cb_dispatch(model, cb, spec))

    o.append("const arpc_cb_kind arpc_cb_kinds[] = {\n")
    for cb, spec in cbs:
        wire = cb_wire_name(callback_setter(model, cb["name"]))
        o.append("\t{ " + qq(wire) + ", call_%s },\n" % wire)
    o.append("\t{ NULL, NULL }\n};\n\n")
    return "".join(o)


def emit_cb_dispatch(model, cb, spec):
    setter = callback_setter(model, cb["name"])
    wire = cb_wire_name(setter)
    o = ["static long long call_%s(void (*fn)(void), void *ctx,\n"
         "\t\t\tconst aj_doc *d, int args)\n{\n" % wire]

    up, urec = cb_union_record(cb, spec)
    fmt = spec.get("format", {})

    # Locals: the union or payload struct, plus anything materialised for a
    # field that owns memory. Declared up front because a switch case cannot.
    temps = []
    if up:
        # libalpm's own typedef, not clang's elaborated spelling of it.
        base = urec["name"].lstrip("_")
        o.append("\t%s %s;\n\tmemset(&%s, 0, sizeof(%s));\n"
                 % (base, up["name"], up["name"], up["name"]))
        for member in sorted(set(spec.get("members", {}).values())):
            variant = cb_variant(next(f["c_type"] for f in urec["fields"]
                                      if f["name"] == member))
            skip = cb_skipped_fields(spec, variant, urec)
            for f in variant["fields"]:
                if f["name"] in skip:
                    continue
                if f["kind"] == "struct_ptr":
                    sub = elem_of(f["c_type"])
                    t = "t_%s_%s" % (member, f["name"])
                    o.append("\t%s %s = NULL;\n" % (f["c_type"], t))
                    temps.append((t, "free_" + sub["name"], None))
                elif f["kind"] == "list":
                    sub = field_list_elem(variant["name"], f["name"])
                    t = "t_%s_%s" % (member, f["name"])
                    o.append("\talpm_list_t *%s = NULL;\n" % t)
                    temps.append((t, None, elem_free_fn(sub)))
    for tname in spec.get("types", {}).values():
        o.append("\t%s v_%s;\n" % (tname, short(tname)))
    if spec.get("types"):
        o.append("\tvoid *%s = NULL;\n" % spec["payload_param"])

    # Plain parameters straight off the frame.
    for p in cb["params"][1:]:
        pn = p["name"]
        if up and pn == up["name"]:
            continue
        if pn == spec.get("payload_param"):
            continue
        if pn in fmt.values():
            continue
        node = "aj_member(d, args, %s)" % qq(pn)
        if p["kind"] == "string":
            o.append("\tconst char *%s = aj_str(d, %s, NULL);\n" % (pn, node))
        else:
            o.append("\t%s %s = (%s)aj_i64(d, %s, 0);\n"
                     % (p["c_type"], pn, p["c_type"], node))

    if up:
        o.append(emit_cb_union_fill(spec, urec, up["name"]))
    if spec.get("types"):
        o.append(emit_cb_void_fill(spec))

    # The call itself.
    args = []
    for p in cb["params"]:
        pn = p["name"]
        if p is cb["params"][0]:
            args.append("ctx")
        elif up and pn == up["name"]:
            args.append("&" + pn)
        elif pn in fmt.values():
            args.append(None)           # consumed; handled below
        else:
            args.append(pn)
    args = [a for a in args if a is not None]

    if fmt:
        # alpm_cb_log wants a va_list and there is no portable way to build
        # one but to be variadic, so the transport owns that trampoline.
        o.append("\tarpc_cb_log_via(fn, %s);\n" % ", ".join(args))
    elif cb["ret"]["kind"] == "void":
        o.append("\t((%s)fn)(%s);\n" % (cb["name"], ", ".join(args)))
    else:
        o.append("\t%s r = ((%s)fn)(%s);\n"
                 % (cb["ret"]["c_type"], cb["name"], ", ".join(args)))

    for t, freefn, elemfree in temps:
        if freefn:
            o.append("\t%s(%s);\n" % (freefn, t))
        else:
            o.append("\tarpc_free_list(%s, %s);\n" % (t, elemfree))

    ans = spec.get("answer")
    if ans and up:
        o.append("\t/* Read back through `any`, which is safe whichever\n"
                 "\t * variant this was. */\n")
        o.append("\treturn %s.%s.%s;\n"
                 % (up["name"], urec["fields"][1]["name"], ans))
    elif cb["ret"]["kind"] != "void":
        o.append("\treturn (long long)r;\n")
    else:
        o.append("\treturn 0;\n")
    o.append("}\n\n")
    return "".join(o)


def emit_cb_union_fill(spec, urec, pn):
    o = []
    tag = urec["fields"][0]
    o.append("\t%s.%s = (%s)aj_i64(d, aj_member(d, args, %s), 0);\n"
             % (pn, tag["name"], tag["c_type"], qq(tag["name"])))
    o.append("\tswitch (%s.%s) {\n" % (pn, tag["name"]))

    by_member = {}
    for val, member in spec.get("members", {}).items():
        by_member.setdefault(member, []).append(val)

    for member, vals in by_member.items():
        variant = cb_variant(next(f["c_type"] for f in urec["fields"]
                                  if f["name"] == member))
        for v in sorted(vals):
            o.append("\tcase %s:\n" % v)
        skip = cb_skipped_fields(spec, variant, urec)
        for f in variant["fields"]:
            if f["name"] in skip:
                continue
            node = "aj_member(d, args, %s)" % qq(f["name"])
            tgt = "%s.%s.%s" % (pn, member, f["name"])
            if f["kind"] == "struct_ptr":
                sub = elem_of(f["c_type"])
                t = "t_%s_%s" % (member, f["name"])
                o.append("\t\t%s = (%s)get_%s(d, %s);\n"
                         % (t, f["c_type"], sub["name"], node))
                o.append("\t\t%s = %s;\n" % (tgt, t))
            elif f["kind"] == "list":
                sub = field_list_elem(variant["name"], f["name"])
                t = "t_%s_%s" % (member, f["name"])
                o.append("\t\t%s = build_list_%s(d, %s);\n"
                         % (t, sub["name"], node))
                o.append("\t\t%s = %s;\n" % (tgt, t))
            elif f["kind"] == "string":
                # Points into the parsed frame, which outlives the callback.
                o.append("\t\t%s = aj_str(d, %s, NULL);\n" % (tgt, node))
            else:
                cli_get_value(o, tgt, f["kind"], f["c_type"], node, 2,
                              variant["name"], f["name"])
        o.append("\t\tbreak;\n")
    o.append("\tdefault:\n\t\tbreak;\n\t}\n")
    return "".join(o)


def emit_cb_void_fill(spec):
    o = ["\tswitch (%s) {\n" % spec["tag_param"]]
    for val, tname in spec.get("types", {}).items():
        rec = find_record(tname)
        nick = short(tname)
        o.append("\tcase %s:\n" % val)
        o.append("\t\tmemset(&v_%s, 0, sizeof(v_%s));\n" % (nick, nick))
        for f in rec["fields"]:
            node = "aj_member(d, args, %s)" % qq(f["name"])
            cli_get_value(o, "v_%s.%s" % (nick, f["name"]), f["kind"],
                          f["c_type"], node, 2, rec["name"], f["name"])
        o.append("\t\t%s = &v_%s;\n\t\tbreak;\n" % (spec["payload_param"], nick))
    o.append("\tdefault:\n\t\tbreak;\n\t}\n")
    return "".join(o)


def emit_client(model, gen, need, rin, src_header):
    elems = collect_elems(gen, need)
    o = [BANNER % src_header]
    o.append('#include "arpc_client.h"\n')
    o.append("#include <alpm.h>\n#include <alpm_list.h>\n")
    o.append("#include <stdarg.h>\n#include <stdio.h>\n"
             "#include <stdlib.h>\n#include <string.h>\n\n")
    o.append("#if defined(__GNUC__) || defined(__clang__)\n"
             "#  define ARPC_MAYBE_UNUSED __attribute__((unused))\n"
             "#else\n"
             "#  define ARPC_MAYBE_UNUSED\n"
             "#endif\n\n")
    o.append(emit_client_helpers(need, elems))
    o.append(emit_client_writers(rin))

    for fn in gen:
        n, rk, rct = fn["name"], fn["ret"]["kind"], fn["ret"]["c_type"]
        spec = OVERLAY["functions"].get(n, {})
        sig = ", ".join("%s %s" % (p["c_type"], p["name"])
                        for p in fn["params"]) or "void"
        vfmt = variadic_fmt(fn)
        if vfmt:
            sig += ", ..."

        # Frees a struct this client materialised. The server has
        # libalpm's own copy, which is not ours to free, so this never
        # leaves the process.
        cl = client_local_record(n)
        if cl:
            o.append("%s %s(%s)\n{\n" % (rct, n, sig))
            o.append("\tfree_%s(%s);\n"
                     % (cl["name"], fn["params"][0]["name"]))
            if rk != "void":
                o.append("\treturn (%s)0;\n" % rct)
            o.append("}\n\n")
            continue

        # The struct itself is the caller's -- it was filled in place -- so
        # only what this client put in it is released. libalpm's own version
        # would be freeing the server's copy.
        clc = client_local_clear_record(n)
        if clc:
            pn = fn["params"][0]["name"]
            o.append("%s %s(%s)\n{\n" % (rct, n, sig))
            o.append("\tif (%s) {\n" % pn)
            o.append("\t\tclear_%s(%s);\n" % (clc["name"], pn))
            o.append("\t\t/* Zeroed as well as emptied, so cleaning up\n"
                     "\t\t * twice is harmless rather than a double free. */\n")
            o.append("\t\tmemset(%s, 0, sizeof(*%s));\n" % (pn, pn))
            o.append("\t}\n")
            if rk != "void":
                o.append("\treturn (%s)0;\n" % rct)
            o.append("}\n\n")
            continue

        # Batchable accessors never call out on their own. The runtime fetches
        # the whole column for the package's list on first use, so the 1149
        # calls that follow it are plain memory reads.
        if is_batchable(fn):
            pn = fn["params"][0]["name"]
            o.append("%s %s(%s)\n{\n" % (rct, n, sig))
            if rk == "string":
                o.append("\treturn arpc_pkg_field_str(ARPC_ID(%s), " % pn
                         + qq(n) + ");\n")
            elif rk == "handle":
                o.append("\treturn (%s)(uintptr_t)arpc_pkg_field_i64("
                         "ARPC_ID(%s), " % (rct, pn) + qq(n) + ");\n")
            else:
                o.append("\treturn (%s)arpc_pkg_field_i64(ARPC_ID(%s), "
                         % (rct, pn) + qq(n) + ");\n")
            o.append("}\n\n")
            continue

        fail = {
            "void": "return;",
            "scalar": "return (%s)-1;" % rct,
            "enum": "return (%s)0;" % rct,
            "string": "return NULL;",
            "handle": "return NULL;",
            "list": "return NULL;",
            "struct_ptr": "return NULL;",
            "opaque_void": "return NULL;",
        }[rk]
        if rk == "scalar" and rct.split()[0] in ("size_t", "unsigned"):
            # -1 in an unsigned return is not "failed", it is the largest
            # count there is. These return a number of bytes, so nothing
            # read is the honest answer.
            fail = "return 0;"
        if "fail" in spec:
            # What to hand the caller when the server cannot be reached at
            # all, where the kind's default would mislead: alpm_errno's zero
            # says "no error", and alpm_strerror's NULL is a crash in the
            # printf every frontend puts it through.
            fail = "return (%s)(%s);" % (rct, spec["fail"])

        first_handle = None
        for p in fn["params"]:
            if p["kind"] == "handle":
                first_handle = p["name"]
                break
        owner = "ARPC_ID(%s)" % first_handle if first_handle else "0"

        # The lock, for the whole of the call: see arpc_client.h.
        o.append("%s %s(%s)\n{\n\tarpc_call c;\n\tarpc_enter();\n"
                 % (rct, n, sig))

        if vfmt:
            o.append("\t/* The wire cannot carry `...`, and this is where\n"
                     "\t * the arguments are, so they are formatted here and\n"
                     "\t * the text travels. */\n")
            o.append("\tchar arpc_fmtbuf[4096];\n")
            o.append("\tva_list arpc_ap;\n")
            o.append("\tva_start(arpc_ap, %s);\n" % vfmt)
            o.append("\tvsnprintf(arpc_fmtbuf, sizeof(arpc_fmtbuf), "
                     "%s ? %s : \"\", arpc_ap);\n" % (vfmt, vfmt))
            o.append("\tva_end(arpc_ap);\n")

        # A borrowed list is looked up before the call, not after: libalpm
        # hands back the same pointer for repeated calls, and re-fetching
        # would either break that or free a list the caller still holds. A
        # cached NULL is an answer, an empty list, and is not fetched again.
        borrowed_ptr = rk == "struct_ptr" and struct_own(n) == "borrowed"
        borrowed_list = rk == "list" and list_ownership(n) == "borrowed"
        cache_key = None
        if borrowed_ptr or borrowed_list:
            setup, cache_key = cache_key_code(fn, first_handle)
            o.append(setup)
        if borrowed_ptr or borrowed_list:
            o.append("\tvoid *cached;\n")
            o.append("\tif (arpc_cached(%s, %s, &cached)) {\n"
                     "\t\tarpc_leave();\n\t\treturn (%s)cached;\n\t}\n"
                     % (owner, cache_key, rct))

        o.append("\tif (!arpc_begin(&c, " + qq(n) + ")) {\n"
                 "\t\tarpc_leave();\n\t\t%s\n\t}\n" % fail)

        for p in fn["params"]:
            k, pn = p["kind"], p["name"]
            blen = byte_buffer(fn, pn)
            if blen and k == "string":
                # Bytes, not text: base64, encoded and freed inside the
                # runtime so there is nothing to clean up on a failure path.
                o.append("\tarpc_put_bytes(&c, %s, (size_t)%s);\n"
                         % (pn, blen))
            elif k == "string":
                o.append("\tarpc_put_str(&c, %s);\n"
                         % ("arpc_fmtbuf" if pn == vfmt else pn))
            elif k in ("scalar", "enum"):
                o.append("\tarpc_put_i64(&c, (long long)%s);\n" % pn)
            elif k == "handle":
                o.append("\tarpc_put_handle(&c, ARPC_ID(%s));\n" % pn)
            elif k == "struct_ptr" and pn in out_params_of(n):
                # Filled on the way back, so nothing goes out in it -- but
                # its slot does, because arguments are read by index.
                o.append("\tarpc_put_null(&c);\n")
            elif k == "struct_ptr":
                e = elem_of(p["c_type"])
                o.append("\tput_%s(&c, %s);\n" % (e["name"], pn))
            elif k == "list":
                e = param_elem(n, pn)
                if e["kind"] == "string":
                    o.append("\tarpc_put_str_list(&c, %s);\n" % pn)
                else:
                    o.append("\tarpc_put_handle_list(&c, %s);\n" % pn)
            elif k == "opaque_void" and opaque_handle(fn, pn):
                o.append("\tarpc_put_handle(&c, ARPC_ID(%s));\n" % pn)
            elif k == "opaque_void" and out_buffer(fn, pn):
                # This buffer is filled on the other side, so nothing goes
                # out in it -- but its slot does, because the arguments are
                # positional and the ones after it are read by index.
                o.append("\tarpc_put_null(&c);\n")

        o.append("\tif (!arpc_invoke(&c)) {\n\t\tarpc_end(&c);\n"
                 "\t\tarpc_leave();\n\t\t%s\n\t}\n" % fail)

        for p in fn["params"]:
            ob = out_buffer(fn, p["name"])
            if not ob:
                continue
            pn = p["name"]
            o.append("\tsize_t %s_n = 0;\n" % pn)
            o.append("\tunsigned char *%s_v = arpc_out_bytes(&c, "
                     % pn + qq(pn) + ", &%s_n);\n" % pn)
            o.append("\t/* Never more than the caller asked for, whatever\n"
                     "\t * came back. */\n")
            o.append("\tif (%s && %s_v)\n" % (pn, pn))
            o.append("\t\tmemcpy(%s, %s_v, %s_n < (size_t)%s\n"
                     "\t\t\t\t? %s_n : (size_t)%s);\n"
                     % (pn, pn, pn, ob, pn, ob))
            o.append("\tfree(%s_v);\n" % pn)

        cli_byte_lens = {byte_buffer(fn, op) for op in out_params_of(n)
                         if byte_buffer(fn, op)}
        for op in out_params_of(n):
            if op in cli_byte_lens:
                continue        # filled from the buffer it measures
            for p in fn["params"]:
                if p["name"] != op:
                    continue
                if p["kind"] == "ptr_string":
                    blen = byte_buffer(fn, op)
                    o.append("\tsize_t %s_n = 0;\n" % op)
                    o.append("\tunsigned char *%s_v = arpc_out_bytes(&c, "
                             % op + qq(op) + ", &%s_n);\n" % op)
                    o.append("\t/* Caller-owned, as the header says: it is\n"
                             "\t * theirs to free, or ours to drop if they\n"
                             "\t * did not ask for it. */\n")
                    o.append("\tif (%s)\n\t\t*%s = %s_v;\n\telse\n"
                             "\t\tfree(%s_v);\n" % (op, op, op, op))
                    o.append("\tif (%s)\n\t\t*%s = %s_n;\n"
                             % (blen, blen, op))
                elif p["kind"] == "ptr_list" and out_list_by_errno(n, op):
                    o.append("\tif (%s) {\n" % op)
                    o.append("\t\t/* Which materialiser depends on why the\n"
                             "\t\t * call failed; the server sent the errno\n"
                             "\t\t * beside the list. */\n")
                    o.append("\t\tint %s_node = arpc_out_node(&c, " % op
                             + qq(op) + ");\n")
                    o.append("\t\tswitch ((alpm_errno_t)arpc_out_i64(&c, "
                             + qq(op + ".errno") + ")) {\n")
                    for err, e in out_list_by_errno(n, op):
                        o.append("\t\tcase %s:\n" % err)
                        o.append("\t\t\t*%s = build_list_%s(arpc_doc(&c), "
                                 "%s_node);\n" % (op, e["name"], op))
                        o.append("\t\t\tbreak;\n")
                    o.append("\t\tdefault:\n\t\t\t*%s = NULL;\n"
                             "\t\t\tbreak;\n\t\t}\n\t}\n" % op)
                elif p["kind"] == "ptr_list":
                    e = param_elem(n, op)
                    o.append("\tif (%s)\n\t\t*%s = build_list_%s("
                             "arpc_doc(&c),\n\t\t\t\tarpc_out_node(&c, "
                             % (op, op, e["name"]) + qq(op) + "));\n")
                elif p["kind"] == "struct_ptr":
                    e = elem_of(p["c_type"])
                    o.append("\t/* Filled in place, into the caller's own\n"
                             "\t * struct. Zeroed first so a field that does\n"
                             "\t * not cross is reliably NULL rather than\n"
                             "\t * whatever the caller's stack held. */\n")
                    o.append("\tif (%s) {\n" % op)
                    o.append("\t\tmemset(%s, 0, sizeof(*%s));\n" % (op, op))
                    o.append("\t\tfill_%s(%s, arpc_doc(&c),\n"
                             "\t\t\t\tarpc_out_node(&c, " % (e["name"], op)
                             + qq(op) + "));\n")
                    o.append("\t}\n")
                elif p["kind"] == "ptr_handle":
                    inner = p["c_type"][:-1].strip()
                    o.append("\tif (%s)\n\t\t*%s = (%s)(uintptr_t)"
                             "arpc_out_i64(&c, " % (op, op, inner)
                             + qq(op) + ");\n")
                    if spec.get("creates") == "handle":
                        # The connection has to outlive what this made, the
                        # same as a create that returns its handle. A
                        # package lives under a handle that already holds
                        # the connection, and would not be a ref of its own
                        # even if it could: alpm_add_pkg hands a loaded
                        # package to the transaction, which frees it, and
                        # alpm_pkg_free is then never called to release it.
                        o.append("\tif (%s && *%s)\n\t\tarpc_conn_ref();\n"
                                 % (op, op))
                else:
                    inner = p["c_type"].rstrip(" *")
                    o.append("\tif (%s)\n\t\t*%s = (%s)arpc_out_i64(&c, "
                             % (op, op, inner) + qq(op) + ");\n")

        inv = invalidation_of(fn)

        def after_call():
            """Once the reply is in: what this call did to the caches, and
            then the lock is let go."""
            if not first_handle:
                o.append("\tarpc_leave();\n")
                return
            if spec.get("destroys") == "handle":
                o.append("\tarpc_purge_root(ARPC_ID(%s));\n" % first_handle)
                o.append("\tarpc_conn_unref();\n")
                o.append("\tarpc_leave();\n")
                return
            if spec.get("destroys"):
                o.append("\tarpc_purge_owner(ARPC_ID(%s));\n" % first_handle)
            if not inv:
                o.append("\tarpc_leave();\n")
                return
            if inv["keys"]:
                o.append("\t/* What this may have changed is detached, so the\n"
                         "\t * next read of it fetches afresh. */\n")
                o.append("\tstatic const char *const arpc_keys[] = {\n")
                for k in inv["keys"]:
                    o.append("\t\t" + qq(k) + ",\n")
                o.append("\t\tNULL\n\t};\n")
                if inv["under"] == "self":
                    o.append("\tarpc_detach(ARPC_ID(%s), 0, arpc_keys);\n"
                             % first_handle)
                elif inv["under"] == "root":
                    o.append("\tarpc_detach(ARPC_ID(%s), 1, arpc_keys);\n"
                             % first_handle)
                else:
                    o.append("\tfor (const alpm_list_t *arpc_l = %s; arpc_l;\n"
                             "\t     arpc_l = arpc_l->next)\n"
                             "\t\tarpc_detach(ARPC_ID(arpc_l->data), 0, "
                             "arpc_keys);\n" % inv["under"])
            for col in inv["columns"]:
                o.append("\tarpc_drop_column(ARPC_ID(%s), %s);\n"
                         % (first_handle, qq(col)))
            o.append("\tarpc_leave();\n")

        if rk == "opaque_void":
            o.append("\tvoid *r = (void *)(uintptr_t)arpc_ret_handle(&c);\n")
            o.append("\tarpc_end(&c);\n")
            after_call()
            o.append("\treturn r;\n")
        elif rk == "void":
            o.append("\tarpc_end(&c);\n")
            after_call()
        elif rk in ("scalar", "enum"):
            o.append("\t%s r = (%s)arpc_ret_i64(&c);\n" % (rct, rct))
            o.append("\tarpc_end(&c);\n")
            after_call()
            o.append("\treturn r;\n")
        elif rk == "string":
            if ret_string_owned(fn):
                o.append("\tchar *r = arpc_take_str(&c);\t/* caller frees */\n")
                o.append("\tarpc_end(&c);\n")
                after_call()
                o.append("\treturn r;\n")
            elif first_handle:
                o.append("\tconst char *r = arpc_intern_str(&c, %s, " % owner
                         + qq(n) + ");\n")
                o.append("\tarpc_end(&c);\n")
                after_call()
                o.append("\treturn r;\n")
            else:
                o.append("\t/* Nothing owns it and its value depends on the\n"
                         "\t * arguments -- alpm_strerror -- so it is kept for\n"
                         "\t * good, as libalpm's own static strings are. */\n")
                o.append("\tconst char *r = arpc_intern_static(&c);\n")
                o.append("\tarpc_end(&c);\n")
                after_call()
                o.append("\treturn r;\n")
        elif rk == "handle":
            o.append("\t%s r = (%s)(uintptr_t)arpc_ret_handle(&c);\n"
                     % (rct, rct))
            o.append("\tarpc_end(&c);\n")
            if spec.get("creates") == "handle":
                o.append("\tif (r)\n\t\tarpc_conn_ref();\n")
            after_call()
            o.append("\treturn r;\n")
        elif rk == "struct_ptr":
            e = elem_of(rct)
            o.append("\t%s r = (%s)get_%s(arpc_doc(&c), "
                     "arpc_ret_node(&c));\n" % (rct, rct, e["name"]))
            o.append("\tarpc_end(&c);\n")
            if struct_own(n) == "borrowed":
                o.append("\t/* Borrowed: lives as long as its owner, and\n"
                         "\t * the caller must not free it. The cache says\n"
                         "\t * which copy lives, in case a callback fetched\n"
                         "\t * the same thing while this was in flight. */\n")
                o.append("\tr = (%s)arpc_cache(%s, %s, r, free_%s);\n"
                         % (rct, owner, cache_key, e["name"]))
            else:
                o.append("\t/* Caller-owned: freed with %s. */\n"
                         % (libalpm_free_fn(rct) or "free"))
            after_call()
            o.append("\treturn r;\n")
        elif rk == "list":
            e = ret_elem(n)
            o.append("\talpm_list_t *r = build_list_%s(arpc_doc(&c), "
                     "arpc_ret_node(&c));\n" % e["name"])
            o.append("\tarpc_end(&c);\n")
            if list_ownership(n) == "borrowed":
                o.append("\t/* Borrowed: lives as long as its owner, and the\n"
                         "\t * caller must not free it. The cache says which\n"
                         "\t * copy lives, in case a callback fetched the same\n"
                         "\t * thing while this was in flight. */\n")
                o.append("\tr = (alpm_list_t *)arpc_cache(%s, %s, r, "
                         "free_list_%s);\n" % (owner, cache_key, e["name"]))
            else:
                o.append("\t/* Caller-owned: freed by the caller, per the\n"
                         "\t * usual libalpm idiom for this function. */\n")
            after_call()
            o.append("\treturn r;\n")
        o.append("}\n\n")

    o.append(emit_cb_client(model))
    return "".join(o)


def emit_handle_tags(model):
    o = [BANNER % "api_model.json"]
    o.append("#ifndef ARPC_HANDLE_TAGS_H\n#define ARPC_HANDLE_TAGS_H\n\n")
    o.append("typedef enum {\n\tARPC_H_NONE = 0,\n")
    for i, t in enumerate(sorted(HANDLE_TAGS), 1):
        o.append("\t%s = %d,\n" % (t, i))
    o.append("} arpc_handle_tag;\n\n#endif\n")
    return "".join(o)


def main():
    global OVERLAY, RECORDS, HANDLE_TAGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--overlay", required=True)
    ap.add_argument("--outdir", required=True)
    a = ap.parse_args()

    with open(a.model, encoding="utf-8") as fh:
        model = json.load(fh)
    with open(a.overlay, encoding="utf-8") as fh:
        OVERLAY = json.load(fh)
    OVERLAY.setdefault("functions", {})
    RECORDS = {r["name"]: r for r in model["records"]}

    for fn in model["functions"]:
        for t in [fn["ret"]] + fn["params"]:
            if t["kind"] == "handle":
                HANDLE_TAGS.add(handle_tag(t["c_type"]))
    # An opaque object has no type name to derive a tag from, so the overlay
    # supplies it. It is still a tag like any other: a stale or wrong-typed
    # id misses the lookup rather than reaching libalpm.
    for tag in OVERLAY.get("opaque_handles", {}).values():
        if isinstance(tag, str):
            HANDLE_TAGS.add(tag)

    owned = set(f["name"] for f in model["functions"]
                if f["ret"]["kind"] == "string" and ret_string_owned(f))
    expected = set(["alpm_dep_compute_string", "alpm_compute_md5sum",
                    "alpm_compute_sha256sum"])
    if owned != expected:
        print("emit: WARNING caller-owned string returns changed in this "
              "libalpm: %s (expected %s). Re-verify ownership before shipping."
              % (sorted(owned), sorted(expected)), file=sys.stderr)

    # The errno an out-list's element type is keyed on has to be a real one:
    # a typo here would be a variant that silently arrives empty.
    errnos = set()
    for e in model["enums"]:
        if any(v["name"] == "ALPM_ERR_OK" for v in e["values"]):
            errnos = {v["name"] for v in e["values"]}
    for key, m in OVERLAY.get("out_list_elem_by_errno", {}).items():
        if key.startswith("_"):
            continue
        for err in m:
            if err not in errnos:
                raise SystemExit("overlay: out_list_elem_by_errno.%s: %s is "
                                 "not a value of alpm_errno_t" % (key, err))

    cb_validate(model)
    gen, skipped, cb_api = select(model)
    GENERATED.update((f["name"], f) for f in gen)
    for name in OVERLAY.get("server_hooks", {}):
        if not name.startswith("_") and name not in GENERATED:
            raise SystemExit("overlay: server_hooks names %s, which is not "
                             "generated" % name)
    paths_validate(model)
    reply_keys_validate(gen)
    need = records_needed(gen, cb_seed_records(model))
    rin = records_input(gen)
    os.makedirs(a.outdir, exist_ok=True)

    files = (
        ("arpc_dispatch.c", emit_server(model, gen, need, rin,
                                       model["header"])),
        ("arpc_stubs.c", emit_client(model, gen, need, rin,
                                    model["header"])),
        ("arpc_handle_tags.h", emit_handle_tags(model)),
        ("alpm.def", emit_def(model)),
    )
    for name, text in files:
        with open(os.path.join(a.outdir, name), "w", encoding="utf-8") as fh:
            fh.write(text)

    reasons = {}
    for _, why in skipped:
        reasons[why] = reasons.get(why, 0) + 1
    report = {
        "generated": len(gen) + len(cb_api),
        "generated_callback_api": cb_api,
        "total": len(model["functions"]),
        "records_materialised": sorted(e["record"]["name"] for e in need),
        # Record names start with an underscore, so a leading one cannot be
        # the test for a comment key here; a real entry is <record>.<field>.
        "record_fields_uncarried": {
            k: v for k, v in
            OVERLAY.get("record_fields_uncarried", {}).items() if "." in k},
        "skipped": {"count": len(skipped), "by_reason": reasons,
                    "detail": dict(skipped)},
        # Calls that detach the client's borrowed caches, with what each
        # detaches, and calls the server brackets with hooks: both come from
        # the overlay, and both are worth seeing at a glance.
        "invalidates": {f["name"]: invalidation_of(f) for f in gen
                        if invalidation_of(f)},
        "server_hooks": sorted(k for k in OVERLAY.get("server_hooks", {})
                               if not k.startswith("_")),
        # The strings the server converts for a connection that asked for
        # Win32 paths, and nothing else does.
        "paths": {k: v for k, v in OVERLAY.get("paths", {}).items()
                  if not k.startswith("_")},
    }
    with open(os.path.join(a.outdir, "coverage.json"), "w",
              encoding="utf-8") as fh:
        json.dump(report, fh, indent=1, sort_keys=True)

    print("emit: generated %d/%d functions, %d record materialisers"
          % (len(gen), len(model["functions"]), len(need)))
    for why, cnt in sorted(reasons.items(), key=lambda kv: -kv[1]):
        print("emit:   skipped %3d  %s" % (cnt, why))


if __name__ == "__main__":
    main()
