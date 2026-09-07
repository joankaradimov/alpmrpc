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
    base = c_type.replace("const", "").replace("*", "").strip()
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
        return {"kind": "record", "c_type": t.replace("const ", ""),
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
    return fn["ret"]["kind"] in ("string", "scalar", "enum")


def exported_record(name):
    """Marshallers shared with the hand-written callback layer, which cannot
    call a static in a generated file."""
    return name in OVERLAY.get("export_helpers", {}).get("records", [])


def exported_list(name):
    return name in OVERLAY.get("export_helpers", {}).get("lists", [])


def out_params_of(name):
    return OVERLAY["functions"].get(name, {}).get("out_params", [])


# --- selection -------------------------------------------------------------


def select(model):
    """Split the API into generated / skipped, with a reason for every skip."""
    allow = set(OVERLAY.get("spike", {}).get("include", []))
    skip_map = {k: v for k, v in OVERLAY.get("skip", {}).items()
                if not k.startswith("_")}

    gen, skipped = [], []
    for fn in model["functions"]:
        n = fn["name"]
        if n in skip_map:
            skipped.append((n, skip_map[n]))
            continue

        outs = set(out_params_of(n))
        why = None

        if fn.get("variadic"):
            why = "variadic: the wire has no way to carry ..."

        if fn["ret"]["kind"] == "list" and ret_elem(n) is None:
            why = "list return with no element type in the overlay"

        # A struct this client materialised is freed locally; nothing about
        # it needs to reach the server.
        if not why and client_local_record(n):
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
            if k in ("ptr_enum", "ptr_scalar", "ptr_handle"):
                if p["name"] not in outs:
                    why = "pointer param with undeclared direction: " + p["name"]
            elif k == "ptr_list":
                # libalpm fills these and hands ownership to the caller, so
                # they need an element type like any other list.
                if p["name"] not in outs:
                    why = "list out-param not declared: " + p["name"]
                elif param_elem(n, p["name"]) is None:
                    why = "list out-param with no element type: " + p["name"]
            elif k == "struct_ptr":
                r = record_unsupported_reason(p["c_type"])
                if r:
                    why = "unsupported record: " + r
                elif p["name"] in outs:
                    why = "struct out-param: " + p["name"]
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
            elif k in ("callback", "unsupported", "opaque_void",
                       "foreign_handle"):
                why = "unsupported type kind: " + k
            elif k not in ("void", "scalar", "enum", "string", "handle"):
                why = "not yet generated: " + k

        if not why and fn["ret"]["kind"] not in (
                "void", "scalar", "enum", "string", "handle", "list",
                "struct_ptr"):
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
    return gen, skipped


def _close_over_records(queue):
    """Expand a set of records to everything they reference, transitively."""
    need = {}
    while queue:
        e = queue.pop()
        rec = e["record"]
        if rec["name"] in need:
            continue
        need[rec["name"]] = e
        for f in rec["fields"]:
            if f["kind"] == "struct_ptr":
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
        if client_local_record(fn["name"]):
            continue        # a local free serialises nothing
        for p in fn["params"]:
            if p["kind"] == "struct_ptr":
                e = elem_of(p["c_type"])
                if e and e["kind"] == "record":
                    queue.append(e)
            elif p["kind"] == "list":
                pe = param_elem(fn["name"], p["name"])
                if pe and pe["kind"] == "record":
                    queue.append(pe)
    return _close_over_records(queue)


def records_needed(gen):
    """Every record the server writes and the client reads, transitively."""
    need, queue = {}, []
    for fn in gen:
        e = ret_elem(fn["name"])
        if e and e["kind"] == "record":
            queue.append(e)
        if fn["ret"]["kind"] == "struct_ptr":
            e = elem_of(fn["ret"]["c_type"])
            if e and e["kind"] == "record":
                queue.append(e)
        cl = client_local_record(fn["name"])
        if cl:
            queue.append(cl)        # its free helper has to exist
        for p in fn["params"]:
            if p["kind"] in ("list", "ptr_list"):
                pe = param_elem(fn["name"], p["name"])
                if pe and pe["kind"] == "record":
                    queue.append(pe)
    while queue:
        e = queue.pop()
        rec = e["record"]
        if rec["name"] in need:
            continue
        need[rec["name"]] = e
        for f in rec["fields"]:
            if f["kind"] == "struct_ptr":
                sub = elem_of(f["c_type"])
                if sub and sub["kind"] == "record":
                    queue.append(sub)
            elif f["kind"] == "list":
                sub = field_list_elem(rec["name"], f["name"])
                if sub and sub["kind"] == "record":
                    queue.append(sub)
    # deepest first, so a helper is defined before it is used
    return list(need.values())


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
    if kind == "string":
        o.append("%sajw_str(w, %s);\n" % (t, expr))
    elif kind in ("scalar", "enum"):
        o.append("%sajw_i64(w, (long long)%s);\n" % (t, expr))
    elif kind == "handle":
        o.append("%sajw_i64(w, (long long)arpc_handle_put(%s, %s, owner));\n"
                 % (t, expr, handle_tag(c_type)))
    elif kind == "struct_ptr":
        sub = elem_of(c_type)
        o.append("%sput_%s(w, %s, owner);\n" % (t, sub["name"], expr))
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
        o.append("\tajw_obj_begin(w);\n")
        for f in rec["fields"]:
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
            # builder as any other list of that element type.
            if p["kind"] in ("list", "ptr_list"):
                add(param_elem(fn["name"], p["name"]))
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
                o.append("\tarpc_put_null(c);\n")
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

    o.append("static void pkg_field_put(aj_w *w, alpm_pkg_t *p, int idx)"
             + NL + "{" + NL + "	switch (idx) {" + NL)
    for i, f in enumerate(fields):
        if f["ret"]["kind"] == "string":
            o.append("	case %d: ajw_str(w, %s(p)); break;" % (i, f["name"]) + NL)
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
    o.append("	int n = arpc_node_count(rq, ids);" + NL)
    o.append("	for (int i = 0; i < n; i++) {" + NL)
    o.append("		uint64_t id = (uint64_t)arpc_node_i64(rq," + NL
             + "				arpc_node_elem(rq, ids, i));" + NL)
    o.append("		alpm_pkg_t *p = (alpm_pkg_t *)arpc_handle_get(id, "
             "ARPC_H_PKG);" + NL)
    o.append("		if (!p)" + NL + "			ajw_null(w);" + NL)
    o.append("		else" + NL + "			pkg_field_put(w, p, idx);" + NL)
    o.append("	}" + NL + "	ajw_arr_end(w);" + NL + "	return 0;" + NL
             + "}" + NL + NL)
    return "".join(o)


def emit_server(gen, need, rin, src_header):
    o = [BANNER % src_header]
    o.append('#include "arpc_server.h"\n')
    o.append("#include <alpm.h>\n#include <alpm_list.h>\n"
             "#include <stdlib.h>\n#include <string.h>\n\n")
    o.append("#if defined(__GNUC__) || defined(__clang__)\n"
             "#  define ARPC_MAYBE_UNUSED __attribute__((unused))\n"
             "#else\n#  define ARPC_MAYBE_UNUSED\n#endif\n\n")
    elems = collect_elems(gen, need)
    o.append(emit_server_helpers(need, elems))
    o.append(emit_server_readers(rin))
    o.append(emit_server_list_writers(elems, param_elems_of(gen)))

    for fn in gen:
        n = fn["name"]
        if client_local_record(n):
            continue    # handled entirely on the client; never reaches here
        spec = OVERLAY["functions"].get(n, {})
        o.append("static int h_%s(arpc_req *rq, arpc_res *rs)\n{\n" % n)

        args, temps, struct_temps = [], [], []
        for i, p in enumerate(fn["params"]):
            k, pn, ct = p["kind"], p["name"], p["c_type"]
            if k == "string":
                o.append("\tconst char *%s = arpc_arg_str(rq, %d);\n" % (pn, i))
                args.append(pn)
            elif k in ("scalar", "enum"):
                o.append("\tlong long %s = arpc_arg_i64(rq, %d);\n" % (pn, i))
                args.append("(%s)%s" % (ct, pn))
            elif k == "handle":
                o.append("\t%s %s = (%s)arpc_arg_handle(rq, %d, %s);\n"
                         % (ct, pn, ct, i, handle_tag(ct)))
                args.append(pn)
            elif k == "struct_ptr":
                e = elem_of(ct)
                o.append("\t%s %s = read_%s(rq, arpc_arg_node(rq, %d));\n"
                         % (e["c_type"], pn, e["name"], i))
                args.append(pn)
                struct_temps.append((pn, e["name"]))
            elif k == "list":
                e = param_elem(n, pn)
                o.append("\talpm_list_t *%s = take_list_%s(rq, %d);\n"
                         % (pn, e["name"], i))
                args.append(pn)
                temps.append((pn, e["name"]))

        out_lists = []
        out_handles = []
        for op in out_params_of(n):
            for p in fn["params"]:
                if p["name"] != op:
                    continue
                if p["kind"] == "ptr_list":
                    o.append("\talpm_list_t *%s_v = NULL;\n" % op)
                    args.append("&%s_v" % op)
                    out_lists.append((op, param_elem(n, op)))
                elif p["kind"] == "ptr_handle":
                    # alpm_pkg_t ** -> alpm_pkg_t *: one level of
                    # indirection off, not every trailing star.
                    inner = p["c_type"][:-1].strip()
                    o.append("\t%s %s_v = NULL;\n" % (inner, op))
                    args.append("&%s_v" % op)
                    out_handles.append((op, handle_tag(inner)))
                else:
                    inner = p["c_type"].rstrip(" *")
                    o.append("\t%s %s_v = 0;\n" % (inner, op))
                    args.append("&%s_v" % op)

        o.append("\tif (arpc_req_bad(rq)) {\n")
        for tn, te in temps:
            o.append("\t\tdrop_list_%s(%s);\n" % (te, tn))
        for tn, te in struct_temps:
            o.append("\t\tdrop_%s(%s);\n" % (te, tn))
        o.append("\t\treturn arpc_fail(rs, ARPC_E_INVALID_PARAMS,\n\t\t\t"
                 + qq(n + ": bad arguments") + ");\n\t}\n")

        call = "%s(%s)" % (n, ", ".join(args))
        rk, rct = fn["ret"]["kind"], fn["ret"]["c_type"]

        # the owner a returned handle or list element should be filed under
        owner = "0"
        for i, p in enumerate(fn["params"]):
            if p["kind"] != "handle":
                continue
            owner = ("arpc_arg_id(rq, %d)" % i
                     if handle_tag(p["c_type"]) == "ARPC_H_HANDLE"
                     else "arpc_owner_of(arpc_arg_id(rq, %d))" % i)
            break

        if rk == "void":
            o.append("\t%s;\n\tarpc_ret_null(rs);\n" % call)
        elif rk in ("scalar", "enum"):
            o.append("\tarpc_ret_i64(rs, (long long)%s);\n" % call)
        elif rk == "string":
            if ret_string_owned(fn):
                o.append("\tchar *r = %s;\n" % call)
                o.append("\tarpc_ret_str(rs, r);\n")
                o.append("\tfree(r);\t/* header says char*: caller-owned */\n")
            else:
                o.append("\tarpc_ret_str(rs, %s);\n" % call)
        elif rk == "handle":
            o.append("\t%s r = %s;\n" % (rct, call))
            o.append("\tarpc_ret_handle(rs, arpc_handle_put(r, %s, %s));\n"
                     % (handle_tag(rct), owner))
        elif rk == "struct_ptr":
            e = elem_of(rct)
            o.append("\t%s r = %s;\n" % (rct, call))
            o.append("\tarpc_ret_begin(rs);\n")
            o.append("\tput_%s(arpc_res_writer(rs), r, %s);\n"
                     % (e["name"], owner))
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
            o.append("\tput_list_%s(arpc_res_writer(rs), r, %s);\n"
                     % (e["name"], owner))
            if own == "caller":
                o.append("\t/* overlay says caller-owned: the server is that "
                         "caller */\n")
                if e["kind"] == "string":
                    o.append("\talpm_list_free_inner(r, free);\n")
                o.append("\talpm_list_free(r);\n")

        out_list_names = [x[0] for x in out_lists]
        out_handle_tags = dict(out_handles)
        for op in out_params_of(n):
            if op in out_list_names:
                continue
            if op in out_handle_tags:
                # A handle handed back through a pointer is filed exactly
                # like a returned one: an id under the same owner, so it
                # dies with that owner and cannot be mistaken for a pointer.
                o.append("\tarpc_out_i64(rs, " + qq(op)
                         + ", (long long)arpc_handle_put(%s_v, %s, %s));\n"
                         % (op, out_handle_tags[op], owner))
                continue
            o.append("\tarpc_out_i64(rs, " + qq(op) + ", (long long)%s_v);\n"
                     % op)
        for op, e in out_lists:
            o.append("\tput_list_%s(arpc_out_writer(rs, " % e["name"]
                     + qq(op) + "), %s_v, %s);\n" % (op, owner))
            o.append("\t/* libalpm filled this for us to own, so it goes once\n"
                     "\t * it is on the wire. */\n")
            if e["kind"] == "string":
                o.append("\talpm_list_free_inner(%s_v, free);\n" % op)
            o.append("\talpm_list_free(%s_v);\n" % op)

        for tn, te in temps:
            o.append("\tdrop_list_%s(%s);\n" % (te, tn))
        for tn, te in struct_temps:
            o.append("\tdrop_%s(%s);\n" % (te, tn))

        if spec.get("destroys"):
            o.append("\tarpc_handle_drop_owner(arpc_arg_id(rq, 0));\n")

        o.append("\treturn 0;\n}\n\n")

    o.append(emit_pkg_batch(gen))

    o.append("const arpc_method arpc_methods[] = {\n")
    if any(is_batchable(f) for f in gen):
        # Not a libalpm function: the one composite method, which returns a
        # single field for many packages at once.
        o.append("\t{ " + qq("arpc.pkg_fields") + ", h_arpc_pkg_fields },\n")
    for fn in gen:
        if client_local_record(fn["name"]):
            continue
        o.append("\t{ " + qq(fn["name"]) + ", h_%s },\n" % fn["name"])
    o.append("\t{ NULL, NULL }\n};\n")
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
    elif kind == "list":
        sub = field_list_elem(recname, fname)
        o.append("%s%s = build_list_%s(d, %s);\n" % (t, target, sub["name"], node))


def emit_client_helpers(need, elems):
    o = []
    for e in need:
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
        o.append("static void *get_%s(const aj_doc *d, int n)\n{\n" % e["name"])
        o.append("\tif (n < 0 || aj_is_null(d, n))\n\t\treturn NULL;\n")
        o.append("\t%s v = (%s)calloc(1, sizeof(*v));\n" % (ct, ct))
        o.append("\tif (!v)\n\t\treturn NULL;\n")
        for f in rec["fields"]:
            node = "aj_member(d, n, %s)" % qq(f["name"])
            cli_get_value(o, "v->" + f["name"], f["kind"], f["c_type"], node,
                          1, rec["name"], f["name"])
        o.append("\treturn v;\n}\n\n")

        o.append("static void free_%s(void *p)\n{\n" % e["name"])
        o.append("\t%s v = (%s)p;\n\tif (!v)\n\t\treturn;\n" % (ct, ct))
        for f in rec["fields"]:
            if f["kind"] == "string":
                o.append("\tfree(v->%s);\n" % f["name"])
            elif f["kind"] == "struct_ptr":
                sub = elem_of(f["c_type"])
                o.append("\tfree_%s(v->%s);\n" % (sub["name"], f["name"]))
            elif f["kind"] == "list":
                sub = field_list_elem(rec["name"], f["name"])
                o.append("\tarpc_free_list(v->%s, %s);\n"
                         % (f["name"], elem_free_fn(sub)))
            # handle fields hold ids, not memory
        o.append("\tfree(v);\n}\n\n")

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
    return "".join(o)


def elem_free_fn(e):
    if e["kind"] == "string":
        return "free"
    if e["kind"] == "handle":
        return "NULL"          # ids, nothing to release
    return "free_" + e["name"]


def emit_client(gen, need, rin, src_header):
    elems = collect_elems(gen, need)
    o = [BANNER % src_header]
    o.append('#include "arpc_client.h"\n')
    o.append("#include <alpm.h>\n#include <alpm_list.h>\n")
    o.append("#include <stdlib.h>\n\n")
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

        # Batchable accessors never call out on their own. The runtime fetches
        # the whole column for the package's list on first use, so the 1149
        # calls that follow it are plain memory reads.
        if is_batchable(fn):
            pn = fn["params"][0]["name"]
            o.append("%s %s(%s)\n{\n" % (rct, n, sig))
            if rk == "string":
                o.append("\treturn arpc_pkg_field_str(ARPC_ID(%s), " % pn
                         + qq(n) + ");\n")
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
        }[rk]

        first_handle = None
        for p in fn["params"]:
            if p["kind"] == "handle":
                first_handle = p["name"]
                break
        owner = "ARPC_ID(%s)" % first_handle if first_handle else "0"

        o.append("%s %s(%s)\n{\n\tarpc_call c;\n" % (rct, n, sig))

        # A borrowed list is looked up before the call, not after: libalpm
        # hands back the same pointer for repeated calls, and re-fetching
        # would either break that or free a list the caller still holds.
        if rk == "struct_ptr" and struct_own(n) == "borrowed":
            o.append("\t%s cached = (%s)arpc_cached_ptr(%s, %s);\n"
                     % (rct, rct, owner, qq(n)))
            o.append("\tif (cached)\n\t\treturn cached;\n")
        if rk == "list" and list_ownership(n) == "borrowed":
            o.append("\talpm_list_t *cached = arpc_cached_list(%s, %s);\n"
                     % (owner, qq(n)))
            o.append("\tif (cached)\n\t\treturn cached;\n")

        o.append("\tif (!arpc_begin(&c, " + qq(n) + "))\n\t\t%s\n" % fail)

        for p in fn["params"]:
            k, pn = p["kind"], p["name"]
            if k == "string":
                o.append("\tarpc_put_str(&c, %s);\n" % pn)
            elif k in ("scalar", "enum"):
                o.append("\tarpc_put_i64(&c, (long long)%s);\n" % pn)
            elif k == "handle":
                o.append("\tarpc_put_handle(&c, ARPC_ID(%s));\n" % pn)
            elif k == "struct_ptr":
                e = elem_of(p["c_type"])
                o.append("\tput_%s(&c, %s);\n" % (e["name"], pn))
            elif k == "list":
                e = param_elem(n, pn)
                if e["kind"] == "string":
                    o.append("\tarpc_put_str_list(&c, %s);\n" % pn)
                else:
                    o.append("\tarpc_put_handle_list(&c, %s);\n" % pn)

        o.append("\tif (!arpc_invoke(&c)) {\n\t\tarpc_end(&c);\n\t\t%s\n\t}\n"
                 % fail)

        for op in out_params_of(n):
            for p in fn["params"]:
                if p["name"] != op:
                    continue
                if p["kind"] == "ptr_list":
                    e = param_elem(n, op)
                    o.append("\tif (%s)\n\t\t*%s = build_list_%s("
                             "arpc_doc(&c),\n\t\t\t\tarpc_out_node(&c, "
                             % (op, op, e["name"]) + qq(op) + "));\n")
                elif p["kind"] == "ptr_handle":
                    inner = p["c_type"][:-1].strip()
                    o.append("\tif (%s)\n\t\t*%s = (%s)(uintptr_t)"
                             "arpc_out_i64(&c, " % (op, op, inner)
                             + qq(op) + ");\n")
                    if spec.get("creates"):
                        # The connection has to outlive what this made, the
                        # same as a create that returns its handle.
                        o.append("\tif (%s && *%s)\n\t\tarpc_conn_ref();\n"
                                 % (op, op))
                else:
                    inner = p["c_type"].rstrip(" *")
                    o.append("\tif (%s)\n\t\t*%s = (%s)arpc_out_i64(&c, "
                             % (op, op, inner) + qq(op) + ");\n")

        def teardown():
            if spec.get("destroys") and first_handle:
                o.append("\tarpc_purge_owner(ARPC_ID(%s));\n" % first_handle)
                o.append("\tarpc_conn_unref();\n")

        if rk == "void":
            o.append("\tarpc_end(&c);\n")
            teardown()
        elif rk in ("scalar", "enum"):
            o.append("\t%s r = (%s)arpc_ret_i64(&c);\n" % (rct, rct))
            o.append("\tarpc_end(&c);\n")
            teardown()
            o.append("\treturn r;\n")
        elif rk == "string":
            if ret_string_owned(fn):
                o.append("\tchar *r = arpc_take_str(&c);\t/* caller frees */\n")
                o.append("\tarpc_end(&c);\n")
                teardown()
                o.append("\treturn r;\n")
            else:
                o.append("\tconst char *r = arpc_intern_str(&c, %s, " % owner
                         + qq(n) + ");\n")
                o.append("\tarpc_end(&c);\n")
                teardown()
                o.append("\treturn r;\n")
        elif rk == "handle":
            o.append("\t%s r = (%s)(uintptr_t)arpc_ret_handle(&c);\n"
                     % (rct, rct))
            o.append("\tarpc_end(&c);\n")
            if spec.get("creates"):
                o.append("\tif (r)\n\t\tarpc_conn_ref();\n")
            teardown()
            o.append("\treturn r;\n")
        elif rk == "struct_ptr":
            e = elem_of(rct)
            o.append("\t%s r = (%s)get_%s(arpc_doc(&c), "
                     "arpc_ret_node(&c));\n" % (rct, rct, e["name"]))
            o.append("\tarpc_end(&c);\n")
            if struct_own(n) == "borrowed":
                o.append("\t/* Borrowed: lives as long as its owner, and\n"
                         "\t * the caller must not free it. */\n")
                o.append("\tarpc_cache_ptr(%s, %s, r, free_%s);\n"
                         % (owner, qq(n), e["name"]))
            else:
                o.append("\t/* Caller-owned: freed with %s. */\n"
                         % (libalpm_free_fn(rct) or "free"))
            teardown()
            o.append("\treturn r;\n")
        elif rk == "list":
            e = ret_elem(n)
            o.append("\talpm_list_t *r = build_list_%s(arpc_doc(&c), "
                     "arpc_ret_node(&c));\n" % e["name"])
            o.append("\tarpc_end(&c);\n")
            if list_ownership(n) == "borrowed":
                o.append("\t/* Borrowed: lives as long as its owner, and the\n"
                         "\t * caller must not free it. */\n")
                o.append("\tarpc_cache_list(%s, %s, r, %s);\n"
                         % (owner, qq(n), elem_free_fn(e)))
            else:
                o.append("\t/* Caller-owned: freed by the caller, per the\n"
                         "\t * usual libalpm idiom for this function. */\n")
            teardown()
            o.append("\treturn r;\n")
        o.append("}\n\n")
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

    owned = set(f["name"] for f in model["functions"]
                if f["ret"]["kind"] == "string" and ret_string_owned(f))
    expected = set(["alpm_dep_compute_string", "alpm_compute_md5sum",
                    "alpm_compute_sha256sum"])
    if owned != expected:
        print("emit: WARNING caller-owned string returns changed in this "
              "libalpm: %s (expected %s). Re-verify ownership before shipping."
              % (sorted(owned), sorted(expected)), file=sys.stderr)

    gen, skipped = select(model)
    need = records_needed(gen)
    rin = records_input(gen)
    os.makedirs(a.outdir, exist_ok=True)

    files = (
        ("arpc_dispatch.c", emit_server(gen, need, rin, model["header"])),
        ("arpc_stubs.c", emit_client(gen, need, rin, model["header"])),
        ("arpc_handle_tags.h", emit_handle_tags(model)),
    )
    for name, text in files:
        with open(os.path.join(a.outdir, name), "w", encoding="utf-8") as fh:
            fh.write(text)

    reasons = {}
    for _, why in skipped:
        reasons[why] = reasons.get(why, 0) + 1
    report = {
        "generated": len(gen),
        "total": len(model["functions"]),
        "records_materialised": sorted(e["record"]["name"] for e in need),
        "skipped": {"count": len(skipped), "by_reason": reasons,
                    "detail": dict(skipped)},
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
