# den macOS harness

SSH-based end-to-end release-candidate validation for den on a real macOS machine.
This harness is intentionally **not in CI** — runs are dev-triggered against a
dedicated physical test host. See `tests/harness/README.md` for how this fits into
the broader harness architecture.

## One-time setup

### 1. Enable Remote Login on the test machine

On the test MacBook, go to **System Settings → General → Sharing → Remote Login**
and toggle it on. This enables the SSH server (`sshd`).

### 2. Set up key-based auth

Copy your public key to the test machine so SSH works without a password:

```sh
ssh-copy-id marcelo@192.168.1.112
# or, if you prefer:
ssh-keygen -t ed25519 -f ~/.ssh/id_ed25519   # if not already done
ssh-copy-id -i ~/.ssh/id_ed25519 marcelo@192.168.1.112
```

Verify it works without a password prompt:

```sh
ssh marcelo@192.168.1.112 uname -s   # should print: Darwin
```

### 3. Add the SSH alias

Add the following stanza to `~/.ssh/config` on your development machine:

```
Host den-test-mac
    HostName 192.168.1.112
    User marcelo
    IdentitiesOnly yes
    IdentityFile ~/.ssh/id_ed25519
    ConnectTimeout 5
```

Verify:

```sh
ssh den-test-mac uname -s   # should print: Darwin
```

### 4. Fix clamshell sleep (IMPORTANT)

When the test laptop lid is closed, macOS sleeps and becomes unreachable over SSH.
To prevent this, run the following **on the test machine** (not on your dev machine):

```sh
sudo pmset -b disablesleep 1
```

This disables lid-close sleep while on battery (`-b`). It is persistent across reboots
but applies only to battery power — plugging in AC power respects normal sleep settings.

To re-enable:

```sh
sudo pmset -b disablesleep 0
```

The harness checks for this at preflight and warns (but does not abort) if `SleepDisabled=0`.

## Per-run usage

Build den locally, then:

```sh
make harness-macos              # binary mode: test the locally-built binary
```

Or to test a published release:

```sh
tests/harness/macos/run.sh --release v0.12.0
```

Full interface:

```
tests/harness/macos/run.sh --binary PATH  [--host ALIAS] [--keep-sandbox] [--reset] [--test-pkg NAME]
tests/harness/macos/run.sh --release VER  [--host ALIAS] [--keep-sandbox] [--reset] [--test-pkg NAME]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--binary PATH` | — | scp a pre-built local binary to the test host |
| `--release VER` | — | run `install.sh` on the test host with `DEN_VERSION=VER` |
| `--host ALIAS` | `den-test-mac` | SSH alias from `~/.ssh/config` |
| `--keep-sandbox` | off | preserve remote sandbox after run (default: keep on failure, delete on success) |
| `--reset` | off | wipe all `/tmp/den-harness/` dirs on the remote and exit |
| `--test-pkg NAME` | `jq` | package to exercise in the smoke set |

## Isolation guarantees

The harness enforces strict guards before any smoke step runs:

- **Loopback guard**: refuses to run if the SSH alias resolves to the local machine
  (`localhost`, `127.0.0.1`, `::1`, or the controller's hostname/IP).
- **Sandbox isolation**: creates a unique path `/tmp/den-harness/<run-id>` on the remote.
  `DEN_HOME` always points there — never to `~/.den` or `/opt/homebrew`.
- **Darwin check**: aborts immediately if `uname -s` on the remote is not `Darwin`.
- **Collision check**: aborts if the generated sandbox path already exists (per-run nonce prevents this in practice).

The harness does **not** take destructive cleanup actions against `/opt/homebrew` if smoke fails.
The smoke set's own `uninstall` step cleans up installed packages.

## Troubleshooting

### Connection refused / timeout

- Verify Remote Login is enabled: **System Settings → General → Sharing → Remote Login**.
- Verify the IP in `~/.ssh/config` is current (`arp -a`, or check the test machine's System Settings → Network).
- Verify the test machine is on the same network and awake.

### Host key verification failed

The test machine's host key changed (e.g., after an OS reinstall). Remove the stale entry:

```sh
ssh-keygen -R 192.168.1.112
ssh-keygen -R den-test-mac
```

Then connect once interactively to accept the new key:

```sh
ssh den-test-mac true
```

### Machine sleeps during the run

The machine went to sleep (lid close or idle). Run on the test machine:

```sh
sudo pmset -b disablesleep 1
```

If the run hung mid-way, the sandbox may have been left behind. Clean up with:

```sh
tests/harness/macos/run.sh --reset
# then retry
```

### Sandbox left behind after a failure

Sandboxes are preserved on failure so you can inspect them:

```sh
ssh den-test-mac
ls /tmp/den-harness/
```

When you are done inspecting, wipe them all:

```sh
tests/harness/macos/run.sh --reset
```

### Permission denied (publickey)

Key-based auth is not set up. Run:

```sh
ssh-copy-id -i ~/.ssh/id_ed25519 marcelo@192.168.1.112
```

## Notes

- This harness is **not in CI**. The test host is a dev-controlled physical machine,
  not a CI runner. Runs are triggered manually via `make harness-macos`.
- Shared Cellar pollution: the smoke set installs and uninstalls `jq` (or `--test-pkg`)
  via real Homebrew bottles into `/opt/homebrew/Cellar/`. The `uninstall` smoke step
  removes it. If smoke fails before uninstall, `jq` will remain in the Cellar — that
  is accepted and harmless.
