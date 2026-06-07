FROM ubuntu:24.04

# Install only the packages required by install.sh and smoke.sh.
# No build toolchain — we are testing pre-built binaries.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        sudo \
        tar \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*

# Create an unprivileged user that can sudo for any setup steps.
RUN useradd -m -s /bin/bash tester \
 && echo 'tester ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/tester \
 && chmod 0440 /etc/sudoers.d/tester

USER tester
WORKDIR /home/tester

# No ENTRYPOINT — the run.sh runner controls invocation via
#   docker run … bash -c …
