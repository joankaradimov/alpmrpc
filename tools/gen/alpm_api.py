#!/usr/bin/env python3
"""Parse the *installed* alpm.h into a machine-readable API model.

The model is deliberately dumb: it records what clang can see and nothing
it cannot. Everything a C header does not express -- above all pointer
ownership -- lives in overlay.json and is merged in by emit.py.

Usage:  alpm_api.py --header <alpm.h> --out api_model.json [--libclang <dll>]
"""
import argparse, json, os, sys

import clang.cindex as cx


# --- type classification ---------------------------------------------------

SCALARS = {
    "int", "unsigned int", "long", "unsigned long", "long long",
    "unsigned long long", "short", "unsigned short", "char", "signed char",
    "unsigned char", "size_t", "ssize_t", "off_t", "time_t", "mode_t",
    "uid_t", "gid_t", "double", "float", "_Bool",
}


def _decl_of(t):
    return t.get_canonical().get_declaration()


def _is_opaque_record(t):
    """A struct that alpm.h forward-declares but never defines -> a handle."""
    c = t.get_canonical()
    if c.kind != cx.TypeKind.RECORD:
        return False
    d = c.get_declaration()
    return d is not None and not d.is_definition()


def _is_transparent_record(t):
    c = t.get_canonical()
    if c.kind != cx.TypeKind.RECORD:
        return False
    d = c.get_declaration()
    return d is not None and d.is_definition()


def classify(t):
    """Map a clang type onto a wire kind. Returns (kind, extra)."""
    canon = t.get_canonical()

    if canon.kind == cx.TypeKind.VOID:
        return "void", {}

    if canon.kind == cx.TypeKind.ENUM:
        return "enum", {"enum": canon.get_declaration().spelling or t.spelling}

    if canon.spelling in SCALARS or canon.kind in (
        cx.TypeKind.INT, cx.TypeKind.UINT, cx.TypeKind.LONG,
        cx.TypeKind.ULONG, cx.TypeKind.LONGLONG, cx.TypeKind.ULONGLONG,
        cx.TypeKind.SHORT, cx.TypeKind.USHORT, cx.TypeKind.DOUBLE,
        cx.TypeKind.FLOAT, cx.TypeKind.BOOL, cx.TypeKind.SCHAR,
        cx.TypeKind.UCHAR, cx.TypeKind.CHAR_S,
    ):
        return "scalar", {}

    # A defined struct held by value, not through a pointer -- a field like
    # alpm_sigresult_t's `key`. It is the same record as a struct_ptr field,
    # differing only in where it lives, so it is worth a kind of its own
    # rather than falling through to "unsupported".
    if _is_transparent_record(canon):
        return "record_value", {"type": canon.spelling.replace("struct ", "")}

    if canon.kind == cx.TypeKind.POINTER:
        pointee = canon.get_pointee()
        pc = pointee.get_canonical()

        # char* / const char*
        if pc.kind in (cx.TypeKind.CHAR_S, cx.TypeKind.SCHAR, cx.TypeKind.UCHAR):
            return "string", {"const": pointee.is_const_qualified()}

        # function pointer -> callback, not generatable
        if pc.kind == cx.TypeKind.FUNCTIONPROTO:
            return "callback", {}

        # void*
        if pc.kind == cx.TypeKind.VOID:
            return "opaque_void", {}

        # alpm_list_t*, but NOT alpm_list_t** -- the latter is an out-param
        # and must not be mistaken for a list being passed in. Requiring the
        # pointee to be the record itself is what separates them.
        decl = pc.get_declaration()
        name = (decl.spelling if decl else "") or pointee.spelling
        if pc.kind == cx.TypeKind.RECORD and (
                "alpm_list_t" in pointee.spelling or name == "__alpm_list_t"):
            return "list", {}

        if pc.kind == cx.TypeKind.RECORD:
            if _is_opaque_record(pointee):
                base = pointee.spelling.replace("struct ", "")
                base = base.replace("const ", "").strip()
                # alpm.h forward-declares foreign opaque types too (struct
                # archive, for the mtree stream). Those are not ours to hand
                # out as handles -- name them so select() can skip them with
                # an honest reason instead of inventing a tag.
                if not base.lstrip("_").startswith("alpm_"):
                    return "foreign_handle", {"type": base}
                return "handle", {"type": base}
            if _is_transparent_record(pointee):
                return "struct_ptr", {"type": pointee.spelling.replace("struct ", "")}

        # pointer-to-scalar / pointer-to-enum -> candidate out-param
        if pc.kind == cx.TypeKind.ENUM:
            return "ptr_enum", {"enum": pc.get_declaration().spelling}
        if pc.spelling in SCALARS or pc.kind in (
            cx.TypeKind.INT, cx.TypeKind.UINT, cx.TypeKind.LONG,
            cx.TypeKind.ULONG, cx.TypeKind.ULONGLONG, cx.TypeKind.LONGLONG,
        ):
            return "ptr_scalar", {"base": pc.spelling}

        # char** , alpm_list_t** , struct**
        if pc.kind == cx.TypeKind.POINTER:
            inner, inner_extra = classify(pointee)
            return "ptr_" + inner, inner_extra

        return "unsupported", {"why": "pointer to " + pointee.spelling}

    return "unsupported", {"why": canon.spelling}


# --- extraction ------------------------------------------------------------

def extract(header, args):
    idx = cx.Index.create()
    tu = idx.parse(
        header, args=args,
        options=cx.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES,
    )
    fatal = [d for d in tu.diagnostics if d.severity >= cx.Diagnostic.Error]
    if fatal:
        for d in fatal:
            print("clang: %s" % d.spelling, file=sys.stderr)
        raise SystemExit("alpm_api: refusing to emit a model from a header "
                         "that did not parse cleanly")

    hdr_real = os.path.realpath(header)
    funcs, records, enums, callbacks = [], [], [], []

    for c in tu.cursor.get_children():
        if not c.location.file:
            continue
        if os.path.realpath(c.location.file.name) != hdr_real:
            continue

        if c.kind == cx.CursorKind.FUNCTION_DECL:
            rk, rx = classify(c.result_type)
            params = []
            for a in c.get_arguments():
                pk, px = classify(a.type)
                params.append({
                    "name": a.spelling or ("arg%d" % len(params)),
                    "c_type": a.type.spelling,
                    "kind": pk,
                    **({"extra": px} if px else {}),
                })
            funcs.append({
                "name": c.spelling,
                "variadic": bool(c.type.is_function_variadic()),
                "ret": {"c_type": c.result_type.spelling, "kind": rk,
                        **({"extra": rx} if rx else {})},
                "params": params,
            })

        # A union is a record whose members overlap. Which one is live is not
        # something clang can say -- that is the overlay's business -- but the
        # members and their types are exactly a record's fields.
        elif c.kind in (cx.CursorKind.STRUCT_DECL,
                        cx.CursorKind.UNION_DECL) and c.is_definition():
            fields = []
            for f in c.get_children():
                if f.kind != cx.CursorKind.FIELD_DECL:
                    continue
                fk, fx = classify(f.type)
                fields.append({"name": f.spelling, "c_type": f.type.spelling,
                               "kind": fk, **({"extra": fx} if fx else {})})
            records.append({"name": c.spelling, "fields": fields,
                            "union": c.kind == cx.CursorKind.UNION_DECL})

        # A callback is a function pointer typedef. Its signature is the
        # whole of what a trampoline has to marshal, and clang has it -- the
        # parameter names included, which the type alone does not carry.
        elif c.kind == cx.CursorKind.TYPEDEF_DECL:
            uc = c.underlying_typedef_type.get_canonical()
            if uc.kind != cx.TypeKind.POINTER:
                continue
            proto = uc.get_pointee()
            if proto.kind != cx.TypeKind.FUNCTIONPROTO:
                continue
            names = [p.spelling for p in c.get_children()
                     if p.kind == cx.CursorKind.PARM_DECL]
            params = []
            for i, at in enumerate(proto.argument_types()):
                pk, px = classify(at)
                params.append({
                    "name": names[i] if i < len(names) else "arg%d" % i,
                    "c_type": at.spelling, "kind": pk,
                    **({"extra": px} if px else {}),
                })
            rk, rx = classify(proto.get_result())
            callbacks.append({
                "name": c.spelling,
                "ret": {"c_type": proto.get_result().spelling, "kind": rk,
                        **({"extra": rx} if rx else {})},
                "params": params,
            })

        elif c.kind == cx.CursorKind.ENUM_DECL and c.is_definition():
            enums.append({
                "name": c.spelling,
                "values": [{"name": e.spelling, "value": e.enum_value}
                           for e in c.get_children()
                           if e.kind == cx.CursorKind.ENUM_CONSTANT_DECL],
            })

    return {"header": header, "functions": funcs,
            "records": records, "enums": enums,
            "callbacks": callbacks}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--header", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--libclang")
    ap.add_argument("--clang-arg", action="append", default=[])
    a = ap.parse_args()

    if a.libclang:
        cx.Config.set_library_file(a.libclang)

    model = extract(a.header, ["-x", "c", "-std=c11"] + a.clang_arg)

    kinds = {}
    for f in model["functions"]:
        for k in [f["ret"]["kind"]] + [p["kind"] for p in f["params"]]:
            kinds[k] = kinds.get(k, 0) + 1

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump(model, fh, indent=1, sort_keys=True)

    print("alpm_api: %d functions, %d records, %d enums -> %s"
          % (len(model["functions"]), len(model["records"]),
             len(model["enums"]), a.out))
    print("alpm_api: type kinds seen: %s"
          % ", ".join("%s=%d" % kv for kv in sorted(kinds.items())))


if __name__ == "__main__":
    main()
