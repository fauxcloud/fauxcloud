"""vendor_bearssl.py -- the upstream BearSSL files this firmware compiles, pinned by commit, with a hash
manifest. BearSSL (MIT, (c) 2016 Thomas Pornin) is the TLS 1.2 client of encrypted forwarding (FWD_TLS=1 builds).

    python -I tools/vendor_bearssl.py --commit <40-hex>        # clone upstream into a new empty temp dir, copy, write VENDORED.txt
    python -I tools/vendor_bearssl.py --check                  # offline (CI): both folders match VENDORED.txt, nothing extra/missing
    python -I tools/vendor_bearssl.py --verify-upstream        # network, manual: re-fetch the pinned commit and compare every file
    python -I tools/vendor_bearssl.py --commit <h> --all       # derivation only: every upstream src/*.c into the device folder, so the
                                                               # link map of one build shows which objects carry bytes (then frozen)

Device list -> firmware/fauxcloud/src/bearssl/ (flattened: the 294 upstream basenames are unique, and upstream's quote includes --
"inner.h", "config.h", "bearssl_*.h" -- resolve in the same folder; BearSSL's config.h never shadows our src/config.h, since a
quote include searches the including file's own folder first). Host-only list -> test/host/bearssl/ (the BearSSL server engine and
the decoders the in-process test server needs; compiled by test/host/run.* only, with -I to the device folder). Upstream files are
never patched: a needed change is a question for the maintainers. The clone is untrusted data: nothing in it is run.
Line ends: the clone runs with core.autocrlf=false / core.eol=lf, .gitattributes marks both folders -text, so the
manifest hashes upstream's bytes on every machine. Bump = rerun --commit with the new hash, review the upstream diff of the vendored files only, host tests,
one OTA. stdlib only; run isolated (python -I).
"""
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEV = ROOT / "firmware" / "fauxcloud" / "src" / "bearssl"
HOST = ROOT / "test" / "host" / "bearssl"
MANIFEST = DEV / "VENDORED.txt"
UPSTREAM = "https://www.bearssl.org/git/BearSSL"
PIN = "7bea48e5e850ab4cafbe68d3765cdaba13a86d6f"  # upstream master of 2026-04-06; a bump re-checks the TLS heap use

# Every header the sources include (copied to the device folder; the host build reaches them by -I).
HEADERS = ["inc/bearssl.h", "inc/bearssl_aead.h", "inc/bearssl_block.h", "inc/bearssl_ec.h", "inc/bearssl_hash.h",
           "inc/bearssl_hmac.h", "inc/bearssl_kdf.h", "inc/bearssl_pem.h", "inc/bearssl_prf.h", "inc/bearssl_rand.h",
           "inc/bearssl_rsa.h", "inc/bearssl_ssl.h", "inc/bearssl_x509.h", "src/inner.h", "src/config.h"]

# The objects the explicit client profile of src/tlsc.cpp links: the 57 archive members that carry bytes in the link
# map of the first FWD_TLS=1 build (derived with --all), frozen here. Grouped by what they serve.
DEVICE_C = [
    # the TLS 1.2 client engine and the handshake, the record layers (GCM, ChaCha20-Poly1305), the PRFs, the sslio wrapper
    "src/ssl/ssl_engine.c", "src/ssl/ssl_hs_client.c", "src/ssl/ssl_client.c", "src/ssl/ssl_io.c",
    "src/ssl/ssl_rec_gcm.c", "src/ssl/ssl_rec_chapol.c", "src/ssl/prf.c", "src/ssl/prf_sha256.c", "src/ssl/prf_sha384.c",
    # certificate path validation against our anchors
    "src/x509/x509_minimal.c",
    # ECDHE (P-256 m31, P-384/521 i31, X25519) and ECDSA verification
    "src/ec/ec_all_m31.c", "src/ec/ec_p256_m31.c", "src/ec/ec_prime_i31.c", "src/ec/ec_c25519_m31.c",
    "src/ec/ec_secp256r1.c", "src/ec/ec_secp384r1.c", "src/ec/ec_secp521r1.c", "src/ec/ecdsa_atr.c",
    "src/ec/ecdsa_i31_bits.c", "src/ec/ecdsa_i31_vrfy_asn1.c", "src/ec/ecdsa_i31_vrfy_raw.c",
    # RSA signature verification (certificates, ServerKeyExchange)
    "src/rsa/rsa_i31_pkcs1_vrfy.c", "src/rsa/rsa_i31_pub.c", "src/rsa/rsa_pkcs1_sig_unpad.c",
    # the i31 big-integer core under both
    "src/int/i31_add.c", "src/int/i31_bitlen.c", "src/int/i31_decmod.c", "src/int/i31_decode.c", "src/int/i31_encode.c",
    "src/int/i31_fmont.c", "src/int/i31_iszero.c", "src/int/i31_modpow.c", "src/int/i31_modpow2.c", "src/int/i31_montmul.c",
    "src/int/i31_muladd.c", "src/int/i31_ninv31.c", "src/int/i31_rshift.c", "src/int/i31_sub.c", "src/int/i31_tmont.c",
    "src/int/i32_div32.c",
    # symmetric: AES (constant-time, CTR), GHASH, ChaCha20, Poly1305
    "src/symcipher/aes_ct.c", "src/symcipher/aes_ct_ctr.c", "src/symcipher/aes_ct_enc.c", "src/symcipher/chacha20_ct.c",
    "src/symcipher/poly1305_ctmul.c", "src/hash/ghash_ctmul.c",
    # hashes, HMAC, the DRBG the engine seeds from our entropy
    "src/hash/sha2small.c", "src/hash/sha2big.c", "src/hash/multihash.c", "src/mac/hmac.c", "src/rand/hmac_drbg.c",
    "src/rand/sysrng.c",
    # byte codecs, constant-time copy
    "src/codec/ccopy.c", "src/codec/dec32be.c", "src/codec/dec64be.c", "src/codec/enc32be.c", "src/codec/enc64be.c",
]

# Host tests only (test/host/test_tlsc.inc): the in-process BearSSL server (EC P-256 key, ECDHE_ECDSA suites) and the X.509
# decoder that checks the generated anchors against the certificates.
HOST_C = [
    # the server engine and handshake, one EC certificate + key
    "src/ssl/ssl_server.c", "src/ssl/ssl_hs_server.c", "src/ssl/ssl_scert_single_ec.c", "src/ssl/ssl_hashes.c",
    # ECDSA signing for the server's ServerKeyExchange
    "src/ec/ecdsa_i31_sign_asn1.c", "src/ec/ecdsa_i31_sign_raw.c", "src/ec/ecdsa_rta.c",
    # the X.509 decoder (anchor DN + key from the DER certificates)
    "src/x509/x509_decoder.c",
    # the test PKI made at run time: RSA-2048 keygen + PKCS#1 signing, P-256 keygen
    "src/rsa/rsa_i31_keygen.c", "src/rsa/rsa_i31_keygen_inner.c", "src/rsa/rsa_i31_pkcs1_sign.c", "src/rsa/rsa_i31_priv.c",
    "src/rsa/rsa_pkcs1_sig_pad.c", "src/ec/ec_keygen.c", "src/ec/ec_pubkey.c",
    "src/int/i31_decred.c", "src/int/i31_mulacc.c", "src/int/i31_moddiv.c", "src/int/i31_reduce.c",
]

LICENSE = "LICENSE.txt"


def sha(p):
    return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()


def git(*args, cwd=None):
    # upstream's bytes, whatever this machine's autocrlf says (a CRLF checkout would hash CRLF bytes)
    subprocess.run(["git", "-c", "core.autocrlf=false", "-c", "core.eol=lf", *args], cwd=cwd, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def fetch(commit):
    """A fresh clone at `commit` in a new empty temp dir; returns (dir, commit date)."""
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="bearssl-"))
    up = tmp / "up"
    git("clone", "--quiet", "--no-checkout", UPSTREAM, str(up))
    git("-c", "advice.detachedHead=false", "checkout", "--quiet", commit, cwd=up)
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=up, check=True, capture_output=True, text=True).stdout.strip()
    if head != commit:
        raise SystemExit("vendor_bearssl: %s resolved to %s" % (commit, head))
    date = subprocess.run(["git", "log", "-1", "--format=%cI"], cwd=up, check=True, capture_output=True,
                          text=True).stdout.strip()
    return tmp, up, date


def plan(up, every=False):
    """(upstream rel path, destination) for every vendored file."""
    out = []
    dev_c = sorted(str(p.relative_to(up)).replace("\\", "/") for p in (up / "src").rglob("*.c")) if every else DEVICE_C
    for rel in HEADERS + dev_c:
        out.append((rel, DEV / pathlib.PurePosixPath(rel).name))
    for rel in HOST_C:
        out.append((rel, HOST / pathlib.PurePosixPath(rel).name))
    out.append((LICENSE, DEV / LICENSE))
    names = [d for _, d in out]
    if len(set(names)) != len(names):
        raise SystemExit("vendor_bearssl: two upstream files flatten to one name")
    return out


def vendor(commit, every):
    tmp, up, date = fetch(commit)
    try:
        for d in (DEV, HOST):
            if d.exists():
                shutil.rmtree(d)
            d.mkdir(parents=True)
        lines = ["# BearSSL vendored by tools/vendor_bearssl.py -- do not edit; checked by --check (CI)",
                 "url " + UPSTREAM, "commit " + commit, "date " + date]
        for rel, dst in plan(up, every):
            src = up / rel
            if not src.is_file():
                raise SystemExit("vendor_bearssl: %s not in upstream %s" % (rel, commit))
            if b"\r\n" in src.read_bytes() and rel != LICENSE:
                raise SystemExit("vendor_bearssl: %s has CRLF bytes: the clone converted line ends" % rel)
            shutil.copyfile(src, dst)
            kind = "host" if dst.parent == HOST else "device"
            lines.append("%s %s -> %s %s" % (kind, rel, dst.relative_to(ROOT).as_posix(), sha(dst)))
        MANIFEST.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
        n_dev = sum(1 for p in DEV.iterdir() if p.suffix == ".c")
        n_host = sum(1 for p in HOST.iterdir() if p.suffix == ".c")
        print("vendor_bearssl: %s (%s): %d device .c, %d host .c, %d headers -> %s" % (
            commit[:12], date, n_dev, n_host, len(HEADERS), MANIFEST.relative_to(ROOT).as_posix()))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def read_manifest():
    meta, files = {}, []
    for line in MANIFEST.read_text(encoding="ascii").splitlines():
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if parts[0] in ("url", "commit", "date"):
            meta[parts[0]] = parts[1]
        else:  # kind rel -> dst sha
            files.append((parts[0], parts[1], parts[3], parts[4]))
    return meta, files


def check():
    if not MANIFEST.is_file():
        print("vendor_bearssl: no %s" % MANIFEST.relative_to(ROOT).as_posix())
        return 1
    meta, files = read_manifest()
    bad = []
    if meta.get("commit") != PIN:
        bad.append("manifest commit %s != PIN %s" % (meta.get("commit"), PIN))
    name = lambda r: pathlib.PurePosixPath(r).name  # noqa: E731
    want = {(r, DEV / name(r)) for r in HEADERS + DEVICE_C + [LICENSE]} | {(r, HOST / name(r)) for r in HOST_C}
    have = {(r, ROOT / d) for _, r, d, _ in files}
    if want != have:
        bad.append("the manifest and the script's lists differ: %s" % sorted(str(x[0]) for x in want ^ have)[:6])
    listed = set()
    for kind, rel, dst, h in files:
        p = ROOT / dst
        listed.add(p.resolve())
        if not p.is_file():
            bad.append("missing " + dst)
        elif sha(p) != h:
            bad.append("changed " + dst)
    for d in (DEV, HOST):
        for p in (d.iterdir() if d.exists() else []):
            if p.resolve() not in listed and p != MANIFEST:
                bad.append("extra " + p.relative_to(ROOT).as_posix())
    for b in bad[:20]:
        print("vendor_bearssl: " + b)
    print("vendor_bearssl: " + ("STALE" if bad else "up to date (%d files, commit %s)" % (len(files), PIN[:12])))
    return 1 if bad else 0


def verify_upstream():
    meta, files = read_manifest()
    tmp, up, _ = fetch(meta["commit"])
    try:
        bad = [rel for _, rel, dst, h in files if sha(up / rel) != h]
        print("vendor_bearssl: upstream " + ("DIFFERS: " + ", ".join(bad[:10]) if bad else "matches every vendored file"))
        return 1 if bad else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    a = sys.argv[1:]
    if a[:1] == ["--check"]:
        sys.exit(check())
    if a[:1] == ["--verify-upstream"]:
        sys.exit(verify_upstream())
    if len(a) >= 2 and a[0] == "--commit":
        if len(a[1]) != 40:
            raise SystemExit("vendor_bearssl: give the full 40-hex commit")
        vendor(a[1], "--all" in a[2:])
        return
    print(__doc__)
    sys.exit(2)


if __name__ == "__main__":
    main()
