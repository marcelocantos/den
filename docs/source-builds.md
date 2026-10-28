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
  writes through it: den detects that and fails the build, but cannot undo
  the write, so uninstall and reinstall the older version.
* **Dependencies by opt path.** A dependency's `opt_prefix`, `opt_lib`,
  `opt_include`, and the rest resolve to `<prefix>/opt/<dep>` (or to the
  dependency's keg if it has no opt link yet).

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

## Source archives

Sources are cached as `<den_home>/cache/sources/<name>-<version><suffix>`,
keeping the URL's archive suffix (`.tar.gz`, `.tar.xz`, `.tar.bz2`,
`.tar.zst`, `.zip`, …; `.src` when the URL has none). Extraction detects the
format from content: `tar xf` in the Ruby build, libarchive in the fallback.

## Per-formula overrides

The generic mode is the rule. A formula whose install step ignores DESTDIR
(it writes `<prefix>/opt/<name>` directly), or that must see its keg path
while building, fails the build and leaves the live prefix as it was. Such a
formula may get a per-formula override only as an escape hatch. Each override
must be listed here with the reason it is needed and must have a regression
test.

There are no overrides yet.
