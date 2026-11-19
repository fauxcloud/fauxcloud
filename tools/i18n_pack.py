"""i18n_pack.py -- the language pack source, format 2 (web/lang/<lang>.json), and how a build resolves it. Stdlib only;
loaded by path from gen_web_assets.py (the packs), i18n_inventory.py (the coverage check)
and the one-time migration to format 2.

  {"_meta": {"name": "Polski", "format": 2},
   "cmn.close":      {"en": "Close", "pl": "Zamknij"},          # the common table: short labels of one role
   "cmn.calm#sent":  {"en": "Calm.", "pl": "Cisza."},           # optional: an explicit variant wins over the transform
   "index.close":    {"en": "Close", "pl": "@cmn.close"},       # a reference
   "dash.bf1":       {"en": "Light air", "pl": "@cmn.light_air#cap"},
   "dash.e_sub":     {"en": "...", "pl": "..."}}                # a literal (also the override where a context needs it)

A reference is "@cmn.<id>", "@cmn.<id>#cap" or "@cmn.<id>#sent", and nothing else: the two whole-phrase transforms are
cap (the first letter upper case) and sent (cap + a final period when the text has no end mark). The same transform
applied to the common entry's English must give the key's English, so a reference cannot change what a key means. The
build resolves every reference into the packs (no runtime cost). Other fields (proposed, hint, _prev, stale) are notes
for the reviewer; the build ignores them.
"""
import re

FORMAT = 2
REF = re.compile(r"^@cmn\.([a-z0-9_]+)(?:#(cap|sent))?$")
KEY = re.compile(r"^(?:[a-z0-9-]+\.[A-Za-z0-9_]+|cmn\.[a-z0-9_]+(?:#(?:cap|sent))?)$")
END = re.compile(r"[.!?…]$")


def cap(s):
    return s[:1].upper() + s[1:]


def sent(s):
    s = cap(s)
    return s if END.search(s) else s + "."


TRANSFORM = {None: lambda s: s, "cap": cap, "sent": sent}


def is_ref(v):
    return isinstance(v, str) and v.startswith("@")


def lookup(d, ref, field):
    """(text, None) or (None, problem) for one reference, in one field ("en" or the language)."""
    m = REF.match(ref)
    if not m:
        return None, "not a reference (only @cmn.<id>, @cmn.<id>#cap, @cmn.<id>#sent): %s" % ref
    cid, how = m.group(1), m.group(2)
    base = d.get("cmn." + cid)
    if base is None:
        return None, "dangling reference %s: no cmn.%s" % (ref, cid)
    var = d.get("cmn.%s#%s" % (cid, how)) if how else None
    if var and var.get(field):
        return var[field], None
    t = base.get(field)
    if not isinstance(t, str):
        return None, "cmn.%s has no %s text" % (cid, field)
    return (TRANSFORM[how](t) if t else ""), None


def resolve(d, lang):
    """({"<file>.<key>": text or count entry}, [(key, problem)]): the entries' texts in `lang` with every reference
    resolved (the common table itself is not a namespace of any page), and the problems found on the way."""
    out, bad = {}, []
    for k, e in d.items():
        if k == "_meta" or k.startswith("cmn."):
            continue
        if not KEY.match(k) or not isinstance(e, dict):
            bad.append((k, "bad entry"))
            continue
        v = e.get(lang)
        if is_ref(v):
            t, why = lookup(d, v, lang)
            if why:
                bad.append((k, why))
                continue
            en, why = lookup(d, v, "en")
            if why or en != e.get("en"):
                bad.append((k, "%s is %r in English, the key is %r" % (v, en, e.get("en"))))
                continue
            v = t
        if v:
            out[k] = v
    return out, bad


def refs(d, lang):
    """{cmn id: [keys referencing it]} (unused common entries are worth a warning)."""
    out = {k[4:]: [] for k in d if k.startswith("cmn.") and "#" not in k}
    for k, e in d.items():
        v = isinstance(e, dict) and e.get(lang)
        m = is_ref(v) and REF.match(v)
        if m and m.group(1) in out:
            out[m.group(1)].append(k)
    return out
