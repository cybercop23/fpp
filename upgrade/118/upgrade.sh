#!/bin/bash
#####################################
# Upgrade 118: Install pipx + uv
#
# pipx installs a Python CLI tool into its own isolated venv automatically, with
# just the tool's entry point exposed on PATH -- no manual venv bookkeeping, and
# no reaching for `pip install --break-system-packages` (which corrupts the
# system Python) or piping a third-party installer script into a shell.
# Several plugins (fpp-live-follow, fpp-performance-capture) currently do the
# latter to get the `uv` tool; pipx is the sanctioned alternative going forward.
#
# uv itself has no Debian package, so once pipx is available we use it to
# install uv the same clean way -- `pipx install --global uv` rather than
# `curl astral.sh/uv/install.sh | sh`. --global puts it on PATH for every
# user (plugin install scripts may run as fpp or root), not just whoever ran
# this upgrade.
#
# Fresh installs get both from SD/FPP_Install.sh; this upgrade brings
# already-installed systems over. Idempotent -- safe to re-run.
#####################################

BINDIR=$(cd $(dirname $0) && pwd)

# See upgrade/117 for why FPPDIR must be resolved before sourcing common: an
# upgrade step lives in ${FPPDIR}/upgrade/<n>/, two levels below FPPDIR, not one.
FPPDIR=$(cd "${BINDIR}/../.." && pwd)
. ${BINDIR}/../../scripts/common

echo "FPP - Upgrade 118: Install pipx + uv"
echo "================================"

if [ "${FPPPLATFORM}" = "MacOS" ]; then
    if command -v pipx > /dev/null 2>&1; then
        echo "  pipx already installed"
    elif command -v brew > /dev/null 2>&1; then
        echo "  Installing pipx via Homebrew"
        brew install pipx
    else
        echo "  WARNING: Homebrew not found; install pipx manually (brew install pipx)"
    fi

    if command -v uv > /dev/null 2>&1; then
        echo "  uv already installed"
    elif command -v pipx > /dev/null 2>&1; then
        echo "  Installing uv via pipx"
        pipx install --global uv || echo "  WARNING: 'pipx install --global uv' failed"
    fi
    exit 0
fi

if dpkg -l pipx 2>/dev/null | grep -q '^ii'; then
    echo "  pipx already installed"
else
    echo "  Installing pipx"
    apt-get update
    apt-get install -y pipx
fi

if command -v uv > /dev/null 2>&1; then
    echo "  uv already installed"
elif command -v pipx > /dev/null 2>&1; then
    echo "  Installing uv via pipx"
    pipx install --global uv || echo "  WARNING: 'pipx install --global uv' failed"
fi

echo ""
echo "Upgrade 118 complete -- uv is available for Python-based plugins."
exit 0
