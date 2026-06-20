FROM ubuntu:24.04

# Install only the packages required by install.sh and smoke.sh.
# No build toolchain — we are testing pre-built binaries.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        libarchive13 \
        sudo \
        tar \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*

# Create an unprivileged user that can sudo for any setup steps.
RUN useradd -m -s /bin/bash tester \
 && echo 'tester ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/tester \
 && chmod 0440 /etc/sudoers.d/tester

# den installs Linux bottles into the Linuxbrew prefix.  The bottles are not
# relocatable — they expect exactly /home/linuxbrew/.linuxbrew/Cellar/...
# Create the prefix up-front, owned by tester, so the smoke install works in
# a clean container without needing a full Linuxbrew install.
RUN install -d -o tester -g tester \
        /home/linuxbrew/.linuxbrew/Cellar \
        /home/linuxbrew/.linuxbrew/Caskroom

USER tester
WORKDIR /home/tester

# No ENTRYPOINT — the run.sh runner controls invocation via
#   docker run … bash -c …
