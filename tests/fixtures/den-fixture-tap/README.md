# verify-release fixture tap

The tap that `scripts/verify-release.sh`'s `tap` step installs from, so the
step depends on nothing outside this repository.

- `Formula/den-vr-hello.rb.in` — a prebuilt-download formula (top-level
  `url`/`sha256`, `bin.install`), the shape den installs natively from a tap.
- `payload/den-vr-hello` — the file the download contains.

At run time the step tars the payload into its scratch root, renders the
formula with that tarball's `file://` URL and sha256 into a scratch tap
directory, and runs `den tap add den-fixture/verify-release <dir>`, then
`den install` / `den uninstall` / `den tap --remove` against it.
