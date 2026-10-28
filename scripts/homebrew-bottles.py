#!/usr/bin/env python3
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
"""Build, verify and describe den's Homebrew bottles.

A release ships one bottle per supported arm64 macOS tag, repackaged from
the darwin-aarch64 release tarball (the same binary the harness validated;
nothing is recompiled). Subcommands:

  build            write den-<ver>.<tag>.bottle.tar.gz (+ .sha256) per tag
  verify           check bottle layout, receipt, and that bin/den is
                   byte-identical to the release tarball's binary
  block            print the formula's `bottle do` block for a root_url
  formula          print a complete den.rb (the RC formula asset)
  releaser-inputs  write formula_includes/install/test for
                   homebrew-releaser to $GITHUB_OUTPUT (GA formula)

Homebrew facts this relies on (cited in the PR that introduced it):
  * For a non-GHCR root_url, brew downloads
    "#{root_url}/#{name}-#{pkg_version}.#{tag}.bottle.tar.gz"
    (Bottle::Filename#url_encode: single dash; the local cache name uses
    "--"). Rebuild 0 adds no ".N" suffix.
  * The tarball's top level is <name>/<pkg_version>/; brew reads the
    version from the second path component and the receipt from
    <name>/<version>/INSTALL_RECEIPT.json, then rewrites it at pour time
    with poured_from_bottle=true.
  * The macOS binary links /opt/homebrew/opt/libarchive by absolute path,
    so the bottle is only valid in the default arm64 prefix: a fixed
    cellar "/opt/homebrew/Cellar". Other prefixes fall back to installing
    from the release tarball as before.
"""

import argparse
import gzip
import hashlib
import io
import json
import os
import re
import sys
import tarfile

NAME = "den"
REPO = "marcelocantos/den"

# Newest first, as `brew bottle` writes them. arm64 only: there is no
# x86_64 macOS build. Nothing below sonoma: the binary is built with
# minos 14.0 and Homebrew's oldest supported macOS is 14. golden_gate is
# macOS 27 (brew >= 6.0); older brews ignore tags they cannot map.
TAGS = ("arm64_golden_gate", "arm64_tahoe", "arm64_sequoia", "arm64_sonoma")
CELLAR = "/opt/homebrew/Cellar"

# Fixed timestamp for reproducible tarballs (2026-01-01T00:00:00Z).
EPOCH = 1767225600

VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")

FORMULA_INCLUDES = """\
# The macOS release binary links libarchive from the Homebrew prefix.
on_macos do
  depends_on "libarchive"
end

# Background maintenance daemon (`brew services start den`). Its
# launchd label is Homebrew's (sh.brew.den / homebrew.mxcl.den),
# distinct from `den daemon install`'s dev.den.daemon; both use
# the same DEN_HOME pid lock, so run one or the other, not both.
service do
  run [opt_bin/"den", "daemon", "run"]
  keep_alive true
  log_path var/"log/den.log"
  error_log_path var/"log/den.log"
  environment_variables PATH: std_service_path_env
end
"""

INSTALL = """\
bin.install "den"
pkgshare.install "share/den/known_hashes.json" if File.exist?("share/den/known_hashes.json")
"""

TEST = """\
assert_equal version.to_s, shell_output("#{bin}/den --version").strip
ENV["DEN_HOME"] = (testpath/".den").to_s
ENV["DEN_PREFIX"] = (testpath/"prefix").to_s
assert_match "No packages installed", shell_output("#{bin}/den list")
"""

# Release tarball member -> path inside the keg.
PAYLOAD = (
    ("den", "bin/den", 0o755),
    ("share/den/known_hashes.json", "share/den/known_hashes.json", 0o644),
)


def die(msg):
    print(f"homebrew-bottles: {msg}", file=sys.stderr)
    sys.exit(1)


def check_version(version):
    if not VERSION_RE.match(version):
        die(f"version must be X.Y.Z (no v, no -rc): {version!r}")


def bottle_name(version, tag):
    return f"{NAME}-{version}.{tag}.bottle.tar.gz"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_release_tarball(path):
    """Return {member: bytes} for the payload members present."""
    out = {}
    with tarfile.open(path, "r:gz") as tf:
        for m in tf.getmembers():
            name = m.name[2:] if m.name.startswith("./") else m.name
            if m.isfile() and name in {p[0] for p in PAYLOAD}:
                out[name] = tf.extractfile(m).read()
    if "den" not in out:
        die(f"{path}: no top-level 'den' binary")
    return out


def receipt(version):
    # Field set mirrors Tab#to_json as written by `brew bottle`:
    # poured_from_bottle=false and time=null until brew pours it.
    return {
        "homebrew_version": "den-release",
        "used_options": [],
        "unused_options": [],
        "built_as_bottle": True,
        "poured_from_bottle": False,
        "loaded_from_api": False,
        "installed_as_dependency": False,
        "installed_on_request": False,
        "changed_files": [],
        "time": None,
        "source_modified_time": EPOCH,
        "compiler": "clang",
        "aliases": [],
        "runtime_dependencies": [],
        "source": {
            "spec": "stable",
            "versions": {"stable": version, "head": None, "version_scheme": 0},
        },
        "arch": "arm64",
        "built_on": {"os": "Macintosh", "os_version": "macOS 14"},
    }


def build_bottle_bytes(version, payload):
    root = f"{NAME}/{version}"
    entries = []  # (path, data-or-None, mode)
    dirs = set()
    files = [(f"{root}/INSTALL_RECEIPT.json",
              (json.dumps(receipt(version), indent=2) + "\n").encode(), 0o644)]
    for member, dest, mode in PAYLOAD:
        if member in payload:
            files.append((f"{root}/{dest}", payload[member], mode))
    for path, _, _ in files:
        parts = path.split("/")[:-1]
        for i in range(1, len(parts) + 1):
            dirs.add("/".join(parts[:i]))
    entries = [(d, None, 0o755) for d in dirs] + files
    entries.sort(key=lambda e: e[0])

    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as tf:
        for path, data, mode in entries:
            ti = tarfile.TarInfo(path)
            ti.mtime = EPOCH
            ti.mode = mode
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = ""
            if data is None:
                ti.type = tarfile.DIRTYPE
                tf.addfile(ti)
            else:
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))
    gz = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=gz, mtime=0,
                       compresslevel=9) as g:
        g.write(raw.getvalue())
    return gz.getvalue()


def cmd_build(a):
    check_version(a.version)
    payload = read_release_tarball(a.tarball)
    data = build_bottle_bytes(a.version, payload)
    os.makedirs(a.out, exist_ok=True)
    digest = hashlib.sha256(data).hexdigest()
    for tag in TAGS:
        name = bottle_name(a.version, tag)
        with open(os.path.join(a.out, name), "wb") as f:
            f.write(data)
        # Same "<hex>  <file>" shape as the other .sha256 assets, so
        # `cat dist/*.sha256` keeps producing a valid checksums file.
        with open(os.path.join(a.out, name + ".sha256"), "w") as f:
            f.write(f"{digest}  {name}\n")
        print(f"{digest}  {name}")


def verify_one(path, version, payload):
    errs = []
    root = f"{NAME}/{version}"
    with tarfile.open(path, "r:gz") as tf:
        members = {m.name: m for m in tf.getmembers()}
        for name, m in members.items():
            if not (name == NAME or name == root or name.startswith(root + "/")):
                errs.append(f"unexpected path {name!r} (want {root}/...)")
            if m.issym() or m.islnk():
                errs.append(f"link in bottle: {name}")
        rec = members.get(f"{root}/INSTALL_RECEIPT.json")
        if rec is None:
            errs.append("missing INSTALL_RECEIPT.json")
        else:
            r = json.load(tf.extractfile(rec))
            if r.get("built_as_bottle") is not True:
                errs.append("receipt: built_as_bottle is not true")
            if r.get("poured_from_bottle") is not False:
                errs.append("receipt: poured_from_bottle must be false before pouring")
            if r.get("changed_files") != []:
                errs.append("receipt: changed_files must be empty (nothing to relocate)")
            got = r.get("source", {}).get("versions", {}).get("stable")
            if got != version:
                errs.append(f"receipt: source.versions.stable {got!r} != {version!r}")
        for member, dest, mode in PAYLOAD:
            m = members.get(f"{root}/{dest}")
            if member not in payload:
                continue
            if m is None:
                errs.append(f"missing {root}/{dest}")
                continue
            if tf.extractfile(m).read() != payload[member]:
                errs.append(f"{dest} differs from the release tarball's {member}")
            if (m.mode & 0o777) != mode:
                errs.append(f"{dest} mode {oct(m.mode & 0o777)} != {oct(mode)}")
    return errs


def cmd_verify(a):
    check_version(a.version)
    payload = read_release_tarball(a.tarball)
    sums = {}
    if a.checksums:
        with open(a.checksums) as f:
            for line in f:
                parts = line.split()
                if len(parts) == 2:
                    sums[parts[1]] = parts[0]
    bad = 0
    for tag in TAGS:
        name = bottle_name(a.version, tag)
        path = os.path.join(a.dir, name)
        if not os.path.isfile(path):
            print(f"FAIL {name}: missing", file=sys.stderr)
            bad += 1
            continue
        errs = verify_one(path, a.version, payload)
        if a.checksums:
            want = sums.get(name)
            got = sha256_file(path)
            if want is None:
                errs.append("not listed in checksums file")
            elif want != got:
                errs.append(f"sha256 {got} != checksums file {want}")
        for e in errs:
            print(f"FAIL {name}: {e}", file=sys.stderr)
        bad += bool(errs)
        if not errs:
            print(f"ok   {name}")
    if bad:
        die(f"{bad} bottle(s) failed verification")


def bottle_block(version, root_url, directory):
    lines = ["bottle do", f'  root_url "{root_url}"']
    for tag in TAGS:
        path = os.path.join(directory, bottle_name(version, tag))
        if not os.path.isfile(path):
            die(f"missing bottle {path}")
        lines.append(f'  sha256 cellar: "{CELLAR}", {tag}: "{sha256_file(path)}"')
    lines.append("end")
    return "\n".join(lines) + "\n"


def release_url(release_tag):
    return f"https://github.com/{REPO}/releases/download/{release_tag}"


def formula_includes(version, release_tag, directory):
    return bottle_block(version, release_url(release_tag), directory) + "\n" + FORMULA_INCLUDES


def cmd_block(a):
    check_version(a.version)
    sys.stdout.write(bottle_block(a.version, a.root_url or release_url(a.release_tag), a.dir))


def indent(text, n):
    pad = " " * n
    return "".join(pad + l if l.strip() else l for l in text.splitlines(True))


def asset_sha(directory, name):
    path = os.path.join(directory, name)
    if not os.path.isfile(path):
        die(f"missing asset {path}")
    return sha256_file(path)


def cmd_formula(a):
    """A den.rb shaped like homebrew-releaser's output, for the RC."""
    check_version(a.version)
    v, d, url = a.version, a.dir, release_url(a.release_tag)
    tarballs = {t: f"{NAME}-{v}-{t}.tar.gz"
                for t in ("darwin-aarch64", "linux-x86_64", "linux-aarch64")}
    sha = {t: asset_sha(d, n) for t, n in tarballs.items()}
    out = f"""# typed: true
# frozen_string_literal: true

# Generated by den's release-candidate workflow (scripts/homebrew-bottles.py)
# for {a.release_tag}. GA formulae are written by promote-rc.yml.
class Den < Formula
{indent(formula_includes(v, a.release_tag, d), 2)}
  desc "Universal development environment manager. Think virtualenv for everything"
  homepage "https://github.com/{REPO}"
  url "{url}/{tarballs['darwin-aarch64']}"
  version "{v}"
  sha256 "{sha['darwin-aarch64']}"
  license "Apache-2.0"

  on_macos do
    on_arm do
      url "{url}/{tarballs['darwin-aarch64']}"
      sha256 "{sha['darwin-aarch64']}"
    end
  end

  on_linux do
    on_intel do
      url "{url}/{tarballs['linux-x86_64']}"
      sha256 "{sha['linux-x86_64']}"
    end

    on_arm do
      url "{url}/{tarballs['linux-aarch64']}"
      sha256 "{sha['linux-aarch64']}"
    end
  end

  def install
{indent(INSTALL, 4)}  end

  test do
{indent(TEST, 4)}  end
end
"""
    sys.stdout.write(re.sub(r"\n{3,}", "\n\n", out))


def cmd_releaser_inputs(a):
    check_version(a.version)
    values = {
        "formula_includes": formula_includes(a.version, a.release_tag, a.dir),
        "install": INSTALL,
        "test": TEST,
    }
    def emit(out):
        for k, val in values.items():
            delim = "EOF_" + hashlib.sha256(val.encode()).hexdigest()[:16]
            out.write(f"{k}<<{delim}\n{val.rstrip()}\n{delim}\n")

    if a.github_output in (None, "", "-"):
        emit(sys.stdout)
    else:
        with open(a.github_output, "a") as out:
            emit(out)

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build")
    b.add_argument("--version", required=True)
    b.add_argument("--tarball", required=True, help="den-<ver>-darwin-aarch64.tar.gz")
    b.add_argument("--out", required=True)
    b.set_defaults(fn=cmd_build)

    v = sub.add_parser("verify")
    v.add_argument("--version", required=True)
    v.add_argument("--tarball", required=True)
    v.add_argument("--dir", required=True)
    v.add_argument("--checksums", help="den-<ver>-checksums.txt to cross-check")
    v.set_defaults(fn=cmd_verify)

    k = sub.add_parser("block")
    k.add_argument("--version", required=True)
    k.add_argument("--dir", required=True)
    g = k.add_mutually_exclusive_group(required=True)
    g.add_argument("--root-url")
    g.add_argument("--release-tag")
    k.set_defaults(fn=cmd_block)

    f = sub.add_parser("formula")
    f.add_argument("--version", required=True)
    f.add_argument("--release-tag", required=True)
    f.add_argument("--dir", required=True)
    f.set_defaults(fn=cmd_formula)

    r = sub.add_parser("releaser-inputs")
    r.add_argument("--version", required=True)
    r.add_argument("--release-tag", required=True)
    r.add_argument("--dir", required=True)
    r.add_argument("--github-output", default=os.environ.get("GITHUB_OUTPUT"))
    r.set_defaults(fn=cmd_releaser_inputs)

    a = p.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
