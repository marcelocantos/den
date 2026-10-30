#!/usr/bin/env python3
# Copyright 2026 Marcelo Cantos
# SPDX-License-Identifier: Apache-2.0
#
# Guard: a den release carries den's own assets and nothing else. Scans every
# workflow under .github/workflows, the Makefile and scripts/*.sh for
# `gh release create`, `gh release upload` and `gh release download
# --pattern`, and checks the assets each one names against a sample release:
#
#   den-<v>-<target>.tar.gz          for darwin-aarch64, linux-x86_64,
#   den-<v>-<target>.tar.gz.sha256   linux-aarch64
#   den-<v>-checksums.txt
#
# A glob (a literal * ? or [ in the argument) must not match anything else
# that can sit next to them, such as harness-linux-logs-<rc>.tar.gz, which a
# bare *.tar.gz picks up. Every gh command in a file that publishes or fetches
# release assets must, between them, publish all seven den assets.
#
# The one non-den asset allowed is the Linux harness log, and only when
# release-candidate.yml names it outright on its prerelease; it never reaches
# a GA release.
#
#   scripts/check-release-assets.py [--version X.Y.Z] [--rc-tag vX.Y.Z-rc.N] [repo-root]
#
# The sample release is den-<version> with RC tag <rc-tag>. --version defaults
# to project(den VERSION X.Y.Z) in the root's CMakeLists.txt (this
# repository's when the root has none); --rc-tag defaults to v<version>-rc.1.
#
# Exit 0 when clean, 1 with one "file:line: message" per problem, 2 when there
# is nothing to scan or the version or RC tag is invalid.

import argparse
import fnmatch
import glob
import os
import re
import shlex
import sys

TARGETS = ["darwin-aarch64", "linux-x86_64", "linux-aarch64"]
HARNESS_LOG = re.compile(r"^harness-linux-logs-.*\.tar\.gz$")
CMAKE_VERSION = re.compile(r"^\s*project\(\s*den\s+VERSION\s+(\d+\.\d+\.\d+)\b", re.M)
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")
RC_TAG_RE = re.compile(r"^v(\d+\.\d+\.\d+)-rc\.\d+$")

# The sample release, set by configure() from --version / --rc-tag.
VERSION = RC_TAG = None
REQUIRED = FOREIGN = []


def configure(version, rc_tag):
    """Name the sample release's den assets and the files that sit beside them."""
    global VERSION, RC_TAG, REQUIRED, FOREIGN
    VERSION, RC_TAG = version, rc_tag
    REQUIRED = (
        [f"den-{version}-{t}.tar.gz" for t in TARGETS]
        + [f"den-{version}-{t}.tar.gz.sha256" for t in TARGETS]
        + [f"den-{version}-checksums.txt"]
    )
    # Files that can be in the working directory or on an RC next to den's assets.
    FOREIGN = [
        f"harness-linux-logs-{rc_tag}.tar.gz",
        "harness-linux-logs.tar.gz",
        "release-notes.md",
        "ga-release-notes.md",
        "source.tar.gz",
        f"den-{version}.tar.gz.sha256.sig",
        "checksums.txt",
        "logs.sha256",
    ]


def cmake_version(root):
    """project(den VERSION ...) from root's CMakeLists.txt, else this repository's."""
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for d in (root, here):
        path = os.path.join(d, "CMakeLists.txt")
        if os.path.isfile(path):
            m = CMAKE_VERSION.search(open(path, encoding="utf-8").read())
            return (m.group(1) if m else None), path
    return None, os.path.join(here, "CMakeLists.txt")
HARNESS_LOG_OWNER = "release-candidate.yml"

# gh release flags that take a value; anything else starting with - is boolean.
VALUE_FLAGS = {
    "--target", "--title", "-t", "--notes", "-n", "--notes-file", "-F",
    "--notes-start-tag", "--repo", "-R", "--discussion-category", "--dir",
    "-D", "--output", "-O", "--archive", "-A", "--pattern", "-p",
    "--skip-existing",
}

GH_EXPR = re.compile(r"\$\{\{\s*([^}]*?)\s*\}\}")
ASSIGN = re.compile(r"^\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)$")
ENV_FILE = re.compile(r'echo\s+"([A-Za-z_][A-Za-z0-9_]*)=([^"]*)"\s*>>\s*"?\$GITHUB_ENV')
SHELL_VAR = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)")
SHELL_EXPANSION = re.compile(r"\$\{[^}]*\}|\$\([^)]*\)")

WILD = "\x00"  # stands for "any text" until the argument becomes a glob


def logical_lines(text):
    """Join backslash-continued lines; yield (first line number, text)."""
    buf, start = "", None
    for n, line in enumerate(text.splitlines(), 1):
        if start is None:
            start = n
        if line.rstrip().endswith("\\"):
            buf += line.rstrip()[:-1] + " "
            continue
        yield start, buf + line
        buf, start = "", None
    if start is not None:
        yield start, buf


def unquote_value(v):
    v = v.strip()
    try:
        parts = shlex.split(v, comments=False)
    except ValueError:
        return v
    return parts[0] if len(parts) == 1 else v


class Resolver:
    def __init__(self):
        self.shell = {}  # last assignment seen above the current line
        self.ghenv = {}  # names written to $GITHUB_ENV in this file

    def note(self, text):
        for m in ENV_FILE.finditer(text):
            self.ghenv[m.group(1)] = m.group(2)
        m = ASSIGN.match(text)
        if m and not text.strip().startswith(("-", "#")):
            self.shell[m.group(1)] = unquote_value(m.group(2))

    def resolve(self, s, depth=0):
        if depth > 8:
            return WILD

        def gh(m):
            expr = m.group(1)
            if expr.startswith("env.") and expr[4:] in self.ghenv:
                return self.resolve(self.ghenv[expr[4:]], depth + 1)
            return WILD

        s = GH_EXPR.sub(gh, s)

        def var(m):
            name = m.group(1) or m.group(2)
            if name in self.shell:
                return self.resolve(self.shell[name], depth + 1)
            return WILD

        s = SHELL_VAR.sub(var, s)
        return SHELL_EXPANSION.sub(WILD, s)


def has_literal_glob(raw):
    stripped = GH_EXPR.sub("", raw)
    stripped = SHELL_EXPANSION.sub("", SHELL_VAR.sub("", stripped))
    return any(c in stripped for c in "*?[")


def to_glob(resolved):
    return os.path.basename(resolved.replace(WILD, "*"))


def assets_of(tokens):
    """Return (verb, [asset args]) for a gh release command, or None."""
    try:
        i = tokens.index("gh")
    except ValueError:
        return None
    if tokens[i + 1:i + 2] != ["release"] or len(tokens) < i + 3:
        return None
    verb, rest = tokens[i + 2], tokens[i + 3:]
    if verb not in ("create", "upload", "download"):
        return None
    out, positional, j = [], [], 0
    while j < len(rest):
        t = rest[j]
        if t in (";", "&&", "||", "|", ")"):
            break
        if t.startswith("-"):
            flag, _, inline = t.partition("=")
            if flag in VALUE_FLAGS:
                val = inline if inline else (rest[j + 1] if j + 1 < len(rest) else "")
                if verb == "download" and flag in ("--pattern", "-p"):
                    out.append(val)
                j += 1 if inline else 2
                continue
            j += 1
            continue
        positional.append(t)
        j += 1
    if verb in ("create", "upload"):
        out = positional[1:]  # first positional is the tag
    return verb, out


def scan_file(path, rel, problems):
    try:
        text = open(path, encoding="utf-8").read()
    except OSError as e:
        problems.append(f"{rel}: cannot read: {e}")
        return
    res = Resolver()
    covered, seen_cmd = {}, {}
    for line_no, line in logical_lines(text):
        res.note(line)
        if "gh release" not in line:
            continue
        body = line.split("run:", 1)[1] if line.lstrip().startswith("run:") else line
        try:
            tokens = shlex.split(body, comments=True)
        except ValueError:
            continue
        got = assets_of(tokens)
        if not got:
            continue
        verb, raw_assets = got
        kind = "download" if verb == "download" else "publish"
        if verb == "download" and not raw_assets:
            # `gh release download` without --pattern fetches every asset.
            problems.append(f"{rel}:{line_no}: gh release download without --pattern fetches every asset")
            continue
        if not raw_assets:
            continue
        seen_cmd.setdefault(kind, line_no)
        cov = covered.setdefault(kind, set())
        for raw in raw_assets:
            pat = to_glob(res.resolve(raw))
            matched = [a for a in REQUIRED + FOREIGN if fnmatch.fnmatchcase(a, pat)]
            cov.update(a for a in matched if a in REQUIRED)
            if pat.strip("*") == "":
                problems.append(f"{rel}:{line_no}: asset {raw!r} cannot be resolved to a den asset name")
                continue
            if has_literal_glob(raw):
                bad = [a for a in matched if a in FOREIGN]
                if bad:
                    problems.append(
                        f"{rel}:{line_no}: glob {raw!r} also matches non-den assets: {', '.join(bad)}"
                    )
                    continue
            else:
                harness_ok = (
                    os.path.basename(rel) == HARNESS_LOG_OWNER
                    and verb == "create"
                    and HARNESS_LOG.match(pat)
                )
                if not pat.startswith("den-") and not harness_ok:
                    problems.append(f"{rel}:{line_no}: asset {raw!r} is not a den release asset")
                    continue
    for kind, line_no in seen_cmd.items():
        if kind != "publish":
            continue  # a download may fetch a subset (e.g. just the .sha256s)
        missing = [a for a in REQUIRED if a not in covered.get(kind, set())]
        if missing:
            problems.append(
                f"{rel}:{line_no}: gh release commands in this file do not publish {', '.join(missing)}"
            )


def main():
    ap = argparse.ArgumentParser(description="Check that den releases carry only den's own assets.")
    ap.add_argument("root", nargs="?", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    help="repository root to scan (default: this repository)")
    ap.add_argument("--version", help="den version X.Y.Z (default: CMakeLists.txt project version)")
    ap.add_argument("--rc-tag", help="RC tag vX.Y.Z-rc.N (default: v<version>-rc.1)")
    args = ap.parse_args()
    root = args.root
    version = args.version
    if version is None:
        version, path = cmake_version(root)
        if version is None:
            print(f"check-release-assets: no project(den VERSION X.Y.Z) in {path}; pass --version",
                  file=sys.stderr)
            return 2
    version = version[1:] if version.startswith("v") else version
    if not VERSION_RE.match(version):
        print(f"check-release-assets: --version {version!r} is not X.Y.Z", file=sys.stderr)
        return 2
    rc_tag = args.rc_tag if args.rc_tag is not None else f"v{version}-rc.1"
    m = RC_TAG_RE.match(rc_tag)
    if not m or m.group(1) != version:
        print(f"check-release-assets: --rc-tag {rc_tag!r} is not v{version}-rc.N", file=sys.stderr)
        return 2
    configure(version, rc_tag)
    files = sorted(
        glob.glob(os.path.join(root, ".github/workflows/*.yml"))
        + glob.glob(os.path.join(root, ".github/workflows/*.yaml"))
        + glob.glob(os.path.join(root, "scripts/*.sh"))
    )
    if os.path.isfile(os.path.join(root, "Makefile")):
        files.append(os.path.join(root, "Makefile"))
    if not files:
        print(f"check-release-assets: no workflows, scripts or Makefile under {root}", file=sys.stderr)
        return 2
    problems = []
    for f in files:
        scan_file(f, os.path.relpath(f, root), problems)
    if problems:
        print("check-release-assets: release assets must be den's own:", file=sys.stderr)
        for p in problems:
            print(p, file=sys.stderr)
        return 1
    print(f"check-release-assets: OK (den-{VERSION}, {RC_TAG})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
