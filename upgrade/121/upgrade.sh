#!/bin/bash
#####################################
# Upgrade 121: Remove the deprecated DPI_FPS setting
#
# DPIPixels no longer uses a fixed 20/40 fps framebuffer mode selected by
# DPI_FPS (and a reboot to switch).  The output frame rate is now sized to the
# longest string and set per-sequence at runtime via the vertical blanking, so
# the setting is meaningless.  It was only ever written internally, never by a
# UI, so just drop any stale key left in the settings file.
#####################################

BINDIR=$(cd $(dirname $0) && pwd)
. ${BINDIR}/../../scripts/common

echo "FPP - Upgrade 121: Remove deprecated DPI_FPS setting"

SETTINGS_FILE="${FPPHOME}/media/settings"

if [ -f "${SETTINGS_FILE}" ]; then
    if grep -qE '^DPI_FPS[[:space:]]*=' "${SETTINGS_FILE}"; then
        sed -i -E '/^DPI_FPS[[:space:]]*=/d' "${SETTINGS_FILE}"
        echo "  Removed deprecated DPI_FPS from settings"
    else
        echo "  DPI_FPS not present in settings (nothing to do)"
    fi
fi

exit 0
