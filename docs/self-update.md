# `den self-update` and the daemon

## How the binary is replaced

`den self-update` downloads the release tarball, verifies its SHA256, extracts
it into a private temporary directory and then **renames** the new `den` over
`~/.den/bin/den`. The rename points the path at the new file's own inode; the
old inode is never written to and stays intact until every process using it
(for example a running daemon) exits. If the temporary directory is on a
different filesystem, the new binary is first copied to a newly created,
uniquely named file beside the target and that file is renamed into place.

This matters on macOS: code signatures are validated per vnode, so writing new
bytes into the inode a running process was launched from gets that process
killed with `OS_REASON_CODESIGNING`. den never does that.

The installer (`install.sh`) likewise moves the new binary into place with
`mv`.

## The daemon after an update

A daemon that keeps running after the update is still executing the old
binary. `den self-update` therefore checks for a running daemon once the new
binary is in place:

- **launchd-managed** (`den daemon install`): the job is re-bootstrapped —
  `launchctl bootout gui/<uid>/dev.den.daemon` (the daemon receives SIGTERM and
  shuts down cleanly) followed by
  `launchctl bootstrap gui/<uid> ~/Library/LaunchAgents/dev.den.daemon.plist` —
  so launchd re-reads the program and starts it on the new binary. den then
  waits up to 15 seconds for `launchctl print` to report a new pid. One
  `OS_REASON_CODESIGNING` spawn failure is tolerated (den kickstarts the job
  once); a second one is reported as a failure. The output says
  `Restarted the den daemon so it runs the new binary.`
  When the update is triggered by the daemon itself, the bootout would kill
  the process doing the restart, so den hands the bootout/bootstrap to a
  detached one-shot helper (`/bin/sh`, double-forked into its own session)
  that outlives the daemon and logs to `~/.den/daemon-restart.log`.
- **started by hand** (`den daemon run`): the daemon is sent SIGTERM and
  self-update tells you to start it again.
- **not running**: nothing happens.

If the restart fails, self-update says so and points at `den daemon install`,
which reloads the launchd job the same way.

`launchctl` is always run with a scrubbed environment (`env -i` with only
`HOME`, `USER` and `PATH=/usr/bin:/bin:/usr/sbin:/sbin`), so nothing from the
calling session (`SSH_AUTH_SOCK`, `SSH_*`, `TERM`, shell variables) can reach
the service: launchd builds the daemon's environment from the plist's
`EnvironmentVariables`, its own defaults and the gui domain only.

**This is a stopgap.** It relies on behaviour observed on macOS 27 — that
re-bootstrapping a job drops the launch constraint launchd recorded for the old
binary — not on a documented guarantee. The real fix is signing den with a
team identity, so the recorded requirement stays satisfied across updates.

## `last exit reason = OS_REASON_CODESIGNING`

`launchctl print gui/$(id -u)/dev.den.daemon` may show

```
last exit reason = OS_REASON_CODESIGNING
```

after the binary under a launchd job has changed. launchd records a code
requirement for the job's program (`managed LWCR` in the job's properties);
when the job is respawned by `KeepAlive` after the binary was swapped but
without the job being re-bootstrapped, the first respawn can be rejected
against the requirement recorded for the old binary. launchd then re-reads the
program and the next respawn (about 10 seconds later, launchd's throttle)
succeeds. `runs` in the same output goes up by two instead of one.

It is transient and harmless: the daemon comes back on the new binary and no
state is lost (the old process shut down cleanly on SIGTERM before the
respawn). Re-bootstrapping the job, as `den self-update` now does, avoids the
rejected respawn in the first place. If you replaced the binary some other way
(e.g. re-running `install.sh`) and the line bothers you, run
`den daemon install` to re-bootstrap the job.
