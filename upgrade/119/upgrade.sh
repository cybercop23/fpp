#!/bin/bash
#####################################
# Upgrade 119: Remove uv/pipx, ensure pip
#
# Upgrade 118 adopted pipx + uv as the sanctioned way for plugins to install
# Python packages, believing `uv pip install --system` avoided needing
# `pip install --break-system-packages` entirely. Live testing on real hardware
# (fpp-live-follow, darylc/fpp-data-ci#268) found that's false: `uv pip install
# --system` hits the exact same PEP 668 "externally managed environment" refusal
# as bare pip does, and has needed `--break-system-packages` since uv's earliest
# releases (astral-sh/uv#2249, March 2024) -- this was never actually PEP
# 668-safe on its own, it was untested against a real PEP-668-enforcing image.
#
# Separately verified: `--break-system-packages` IS safe regardless of which
# tool passes it, because both `uv pip install --system` and plain
# `pip install` target /usr/local/lib/python3.x/dist-packages, which is not
# dpkg-tracked (apt-installed python3-* packages live in
# /usr/lib/python3/dist-packages instead) -- the "corrupts the system Python"
# risk PEP 668 exists to prevent doesn't actually apply once the flag is
# combined with that target directory.
#
# Given the flag is required and safe either way, FPP drops uv/pipx as an
# extra bootstrap dependency (no Debian package, needs pipx as a go-between)
# in favor of plain pip -- a first-class apt package with a much larger
# community/troubleshooting surface. See plugin.php's ResolvePluginDependencies
# for the corresponding install-command change.
#
# Deliberately does NOT uninstall any existing uv/pipx install on upgrade --
# forcibly removing tools from already-deployed field hardware carries more
# risk than leaving an unused binary in place. This only ensures pip is
# present; FPP simply stops invoking uv going forward.
#
# Fresh installs get python3-pip from SD/FPP_Install.sh; this upgrade brings
# already-installed systems over. Idempotent -- safe to re-run.
#####################################

BINDIR=$(cd $(dirname $0) && pwd)

# See upgrade/117 for why FPPDIR must be resolved before sourcing common: an
# upgrade step lives in ${FPPDIR}/upgrade/<n>/, two levels below FPPDIR, not one.
FPPDIR=$(cd "${BINDIR}/../.." && pwd)
. ${BINDIR}/../../scripts/common

echo "FPP - Upgrade 119: Remove uv/pipx, ensure pip"
echo "================================"

if [ "${FPPPLATFORM}" = "MacOS" ]; then
    # Homebrew's python3 formula bundles pip already -- no separate package to
    # install, unlike Debian which splits it out specifically to make PEP 668
    # opt-in explicit.
    echo "  macOS: pip ships with Homebrew's python3, nothing to do"
    exit 0
fi

if command -v pip3 > /dev/null 2>&1 || python3 -m pip --version > /dev/null 2>&1; then
    echo "  pip already available"
else
    echo "  Installing python3-pip"
    apt-get update
    apt-get install -y python3-pip
fi

echo ""
echo "Upgrade 119 complete -- pip is available for Python-based plugins."
exit 0
