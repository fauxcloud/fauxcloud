"""gen_fwd_roots.py -- the anchors of forwarding (seven roots + two cross-signed intermediates) as BearSSL
trust anchors (subject DN + public key) for the firmware.

Source: tools/fwd_roots.pem (committed): the roots, picked from a Mozilla-derived CA store, and the two
cross-signed certificates, taken from the servers, all by their SHA-256
fingerprint (the table below is the pin; a root that does not match it is refused). Output (generated, committed):
firmware/fauxcloud/src/fwd_roots.h (masks, the extern), src/fwd_roots.c (the br_x509_trust_anchor structs) and
test/host/fwd_roots_der.inc (the DER the host test decodes to check both agree). A root swap (GlobalSign Root CA ends 2028-01-28) is: put
the new root's fingerprint in ROOTS, import, regenerate.

    python -I tools/gen_fwd_roots.py                       # regenerate the three outputs from fwd_roots.pem
    python -I tools/gen_fwd_roots.py --check               # CI: exit 1 if an output or fwd_roots.pem is stale
    python -I tools/gen_fwd_roots.py --import FILE.pem     # rebuild fwd_roots.pem from a CA bundle (cacert.pem, cacrt_all.pem)
    python -I tools/gen_fwd_roots.py --import-windows      # ... or from the Windows root store (AuthRoot / ROOT)
    python -I tools/gen_fwd_roots.py --list                # subject, SHA-256, NotAfter of each

The pinned core (esp32 3.3.12) ships no cacrt_all.pem (its bundle keeps only subject + public key), so the PEM file is
the committed source. stdlib only; run isolated.
"""
import base64
import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PEM = ROOT / "tools" / "fwd_roots.pem"
OUT = ROOT / "firmware" / "fauxcloud" / "src" / "fwd_roots.h"
OUT_C = ROOT / "firmware" / "fauxcloud" / "src" / "fwd_roots.c"
OUT_DER = ROOT / "test" / "host" / "fwd_roots_der.inc"

# (C name, mask bit, who uses it, SHA-256 of the DER)
ROOTS = [
    ("STARFIELD_G2", "Starfield Services Root Certificate Authority - G2", "WU, PWSweather",
     "568d6905a2c88708a4b3025190edcfedb1974a606a13c6e5290fcb2ae63edab5"),
    ("GLOBALSIGN", "GlobalSign Root CA", "Windy",
     "ebd41040e4bb3ec742c9e381d31ef2a41a48b6685c96e7cef3c1df6cd4331c99"),
    ("USERTRUST_RSA", "USERTrust RSA Certification Authority", "Weathercloud (v1.1), OpenWeatherMap",
     "e793c9b02fd8aa13e21c31228accb08119643b749c898964b1746d46c3d4cbd2"),
    ("HARICA_2015", "Hellenic Academic and Research Institutions RootCA 2015", "WOW-BE",
     "a040929a02ce53b4acf4f2ffc6981ce4496f755e6d45fe0b2a692bcd52523f36"),
    ("ISRG_X1", "ISRG Root X1", "AWEKAS, openSenseMap, most custom hosts",
     "96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6"),
    ("DIGICERT_G2", "DigiCert Global Root G2", "WOW-UK, METAR later",
     "cb3ccbb76031e5e0138f8dd39a23f9de47ffc35e43c1144cea27d46a5ab1cb5f"),
    # The servers' own cross-signed intermediates as extra anchors: an anchor is subject + key, so the
    # chain ends one step lower (one RSA signature check fewer); kept for the shorter check.
    # Taken from the servers, pinned like the roots.
    ("AMAZON_CA1_X", "Amazon Root CA 1 (cross-signed by Starfield Services G2)", "WU, PWSweather",
     "87dcd4dc74640a322cd205552506d1be64f12596258096544986b4850bc72706"),
    ("GTS_R1_X", "GTS Root R1 (cross-signed by GlobalSign Root CA)", "Windy",
     "3ee0278df71fa3c125c4cd487f01d774694e6fc57e0cd94c24efd769133918e5"),
    # api.weather.com's chain ends here (the WU forecast line)
    ("GLOBALSIGN_R3", "GlobalSign Root CA - R3", "the WU forecast (api.weather.com)",
     "cbb522d7b7f127ad6a0113865bdf1cd4102e7d0759af635a7cf4720dc963c53b"),
]


# ---------------------------------------------------------------- a minimal DER walk (validity, subject CN)
def tlv(b, i):
    tag = b[i]
    n = b[i + 1]
    i += 2
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(b[i:i + k], "big")
        i += k
    return tag, i, i + n  # tag, value start, value end


def children(b, start, end):
    out, i = [], start
    while i < end:
        tag, s, e = tlv(b, i)
        out.append((tag, s, e))
        i = e
    return out


def describe(der):
    _, s, e = tlv(der, 0)                       # Certificate
    tag, ts, te = children(der, s, e)[0]        # tbsCertificate
    f = children(der, ts, te)
    if f[0][0] == 0xA0:                         # [0] version
        f = f[1:]
    validity, subject = f[3], f[4]
    nb, na = children(der, validity[1], validity[2])

    def t(x):
        v = der[x[1]:x[2]].decode()
        if x[0] == 0x17:                        # UTCTime YYMMDDHHMMSSZ
            v = ("19" if int(v[:2]) >= 50 else "20") + v
        return f"{v[0:4]}-{v[4:6]}-{v[6:8]}"
    cn = ""
    for _, rs, re_ in children(der, subject[1], subject[2]):         # RDN sets
        for _, as_, ae in children(der, rs, re_):                      # AttributeTypeAndValue
            (ot, os_, oe), (vt, vs, ve) = children(der, as_, ae)
            if der[os_:oe] == bytes([0x55, 0x04, 0x03]):               # commonName
                cn = der[vs:ve].decode("utf-8", "replace")
            elif der[os_:oe] == bytes([0x55, 0x04, 0x0B]) and not cn:  # OU (GlobalSign Root CA has a CN too)
                pass
    return cn, t(nb), t(na)


def pem_certs(text):
    for m in re.finditer(r"-----BEGIN CERTIFICATE-----(.+?)-----END CERTIFICATE-----", text, re.S):
        yield base64.b64decode("".join(m.group(1).split()))


def to_pem(der):
    b = base64.b64encode(der).decode()
    return "-----BEGIN CERTIFICATE-----\n" + "\n".join(b[i:i + 64] for i in range(0, len(b), 64)) + \
        "\n-----END CERTIFICATE-----\n"


def pick(ders):
    by = {hashlib.sha256(d).hexdigest(): d for d in ders}
    missing = [r[1] for r in ROOTS if r[3] not in by]
    if missing:
        raise SystemExit("gen_fwd_roots: not in the source: " + "; ".join(missing))
    return [by[r[3]] for r in ROOTS]


def pem_text(ders):
    out = ["# The roots and cross-signed intermediates of fauxcloud forwarding, generated by tools/gen_fwd_roots.py --import; source of",
           "# firmware/fauxcloud/src/fwd_roots.h. Pinned by SHA-256 in gen_fwd_roots.py. Public CA certificates.", ""]
    for r, d in zip(ROOTS, ders):
        cn, nb, na = describe(d)
        out.append(f"# {r[1]} -- {r[2]}")
        out.append(f"# SHA-256 {r[3]}  NotBefore {nb}  NotAfter {na}  {len(d)} B DER")
        out.append(to_pem(d))
    return "\n".join(out)


def spki(der):
    """The subject DN (the whole Name TLV, as BearSSL hashes it) and the public key of a certificate:
    ("rsa", n, e) with leading zero bytes dropped, or ("ec", curve id, point)."""
    _, s, e = tlv(der, 0)
    _, ts, te = children(der, s, e)[0]
    f = children(der, ts, te)
    if f[0][0] == 0xA0:
        f = f[1:]
    subj, key = f[4], f[5]
    dn = der[f[3][2]:subj[2]]  # the Name TLV: from the end of Validity (its predecessor) to the end of the subject
    alg, bits = children(der, key[1], key[2])
    oids = children(der, alg[1], alg[2])
    oid = der[oids[0][1]:oids[0][2]]
    raw = der[bits[1] + 1:bits[2]]  # BIT STRING: skip the unused-bits byte
    if oid == bytes.fromhex("2a864886f70d010101"):  # rsaEncryption
        _, s2, e2 = tlv(raw, 0)
        n, ex = children(raw, s2, e2)
        return dn, ("rsa", raw[n[1]:n[2]].lstrip(b"\0"), raw[ex[1]:ex[2]].lstrip(b"\0"))
    if oid == bytes.fromhex("2a8648ce3d0201"):  # id-ecPublicKey
        curves = {"2a8648ce3d030107": 23, "2b81040022": 24, "2b81040023": 25}  # P-256 / P-384 / P-521 (BR_EC_*)
        c = der[oids[1][1]:oids[1][2]].hex()
        if c not in curves:
            raise SystemExit("gen_fwd_roots: unsupported curve " + c)
        return dn, ("ec", curves[c], raw)
    raise SystemExit("gen_fwd_roots: unsupported key algorithm " + oid.hex())


def carr(name, data):
    out = [f"static const unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 24):
        out.append("    " + ",".join("0x%02x" % x for x in data[i:i + 24]) + ",")
    out.append("};")
    return out


def header(ders):
    total = sum(len(d) for d in ders)
    lines = ["// fwd_roots.h -- GENERATED by tools/gen_fwd_roots.py from tools/fwd_roots.pem. Do not edit.",
             "// The trust anchors of encrypted forwarding (BearSSL takes DN + public key, the data is in",
             "// fwd_roots.c). Seven roots + two cross-signed intermediates: as anchors they end a chain one link earlier, one RSA",
             "// verify fewer -- a TLS probe of each service shows whether its chain ends there. A preset passes its own mask,",
             "// a custom HTTPS host all of them. Linked only by FWD_TLS=1 builds. BearSSL-free: the structs are",
             "// declared in src/tlsc_trust.h, which only src/tlsc.cpp and the host tests include.",
             "#pragma once",
             "#include <cstdint>",
             "",
             "namespace fc {",
             ""]
    for i, (r, d) in enumerate(zip(ROOTS, ders)):
        cn, nb, na = describe(d)
        flag = f"  <-- renew: ends {na[:4]}" if na[:4] < "2030" else ""
        lines.append(f"// {i}: {r[1]} ({r[2]}); SHA-256 {r[3]}; NotAfter {na}{flag}")
    lines.append("constexpr int FWD_ROOT_COUNT = %d;  // from %d B of DER certificates" % (len(ROOTS), total))
    for i, r in enumerate(ROOTS):
        lines.append(f"constexpr uint16_t FWD_ROOTBIT_{r[0]} = 1u << {i};")
    lines.append("constexpr uint16_t FWD_ROOTS_ALL = 0x%03x;" % ((1 << len(ROOTS)) - 1))
    assert len(ROOTS) <= 16, "root masks are uint16_t"
    lines.append("")
    lines.append("}  // namespace fc")
    return "\n".join(lines) + "\n"


def csource(ders):
    lines = ["/* fwd_roots.c -- GENERATED by tools/gen_fwd_roots.py from tools/fwd_roots.pem. Do not edit.",
             "   BearSSL trust anchors (DN DER + public key) of encrypted forwarding; see fwd_roots.h. C for the designated",
             "   initialisers of the key union. */",
             '#include "bearssl/bearssl_x509.h"',
             ""]
    entries, nbytes = [], 0
    for r, d in zip(ROOTS, ders):
        dn, key = spki(d)
        lines.append(f"/* {r[1]} */")
        lines += carr(f"TA_{r[0]}_DN", dn)
        nbytes += len(dn)
        if key[0] == "rsa":
            lines += carr(f"TA_{r[0]}_N", key[1]) + carr(f"TA_{r[0]}_E", key[2])
            nbytes += len(key[1]) + len(key[2])
            k = (f"{{ BR_KEYTYPE_RSA, {{ .rsa = {{ (unsigned char *)TA_{r[0]}_N, sizeof TA_{r[0]}_N, "
                 f"(unsigned char *)TA_{r[0]}_E, sizeof TA_{r[0]}_E }} }} }}")
        else:
            lines += carr(f"TA_{r[0]}_Q", key[2])
            nbytes += len(key[2])
            k = (f"{{ BR_KEYTYPE_EC, {{ .ec = {{ {key[1]}, (unsigned char *)TA_{r[0]}_Q, sizeof TA_{r[0]}_Q }} }} }}")
        entries.append(f"    {{ {{ (unsigned char *)TA_{r[0]}_DN, sizeof TA_{r[0]}_DN }}, BR_X509_TA_CA, {k} }},")
    lines.append("")
    lines.append(f"/* {len(ROOTS)} anchors, {nbytes} B of DN and key arrays */")
    lines.append(f"const br_x509_trust_anchor FWD_TAS[{len(ROOTS)}] = {{")
    lines += entries
    lines.append("};")
    return "\n".join(lines) + "\n", nbytes


def derinc(ders):
    lines = ["// fwd_roots_der.inc -- GENERATED by tools/gen_fwd_roots.py. Do not edit. The DER certificates behind",
             "// src/fwd_roots.c, for test/host/test_tlsc.inc (br_x509_decoder must give the same DN and key). Host tests only.",
             ""]
    for r, d in zip(ROOTS, ders):
        lines += carr(f"FWD_ROOT_DER_{r[0]}", d)
    lines.append("struct FwdRootDer {")
    lines.append("  const unsigned char* der;")
    lines.append("  size_t len;")
    lines.append("};")
    lines.append("static const FwdRootDer FWD_ROOT_DERS[] = {")
    for r in ROOTS:
        lines.append(f"    {{FWD_ROOT_DER_{r[0]}, sizeof FWD_ROOT_DER_{r[0]}}},")
    lines.append("};")
    return "\n".join(lines) + "\n"


def from_pem_file():
    return pick(list(pem_certs(PEM.read_text(encoding="ascii"))))


def main():
    args = sys.argv[1:]
    if args[:1] == ["--import"] and len(args) == 2:
        ders = pick(list(pem_certs(pathlib.Path(args[1]).read_text(encoding="utf-8", errors="replace"))))
        PEM.write_text(pem_text(ders), encoding="ascii", newline="\n")
        print("gen_fwd_roots: wrote", PEM.relative_to(ROOT))
        return
    if args[:1] == ["--import-windows"]:
        import ssl
        ders = []
        for store in ("ROOT", "AuthRoot", "CA"):
            try:
                ders += [d for d, enc, _ in ssl.enum_certificates(store) if enc == "x509_asn"]
            except (OSError, AttributeError):
                pass
        PEM.write_text(pem_text(pick(ders)), encoding="ascii", newline="\n")
        print("gen_fwd_roots: wrote", PEM.relative_to(ROOT))
        return
    ders = from_pem_file()
    if args[:1] == ["--list"]:
        for r, d in zip(ROOTS, ders):
            cn, nb, na = describe(d)
            print(f"{r[1]:58} {len(d):5} B  NotAfter {na}  SHA-256 {r[3]}")
        return
    text = header(ders)
    ctext, nbytes = csource(ders)
    dtext = derinc(ders)
    if args[:1] == ["--check"]:
        ok = all(p.exists() and p.read_text(encoding="utf-8") == t for p, t in ((OUT, text), (OUT_C, ctext), (OUT_DER, dtext)))
        ok = ok and PEM.read_text(encoding="ascii") == pem_text(ders)
        print("gen_fwd_roots: " + ("up to date" if ok else "STALE: run python -I tools/gen_fwd_roots.py"))
        sys.exit(0 if ok else 1)
    for p, t in ((OUT, text), (OUT_C, ctext), (OUT_DER, dtext)):
        p.write_text(t, encoding="utf-8", newline="\n")
    print("gen_fwd_roots: wrote %s, %s, %s (%d anchors, %d B of DN + key arrays, from %d B DER)" % (
        OUT.relative_to(ROOT), OUT_C.relative_to(ROOT), OUT_DER.relative_to(ROOT), len(ders), nbytes, sum(map(len, ders))))


if __name__ == "__main__":
    main()
