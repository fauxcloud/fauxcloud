#!/usr/bin/env python3
"""release_assets.py -- the release workflow's own steps: the version check and the release assets. Stdlib only.

  version   reads FC_VERSION_BASE from app.h; with --tag it fails unless the tag is exactly "v" + that version and the
            version is a release (no "-dev", no "+build"); prints GitHub step outputs (version=, tag=, prerelease=).
  assemble  takes the compile job's files (--parts) and writes the release assets, the release body and a gate folder:
              OUT/assets/  fauxcloud-<ver>.bin            the app image: the OTA file, and the app part of a first install
                           fauxcloud-<ver>-diag.bin       the diagnostic image ("+diag" in its version)
                           fauxcloud-<ver>-factory.zip    the first-install parts, each at its own offset, with
                                                          README.txt, manifest.json, parts.json, flash_args (never one
                                                          merged image: written at 0x0 it would erase the settings)
                           fauxcloud-<ver>-elf.zip        both ELF files, to decode a crash report of either image
                           THIRD_PARTY_NOTICES_FIRMWARE.txt
                           SHA256SUMS                     `sha256sum -c SHA256SUMS` format, every file above
              OUT/body.md  the release body: the template with its placeholders filled
              OUT/gate/    every file that goes out, unpacked, for the leak gate: text/, bin/, elf/
            Before writing anything it checks: both images carry the image marker with this exact version (the diag one
            with "+diag"), the partition table part equals partitions.csv, and no first-install part touches NVS,
            otadata beyond boot_app0, app1, the coredump or LittleFS.

Release body template placeholders: {{version}}, {{tag}}, {{repo_url}}, {{changelog}} (the CHANGELOG.md section whose
heading is "## [<version>]" or "## <version>"), {{sha256sums}}. Any other "{{...}}" left over is an error, and the body
must name the tag and THIRD_PARTY_NOTICES_FIRMWARE.txt. The placeholders are filled in that order, so a changelog
section that quotes "{{tag}}" gets it filled and one that quotes any other "{{...}}" stops the release. A missing
CHANGELOG.md section stops the release; with --changelog-optional (the manual dry run) a stand-in line takes its place.

Usage
  python -I tools/release_assets.py version --app-h firmware/fauxcloud/app.h [--tag v1.0.0]
  python -I tools/release_assets.py assemble --version 1.0.0 --tag v1.0.0 --parts DIR --out DIR \\
      [--partitions firmware/fauxcloud/partitions.csv] [--notices THIRD_PARTY_NOTICES_FIRMWARE.txt] \\
      [--template .github/RELEASE_TEMPLATE.md] [--changelog CHANGELOG.md] [--changelog-optional] [--repo-url URL]
SOURCE_DATE_EPOCH (when set) dates the zip entries, so two runs of one commit write the same zip layout.
"""
import argparse
import hashlib
import io
import json
import os
import re
import shutil
import struct
import sys
import time
import zipfile

MARKER = b"FAUXCLOUD-IMAGE:"
# semver: MAJOR.MINOR.PATCH with an optional pre-release; build metadata ("+...") is the diag image's suffix, never a tag
VERSION_RX = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$")
DEFINE_RX = re.compile(r'^#define\s+FC_VERSION_BASE\s+"([^"]*)"', re.M)

# the compile job's files, relative to --parts (the build folder of the release image and its diag/ subfolder)
PARTS = {
    "app": "fauxcloud.ino.bin",
    "elf": "fauxcloud.ino.elf",
    "bootloader": "fauxcloud.ino.bootloader.bin",
    "partitions": "fauxcloud.ino.partitions.bin",
    "boot_app0": "boot_app0.bin",
    "flash_args": "flash_args",
    "diag": "diag/fauxcloud.ino.bin",
    "diag_elf": "diag/fauxcloud.ino.elf",
}
FLASH_SIZE = 0x400000  # the 4 MB WROOM-32 the table is written for


# the {{changelog}} text of a dry run whose CHANGELOG.md has no section for the version
DRY_RUN_CHANGELOG = "_Dry run: CHANGELOG.md has no section for %s yet._"


class Fail(Exception):
    pass


def note(msg):
    """A notice: an annotation in GitHub Actions, a plain line on stderr elsewhere."""
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print("::notice::release_assets: %s" % msg)
    else:
        print("release_assets: note: %s" % msg, file=sys.stderr)


# --- version -----------------------------------------------------------------------------------------------------
def read_version(app_h_text):
    found = DEFINE_RX.findall(app_h_text)
    if len(found) != 1:
        raise Fail("app.h: expected exactly one FC_VERSION_BASE define, found %d" % len(found))
    v = found[0]
    if not VERSION_RX.match(v):
        raise Fail("FC_VERSION_BASE %r is not a semantic version (MAJOR.MINOR.PATCH[-pre.release])" % v)
    return v


def check_tag(version, tag):
    """A release tag is exactly "v" + FC_VERSION_BASE, and a release is never a development build."""
    if tag != "v" + version:
        raise Fail("tag %r does not match FC_VERSION_BASE %r (expected tag %r)" % (tag, version, "v" + version))
    if re.search(r"-dev(\.|$)", version):
        raise Fail("version %r is a development build: a -dev image is never released" % version)


def is_prerelease(version):
    return "-" in version


# --- partitions ------------------------------------------------------------------------------------------------------
def parse_csv(text):
    """partitions.csv -> [{name, type, subtype, offset, size}] (offsets and sizes as given, hex or decimal)."""
    rows = []
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        f = [c.strip() for c in line.split(",")]
        if len(f) < 5 or not f[3] or not f[4]:
            raise Fail("partitions.csv: every row needs an explicit offset and size: %r" % raw)
        rows.append({"name": f[0], "type": f[1], "subtype": f[2], "offset": int(f[3], 0), "size": int(f[4], 0)})
    return rows


TYPES = {"app": 0x00, "data": 0x01}
SUBTYPES = {
    "app": {"factory": 0x00, "test": 0x20, **{"ota_%d" % i: 0x10 + i for i in range(16)}},
    "data": {"ota": 0x00, "phy": 0x01, "nvs": 0x02, "coredump": 0x03, "nvs_keys": 0x04, "efuse": 0x05,
             "undefined": 0x06, "esphttpd": 0x80, "fat": 0x81, "spiffs": 0x82, "littlefs": 0x83},
}


def parse_table_bin(data):
    """The binary partition table -> [{name, type, subtype, offset, size}] (32-byte entries, magic 0x50AA)."""
    rows = []
    for i in range(0, len(data) - 31, 32):
        e = data[i:i + 32]
        magic, typ, sub, off, size = struct.unpack_from("<HBBII", e)
        if magic != 0x50AA:
            break  # the MD5 entry (0xEBEB) or the 0xFF fill ends the table
        label = e[12:28].split(b"\0", 1)[0].decode("ascii", "replace")
        rows.append({"name": label, "type": typ, "subtype": sub, "offset": off, "size": size})
    return rows


def check_table(table_bin, csv_rows):
    got = [(r["name"], r["type"], r["subtype"], r["offset"], r["size"]) for r in parse_table_bin(table_bin)]
    want = []
    for r in csv_rows:
        t = TYPES.get(r["type"])
        s = SUBTYPES.get(r["type"], {}).get(r["subtype"])
        if t is None or s is None:
            raise Fail("partitions.csv: unknown type/subtype %s/%s" % (r["type"], r["subtype"]))
        want.append((r["name"], t, s, r["offset"], r["size"]))
    if got != want:
        raise Fail("the built partition table differs from partitions.csv")


def parse_flash_args(text):
    """flash_args -> (option line, [(offset, file name)])."""
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    if not lines or not lines[0].startswith("--"):
        raise Fail("flash_args: the first line must hold the flash options")
    parts = []
    for ln in lines[1:]:
        f = ln.split()
        if len(f) != 2:
            raise Fail("flash_args: unexpected line %r" % ln)
        parts.append((int(f[0], 0), f[1]))
    return lines[0], parts


def check_layout(parts, csv_rows):
    """parts: [(role, offset, size)]. Every part sits where the table expects it and touches nothing it must keep."""
    by = {r["name"]: r for r in csv_rows}
    for name in ("nvs", "otadata", "app0"):
        if name not in by:
            raise Fail("partitions.csv has no %s row" % name)
    nvs, ota, app0 = by["nvs"], by["otadata"], by["app0"]
    want = {"bootloader": 0x1000, "partitions": 0x8000, "boot_app0": ota["offset"], "app": app0["offset"]}
    limit = {"bootloader": 0x8000 - 0x1000, "partitions": 0x1000, "boot_app0": ota["size"], "app": app0["size"]}
    roles = [p[0] for p in parts]
    if sorted(roles) != sorted(want):
        raise Fail("the first-install set must be exactly %s, got %s" % (sorted(want), sorted(roles)))
    spans = []
    for role, off, size in parts:
        if off != want[role]:
            raise Fail("%s at 0x%X, expected 0x%X" % (role, off, want[role]))
        if size <= 0 or size > limit[role]:
            raise Fail("%s is %d B, its room is %d B" % (role, size, limit[role]))
        spans.append((off, off + size, role))
    spans.sort()
    for (a0, a1, ra), (b0, b1, rb) in zip(spans, spans[1:]):
        if b0 < a1:
            raise Fail("%s and %s overlap" % (ra, rb))
    # the regions a first install must never write: NVS, and every partition but otadata (boot_app0) and app0
    keep = [r for r in csv_rows if r["name"] not in ("otadata", "app0")]
    for lo, hi, role in spans:
        for r in keep:
            if lo < r["offset"] + r["size"] and r["offset"] < hi:
                raise Fail("%s (0x%X-0x%X) would overwrite the %s partition" % (role, lo, hi, r["name"]))
        if hi > FLASH_SIZE:
            raise Fail("%s ends past the 4 MB flash" % role)
    if nvs["offset"] < 0x9000:
        raise Fail("NVS at 0x%X: the bootloader and the table area end at 0x9000" % nvs["offset"])


# --- images ----------------------------------------------------------------------------------------------------------
def marker_version(data):
    """The version text of the image marker: the bytes after "FAUXCLOUD-IMAGE:" up to the first space."""
    i = data.find(MARKER)
    if i < 0 or data.find(MARKER, i + 1) >= 0:
        raise Fail("the image must hold the image marker exactly once")
    j = i + len(MARKER)
    k = data.find(b" ", j, j + 64)
    if k < 0:
        raise Fail("the image marker has no build date after the version")
    return data[j:k].decode("ascii", "replace")


# --- release body ----------------------------------------------------------------------------------------------------
def changelog_section(text, version):
    """The CHANGELOG.md section for this version, without its heading; None when there is none."""
    rx = re.compile(r"^##\s+\[?" + re.escape(version) + r"\]?(?=[\s(]|$).*$", re.M)
    m = rx.search(text)
    if not m:
        return None
    nxt = re.compile(r"^##\s", re.M).search(text, m.end())
    body = text[m.end():nxt.start() if nxt else len(text)]
    return body.strip("\n")


def render_body(template, values):
    out = template
    for k, v in values.items():
        out = out.replace("{{%s}}" % k, v)
    left = sorted(set(re.findall(r"\{\{[^{}]*\}\}", out)))
    if left:
        raise Fail("the release body template has unknown placeholders: %s" % ", ".join(left))
    if values["tag"] not in out:
        raise Fail("the release body never names the tag (the source sentence must say which tag the source is at)")
    if "THIRD_PARTY_NOTICES_FIRMWARE.txt" not in out:
        raise Fail("the release body never names THIRD_PARTY_NOTICES_FIRMWARE.txt")
    return out


# --- writing ---------------------------------------------------------------------------------------------------------
def zip_date():
    sde = os.environ.get("SOURCE_DATE_EPOCH", "")
    t = int(sde) if sde.isdigit() else 315532800  # 1980-01-01, the earliest date a zip entry holds
    return time.gmtime(max(t, 315532800))[:6]


def write_zip(path, entries):
    """entries: [(name inside the zip, bytes)], written in this order with one fixed date and mode."""
    date = zip_date()
    with zipfile.ZipFile(path, "w") as z:
        for name, data in entries:
            zi = zipfile.ZipInfo(name, date_time=date)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            zi.create_system = 3
            z.writestr(zi, data, compresslevel=9)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def flash_readme(version, names, offsets):
    cmd = " ".join("0x%X %s" % (offsets[r], names[r]) for r in ("bootloader", "partitions", "boot_app0", "app"))
    return (
        "fauxcloud %s -- first install or recovery over USB\n"
        "\n"
        "To UPDATE a working board, do not use this folder: on the admin page open Maintenance -> Firmware\n"
        "update and choose fauxcloud-%s.bin from the release page. Settings and history stay.\n"
        "\n"
        "Write the four parts, each at its own offset (esptool 4.x spells the command write_flash):\n"
        "\n"
        "  esptool --chip esp32 --port PORT write-flash %s\n"
        "\n"
        "This writes the bootloader, the partition table, the boot selector and the app. It writes nothing\n"
        "between 0x9000 and 0xE000, where the settings live (Wi-Fi, MQTT, keys, learned values), and nothing\n"
        "after the first app slot, so an install over an existing board keeps them.\n"
        "Never write a merged image at 0x0 over a working board: its padding erases the settings.\n"
        "A clean start (settings and history wiped) is `esptool --chip esp32 --port PORT erase-flash` first.\n"
        "manifest.json lists the same parts and offsets for a browser flasher (ESP Web Tools); parts.json lists\n"
        "them with each part's size and SHA-256 for the installer at https://fauxcloud.org/install/. A browser\n"
        "flasher that offers to erase the device first: leave that box unticked to keep the settings, tick it\n"
        "for a clean start.\n"
    ) % (version, version, cmd)


ORDER = ("bootloader", "partitions", "boot_app0", "app")


def parts_json(version, names, offsets, files):
    """parts.json of the factory zip: the four parts in flash order, each with its file name, offset, size and SHA-256
    (the facts README.txt states), read by the site's installer so it never parses the README."""
    roles = sorted(ORDER, key=lambda r: offsets[r])
    return json.dumps({
        "name": "fauxcloud",
        "version": version,
        "chip": "ESP32",
        "flash_size": "4MB",
        "parts": [{"role": r, "name": names[r], "offset": offsets[r], "size": len(files[r]), "sha256": sha256(files[r])}
                  for r in roles],
    }, indent=2) + "\n"


def factory_set(version, opts, offsets, files):
    """The factory zip of one version: (zip entries in order, {text name: text}). `opts` is the flash_args option
    line, `offsets` {role: offset} and `files` {role: bytes} for the four roles of ORDER. Shared by the release and by
    build_site.py --local-firmware, so the browser installer always reads the release's own layout."""
    stem = "fauxcloud-%s" % version
    names = {"app": stem + ".bin", "bootloader": stem + "-bootloader.bin", "partitions": stem + "-partitions.bin",
             "boot_app0": stem + "-boot_app0.bin"}
    roles = sorted(ORDER, key=lambda r: offsets[r])
    flash_args = opts + "\n" + "".join("0x%X %s\n" % (offsets[r], names[r]) for r in roles)
    manifest = json.dumps({
        "name": "fauxcloud",
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32",
                    "parts": [{"path": names[r], "offset": offsets[r]} for r in roles]}],
    }, indent=2) + "\n"
    texts = {"README.txt": flash_readme(version, names, offsets), "manifest.json": manifest,
             "parts.json": parts_json(version, names, offsets, files), "flash_args": flash_args}
    folder = stem + "-factory/"
    entries = [(folder + n, texts[n].encode()) for n in ("README.txt", "manifest.json", "parts.json", "flash_args")]
    entries += [(folder + names[r], files[r]) for r in roles]
    return entries, texts, names


def first_install(files, csv_rows):
    """The checked first-install layout of a build: (flash option line, {role: offset}). The partition part equals the
    table of partitions.csv, flash_args names exactly the four parts, and none touches NVS or another kept partition."""
    check_table(files["partitions"], csv_rows)
    opts, fa = parse_flash_args(files["flash_args"].decode("utf-8"))
    role_of = {PARTS[r]: r for r in ORDER}
    layout, offsets = [], {}
    for off, name in fa:
        if name not in role_of:
            raise Fail("flash_args names a file outside the first-install set: %s" % name)
        role = role_of[name]
        layout.append((role, off, len(files[role])))
        offsets[role] = off
    check_layout(layout, csv_rows)
    return opts, offsets


def no_dev_seed(build_dir):
    """Refuse a build folder compiled with the optional secrets.h dev seed: the image would carry the developer's Wi-Fi,
    admin and MQTT seed and a fresh board would skip setup. The build tree copies the sketch sources into sketch/, so the
    file's presence there is the sign; it is never opened."""
    if os.path.isfile(os.path.join(build_dir, "sketch", "secrets.h")):
        raise Fail("%s was built with secrets.h (the developer's seed): build the sketch without secrets.h (a copy of the "
                   "sketch folder without it) and use that build" % build_dir)


def factory_from_build(build_dir, partitions_csv):
    """A development build folder (firmware/fauxcloud/build/default/) -> (version, factory zip bytes), with the same
    checks as a release: the version is the image marker's, the layout passes first_install(). For the site's
    --local-firmware preview only; never a release asset. The sketch may carry another name (a copy of the sketch
    built as <name>.ino, e.g. without the developer's secrets.h): its <name>.ino.* parts are read and named as the
    release's. A build with the developer's seed is refused (no_dev_seed)."""
    no_dev_seed(build_dir)
    stems = sorted(n[:-len(".ino.bin")] for n in os.listdir(build_dir)
                   if n.endswith(".ino.bin") and not n.endswith(".ino.merged.bin")) if os.path.isdir(build_dir) else []
    stem = "fauxcloud" if "fauxcloud" in stems or len(stems) != 1 else stems[0]
    own = {r: PARTS[r].replace("fauxcloud.ino", stem + ".ino") for r in ORDER + ("flash_args",)}
    files = {}
    for role in ORDER + ("flash_args",):
        p = os.path.join(build_dir, own[role])
        if not os.path.isfile(p):
            raise Fail("missing part %s (%s) in %s" % (role, own[role], build_dir))
        with open(p, "rb") as f:
            files[role] = f.read()
    if stem != "fauxcloud":  # flash_args names the sketch's own files: the release's names in their place
        text = files["flash_args"].decode("utf-8")
        for role in ORDER:
            text = re.sub(r"(?<=\s)%s(?=\s|$)" % re.escape(own[role]), PARTS[role], text)
        files["flash_args"] = text.encode("utf-8")
    version = marker_version(files["app"])
    if not VERSION_RX.match(version):
        raise Fail("the image marker says %r: not a semantic version" % version)
    with open(partitions_csv, encoding="utf-8") as f:
        opts, offsets = first_install(files, parse_csv(f.read()))
    entries = factory_set(version, opts, offsets, files)[0]
    buf = io.BytesIO()
    write_zip(buf, entries)
    return version, buf.getvalue()


def assemble(a):
    version, tag = a.version, a.tag
    if not VERSION_RX.match(version):
        raise Fail("--version %r is not a semantic version" % version)
    no_dev_seed(a.parts)
    files = {}
    for role, rel in PARTS.items():
        p = os.path.join(a.parts, rel)
        if not os.path.isfile(p):
            raise Fail("missing part %s (%s)" % (role, rel))
        with open(p, "rb") as f:
            files[role] = f.read()
    for p in (a.partitions, a.notices, a.template):
        if not os.path.isfile(p):
            raise Fail("missing %s" % p)

    # the images carry this exact version: the OTA page accepts only a marked image, and the marker is what the
    # board then reports as its version (and sends as its User-Agent)
    got = marker_version(files["app"])
    if got != version:
        raise Fail("the app image says %r, the release is %r" % (got, version))
    got = marker_version(files["diag"])
    if got != version + "+diag":
        raise Fail("the diag image says %r, expected %r" % (got, version + "+diag"))

    with open(a.partitions, encoding="utf-8") as f:
        opts, offsets = first_install(files, parse_csv(f.read()))

    # the body's inputs are checked before anything is written (the checksums are filled in at the end)
    changelog = ""
    with open(a.template, encoding="utf-8") as f:
        template = f.read()
    if "{{changelog}}" in template:
        missing = None
        if not a.changelog or not os.path.isfile(a.changelog):
            missing = "the template asks for {{changelog}} and there is no CHANGELOG.md"
        else:
            with open(a.changelog, encoding="utf-8") as f:
                changelog = changelog_section(f.read(), version)
            if changelog is None:
                missing = "CHANGELOG.md has no section for %s" % version
        if missing:
            # a tag release needs its section; a dry run of a development tree has none, so it gets a stand-in line
            if not a.changelog_optional:
                raise Fail(missing)
            changelog = DRY_RUN_CHANGELOG % version
            note(missing + ": a stand-in line is used (dry run)")
    values = {"version": version, "tag": tag, "repo_url": a.repo_url, "changelog": changelog, "sha256sums": ""}
    render_body(template, values)

    out = a.out
    if os.path.exists(out) and os.listdir(out):
        raise Fail("--out %s is not empty" % out)
    assets, gate = os.path.join(out, "assets"), os.path.join(out, "gate")
    for d in (assets, os.path.join(gate, "text"), os.path.join(gate, "bin"), os.path.join(gate, "elf")):
        os.makedirs(d)

    stem = "fauxcloud-%s" % version
    factory, texts, names = factory_set(version, opts, offsets, files)
    readme = texts["README.txt"]

    out_assets = {
        stem + ".bin": files["app"],
        stem + "-diag.bin": files["diag"],
    }
    for name, data in out_assets.items():
        with open(os.path.join(assets, name), "wb") as f:
            f.write(data)
    write_zip(os.path.join(assets, stem + "-factory.zip"), factory)
    write_zip(os.path.join(assets, stem + "-elf.zip"), [(stem + ".elf", files["elf"]),
                                                       (stem + "-diag.elf", files["diag_elf"])])
    shutil.copyfile(a.notices, os.path.join(assets, os.path.basename(a.notices)))
    sums = ""
    for name in sorted(os.listdir(assets)):
        with open(os.path.join(assets, name), "rb") as f:
            sums += "%s  %s\n" % (sha256(f.read()), name)
    with open(os.path.join(assets, "SHA256SUMS"), "w", encoding="utf-8", newline="\n") as f:
        f.write(sums)

    body = render_body(template, dict(values, sha256sums=sums.rstrip("\n")))
    with open(os.path.join(out, "body.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(body)

    # the gate folder: every text and every image that goes out, unpacked from the zips
    text = dict(texts, **{"release-body.md": body, "SHA256SUMS": sums})
    for name, t in text.items():
        with open(os.path.join(gate, "text", name), "w", encoding="utf-8", newline="\n") as f:
            f.write(t)
    for name, data in [(stem + ".bin", files["app"]), (stem + "-diag.bin", files["diag"])] + \
            [(names[r], files[r]) for r in ("bootloader", "partitions", "boot_app0")]:
        with open(os.path.join(gate, "bin", name), "wb") as f:
            f.write(data)
    for name, data in ((stem + ".elf", files["elf"]), (stem + "-diag.elf", files["diag_elf"])):
        with open(os.path.join(gate, "elf", name), "wb") as f:
            f.write(data)

    for name in sorted(os.listdir(assets)):
        print("%10d  %s" % (os.path.getsize(os.path.join(assets, name)), name))


def main(argv=None):
    ap = argparse.ArgumentParser(prog="release_assets.py", description="The release workflow's version check and assets.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("version", help="read FC_VERSION_BASE; with --tag, check the tag against it")
    v.add_argument("--app-h", required=True)
    v.add_argument("--tag", help="the pushed tag; omitted for a dry run")
    s = sub.add_parser("assemble", help="write the release assets, the body and the gate folder")
    s.add_argument("--version", required=True)
    s.add_argument("--tag", required=True)
    s.add_argument("--parts", required=True, help="the release build folder (with its diag/ subfolder)")
    s.add_argument("--out", required=True)
    s.add_argument("--partitions", default="firmware/fauxcloud/partitions.csv")
    s.add_argument("--notices", default="THIRD_PARTY_NOTICES_FIRMWARE.txt")
    s.add_argument("--template", default=".github/RELEASE_TEMPLATE.md")
    s.add_argument("--changelog", default="CHANGELOG.md")
    s.add_argument("--changelog-optional", action="store_true",
                   help="a missing CHANGELOG.md section becomes a stand-in line (the manual dry run only)")
    s.add_argument("--repo-url", default="https://github.com/fauxcloud/fauxcloud")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "version":
            with open(a.app_h, encoding="utf-8") as f:
                version = read_version(f.read())
            if a.tag is not None:
                check_tag(version, a.tag)
            print("version=%s" % version)
            print("tag=%s" % (a.tag if a.tag is not None else "v" + version))
            print("prerelease=%s" % ("true" if is_prerelease(version) else "false"))
        else:
            assemble(a)
    except (Fail, OSError, ValueError) as e:
        print("release_assets: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
