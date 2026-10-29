# Source builds: opt-prefix mode

When den builds a formula from source (`den install --build-from-source`, or
a package with no usable bottle), it builds the way a Homebrew bottle is laid
out:

* **Configured for the opt path.** The build sees its prefix as
  `<prefix>/opt/<name>`, the stable link den maintains for the linked keg.
  Everything the build bakes in — install names, pkg-config files, wrapper
  scripts, a compiled-in `sysconfdir` or runtime prefix — names that path,
  so it stays valid when the package is upgraded.
* **Installed into the versioned keg.** Install steps run with
  `DESTDIR=<den_home>/cache/build/<name>-<version>.destdir`. Right after each
  install step, den merges `<stage><prefix>/opt/<name>/…` into
  `<prefix>/Cellar/<name>/<version>`. Files staged for the shared
  `<prefix>/etc` and `<prefix>/var` go there, never replacing a file that
  already exists. Anything staged anywhere else fails the build.
* **The live opt link is never written.** den snapshots `<prefix>/opt/<name>`
  (missing, or a link to an older keg) before the build and fails the build
  if it changed. A directory the build created there is removed. The opt link
  only moves to the new keg when den links the keg, as for a poured bottle.
  If the link already names an older keg, a build that ignores DESTDIR
  would write through it. On macOS the [build sandbox](#build-sandbox-macos)
  denies that write, so the build fails and the older keg is untouched. On
  Linux den detects the write and fails the build, but cannot undo it, so
  uninstall and reinstall the older version.
* **Dependencies by opt path.** A dependency's `opt_prefix`, `opt_lib`,
  `opt_include`, and the rest resolve to `<prefix>/opt/<dep>` (or to the
  dependency's keg if it has no opt link yet).

## Unattended source builds

Interactive `den install -s` builds as described here. An unattended run
(see [trust-model.md](trust-model.md#unattended-installs--trustunattended))
does not build from source by default:

* `trust.unattended_source_builds` is `off` by default: every unattended
  source build is deferred and reported.
* With `den set trust.unattended_source_builds allowlist`, a formula in
  `trust.source_build_allowlist` (default `gettext,git,openssl@3,openssl@4,pkgconf`;
  set it with `den set trust.source_build_allowlist "git,pkgconf"`) may build
  unattended, but only when the source archive's hash is cross-checked
  against the replica (key `<name>--<version>--source`; den ships one for
  each formula in the default allowlist, kept current by the replica-verify
  pipeline). If only one source vouches for it, the build is deferred and
  reported instead of built.
* Hashes that disagree refuse the build in every mode.

A deferred build is not attempted. It is recorded in the trust ledger
(`reason: "unattended-source-build"`, with the setting or replica status
that deferred it), shown by `den doctor` and `den trust review`, and the
command exits non-zero. `den trust accept <formula>@<version>` lets that exact
version build unattended once.

## What counts as an install step

`make`/`gmake` with an `install`, `install-*` or `install_*` target (den
appends `DESTDIR=` on the command line, which overrides a Makefile's own
`DESTDIR =`, as in OpenSSL's), `cmake --install`,
`cmake --build … --target install`, `ninja … install` and `meson install`
(den exports `DESTDIR`).

In the bundled-Ruby build (`src/ruby/den_build.rb`) the formula's own Ruby
file operations (`bin.install`, `rm`, `inreplace` of an installed file) still
work on the keg, because `prefix`, `bin`, `lib` and the rest are the keg's
paths. Only what reaches a build command is rewritten to the opt path:
`system` arguments, environment variables for the duration of a command, and
`inreplace` replacement text. `etc` and `var` are the shared
`<prefix>/etc` and `<prefix>/var`, as in Homebrew.

The native fallback (no Ruby) uses the same mode. Its formula parser and the
generic cmake, autotools and make recipes are configured with the opt path.

### When the Ruby build fails

den tries the bundled Ruby build first (macOS), then `brew ruby` when a brew
serves den's prefix. If a Ruby build ran and failed, den builds with its
native recipe parser instead, but not silently:

* It prints a warning naming the Ruby failure (exit status and the last line
  of its output).
* The keg's receipt records `"fallback": "native"` and the reason in
  `fallback_reason`.
* `den doctor` marks the keg `[fallback=native]` in its source-built list and
  warns, suggesting a rebuild once the Ruby build works.

`den set build.strict true` turns the fallback into an error: the install
fails with the Ruby build's reason and nothing is built. `build.strict` is
`false` by default. A source checksum mismatch never falls back, in either
mode. Where no Ruby build is available (Linux, or no formula source), the
parser is the only path, so it is not a fallback.

## Build sandbox (macOS)

On macOS every configure, build and install command runs under
`/usr/bin/sandbox-exec` with a Seatbelt profile that denies writes to:

* den's prefix (and its Cellar, if that resolves elsewhere) and `$DEN_HOME`;
* Homebrew's default prefixes, `/opt/homebrew` (its Cellar included) and
  `/usr/local`, and the detected Homebrew prefix and Cellar
  (`HOMEBREW_PREFIX` / `HOMEBREW_CELLAR`) where they differ;
* `$HOME/Library`, except `$HOME/Library/Developer`, where Xcode tools keep
  their caches (Homebrew's sandbox allows it too).

Within den's trees, three directories stay writable:

* the build directory, `<den_home>/cache/build/<name>-<version>`;
* the staging DESTDIR, `<den_home>/cache/build/<name>-<version>.destdir`;
* the cache temp directory, `<den_home>/cache/tmp/<name>-<version>`, which
  native-fallback build commands get as `TMPDIR` and which is removed after
  the build.

Language tool caches live in the cache temp directory too, on every build
path: `GOCACHE` (`go-build`), `GOMODCACHE` (`go/pkg/mod`), `GOPATH` (`go`),
`PIP_CACHE_DIR` (`pip`), `npm_config_cache` (`npm`) and `CARGO_HOME`
(`cargo`). Their defaults are under `$HOME/Library/Caches`, which the sandbox
denies, or in the home directory, where a build should leave nothing behind.

Denied writes include creating, overwriting, removing, renaming, chmod and
extended attributes. Seatbelt matches real paths, so a write through a
symlink (such as the opt link to an older keg) is denied when its target is.
Setuid and setgid bits are refused everywhere. Everything outside the denied
trees stays as writable as it is for den itself. A build that writes into the
prefix directly, ignores DESTDIR, or writes elsewhere under `$DEN_HOME` or a
denied system root fails, and nothing it tried to write remains.

The profile follows Homebrew's (`Library/Homebrew/sandbox.rb`): a
`(debug deny)` header, paths resolved through symlinks, the same string
quoting, and `sandbox-exec -p <profile>`. Homebrew's is an allow list for its
whole build process. Den's is a deny list of the trees above, and it
does not limit network or Mach lookups. Like Homebrew, den will not nest
sandboxes: run inside a sandbox, it refuses to build from source.

What runs inside the sandbox:

* **Native fallback:** every build command.
* **Bundled Ruby (`den_build.rb`, and `brew ruby`):** every command the
  formula's install method runs through `system` (Formula#system). The Ruby
  process itself is not sandboxed. The formula's Ruby file operations on the
  keg (`bin.install`, `rm`, `inreplace`), source download and extraction, and
  den's stage merge run outside it, as they must write the keg.

Tests rebase the fixed system roots (`/opt/homebrew`, `/usr/local`,
`$HOME/Library`) under `DEN_BUILD_SANDBOX_SYSTEM_ROOT`, so they exercise the
denials without touching the real paths.

**Future work.** A full allow list, as Homebrew uses (writes only to the
build, staging and temp directories plus a short list of tool caches), and
network restriction for the build steps (sources and resources are already
fetched and checksummed before the build starts). Both need a survey of
which formulae write elsewhere or fetch during the build.

**Linux gap.** Linux has no sandbox yet. Landlock is allow-list only, so it
cannot express "everything except den's trees" without listing the rest of
the filesystem. bwrap is not generally installed. Builds there run
unsandboxed, and the opt-path check above is the only guard.

## Mach-O linkage (macOS)

After a successful build, before the keg is recorded as den's, den fixes the
linkage of every Mach-O executable, dylib and bundle in the keg. It walks the
whole keg, not just `lib` and `bin`: plugin directories such as
`lib/engines-3` and `lib/ossl-modules`, `libexec/**` (`git-core`, gettext's
helpers) and anything else. Symlinks, static archives, object files and
non-Mach-O files are skipped (`src/build/macho_fixup.h`).

* **The keg's own files by `@loader_path`.** A load command or rpath that
  names the keg, either by its versioned path
  (`<prefix>/Cellar/<name>/<version>/…`, or another version of the same
  formula) or through its own opt link (`<prefix>/opt/<name>/…`, which is
  what an opt-prefix build records), becomes `@loader_path/<relative path>`.
  The keg keeps loading its own libraries after its opt link moves to
  another version, or before it has one.
* **Dependencies by opt path.** A load command or rpath that names a
  dependency's versioned keg (`<prefix>/Cellar/<dep>/<version>/…`) becomes
  `<prefix>/opt/<dep>/…`, so the keg follows its dependency's opt link.
  Bottles do the same where den owns `<prefix>/opt/<dep>`; in a prefix
  shared with Homebrew, or where the link names a keg den did not pour, den
  pins them to the exact versioned keg (see `bind_opt_reference` in
  `src/build/relocate.h`).
* **Ids by opt path.** An absolute dylib id becomes
  `<prefix>/opt/<name>/<path in keg>`, the stable name that dependents link
  against. `@rpath/…` ids are kept.
* **rpaths that still resolve.** Duplicate rpaths left by the rewrite are
  deleted. An `@rpath/…` load that no rpath resolves gets an `@loader_path`
  rpath to the keg's `lib` (or the file's own directory) when the library is
  there.

Each modified file is re-signed ad hoc (`codesign -f -s -`), as is any
unmodified file that fails `codesign -v`. den then verifies the keg: every
non-weak dylib load resolves (`@loader_path`, `@executable_path` for
executables, `@rpath` through the file's rpaths, and absolute paths; dyld
shared-cache paths under `/usr/lib` and `/System` count as resolved), no
load command, id or rpath names a path under `<prefix>/Cellar`, and every
Mach-O file passes `codesign -v`. Any failure, or a rewrite that does not fit
in a file's Mach-O header, fails the build and removes the keg. Linux builds
are not changed by this step.

## Source archives

Sources are cached as `<den_home>/cache/sources/<name>-<version><suffix>`,
keeping the URL's archive suffix (`.tar.gz`, `.tar.xz`, `.tar.bz2`,
`.tar.zst`, `.zip`, …; `.src` when the URL has none). Extraction detects the
format from content: `tar xf` in the Ruby build, libarchive in the fallback.

## Verified inputs

A homebrew-core formula's Ruby source is fetched at the commit the index
records (`tap_git_head` in Homebrew's formula JSON), not from `master`, and
checked against the index's `ruby_source_checksum`. A mismatch stops the
build. An index without the commit falls back to `master` with a warning.

The source tarball must have a sha256, and so must every `resource` and
external patch: the bundled Ruby build refuses one without, and does not fall
back to the parser.

**Resources.** The bundled Ruby build fetches every `resource` into den's
source cache before the build starts, as
`<cache>/sources/<name>-<version>--<resource><suffix>` (the resource name with
anything but letters, digits and `._+@-` turned into `-`), and checks each
against its sha256; a mismatch fails the build before any step runs.
`resource("x").stage { … }` then unpacks that file into a scratch directory
under `<cache>/build` and runs the block inside the archive's single top-level
directory, if it has one; `dir.install resource("x")` moves the unpacked
contents into `dir`. Homebrew's downloader and cache are not used. This is
what git's macOS resources need: the `Authen::SASL` and `Net::SMTP::SSL` Perl
modules (staged with a block) and the `html` and `man` pages (installed into
a directory). A resource with its own patches is refused for now.

## Receipt

A source-built keg's `DEN_RECEIPT.json` records, beside the fields every den
keg has:

- `built_from_source`: `true`.
- `source`: the tarball `url` and `sha256`, plus `resources` (`name`, `url`,
  `sha256`) and `patches` (`url`, `sha256`).
- `formula`: the `tap`, the `commit` the Ruby source was fetched at (empty for
  a local tap or an unpinned fetch), and its `path`.
- `toolchain`: the macOS `sdk` path and the first line of `clang --version`.
- `opt_prefix_baked`: the `<prefix>/opt/<name>` path the build compiled in.
- `fallback` and `fallback_reason`: present only when den's native parser
  built the keg after a Ruby build failed (`"fallback": "native"`; see
  [When the Ruby build fails](#when-the-ruby-build-fails)).

`den doctor` lists source-built kegs. It warns about any whose formula, at the
same version, now names a source tarball with a different sha256, and
suggests `den install --build-from-source <name>` to rebuild it.

## Per-formula overrides

The generic mode is the rule. A formula whose install step ignores DESTDIR
(it writes `<prefix>/opt/<name>` directly), or that must see its keg path
while building, fails the build and leaves the live prefix as it was. Such a
formula may get a per-formula override only as an escape hatch. Each override
must be listed here with the reason it is needed and must have a regression
test.

There are no overrides yet.
