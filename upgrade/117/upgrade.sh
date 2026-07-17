#!/bin/bash
#####################################
# Upgrade 117: Refresh /etc/logrotate.d/fpp_other_logs and normalise log ownership
#
# fpp_other_logs gained a `create 0664 fpp fpp` line, but nothing was delivering
# it to existing systems: /etc/logrotate.d/ is populated ONLY by
# SD/FPP_Install.sh (`cp /opt/fpp/etc/logrotate.d/*`), which runs at image build
# / fresh install.  The FPP-software upgrade path (git_pull -> upgrade_config)
# never touches /etc, so every already-installed box would have kept the old
# config indefinitely and never seen the fix.
#
# Why the `create` line matters: logrotate renames the log and, without `create`,
# makes nothing in its place -- so the file is recreated by whichever process
# logs next.  For fppd.log that is normally fppd itself, which runs as root
# (fppd.service sets no User=), so the log came back root:root 0644 inside an
# fpp-owned directory.  Every writer that is not root was then denied: the PHP
# breadcrumbs (backup, mp3gain) run as Apache/fpp and their writes are
# error-suppressed best-effort, so they failed SILENTLY and stayed broken until
# the next reboot.  fppd.log is a merged log with several writers now, so this is
# no longer a theoretical case.
#
# Step 2 is the part the config change cannot do by itself: `create` only takes
# effect at the NEXT rotation, so a log that is ALREADY root-owned on this box
# stays broken until then.  We normalise the existing files here.
#
# Idempotent -- safe to re-run.  Only fpp_other_logs is copied: fpp_apache2_logs
# is unchanged, and a blanket copy would be a wider claim than this fix needs.
#####################################

BINDIR=$(cd $(dirname $0) && pwd)

# Resolve FPPDIR before sourcing common, and let common inherit it.
#
# common derives FPPDIR from `dirname $0` -- and $0 is the EXECUTING script, not
# common itself -- so it assumes its caller lives in ${FPPDIR}/scripts/.  An
# upgrade step lives in ${FPPDIR}/upgrade/<n>/, so common would compute
# FPPDIR=${FPPDIR}/upgrade.  That is not only wrong for our own paths: it makes
# ${FPPDIR}/www/media_root.txt unreadable, so MEDIADIR/LOGDIR/FPPHOME silently
# fall back to the /home/fpp defaults and would be WRONG on a box with a custom
# media root.  common respects an FPPDIR that is already set (FPPDIR=${FPPDIR:-...}),
# so setting it here fixes all of them at once.
FPPDIR=$(cd "${BINDIR}/../.." && pwd)
. ${BINDIR}/../../scripts/common

echo "FPP - Upgrade 117: Refresh logrotate config for FPP logs"
echo "======================================================="

if [ "${FPPPLATFORM}" = "MacOS" ]; then
    echo "  Skipping on MacOS (no /etc/logrotate.d, no fpp user)"
    exit 0
fi

#######################################
# 1. Install the current fpp_other_logs.
#######################################
SRC="${FPPDIR}/etc/logrotate.d/fpp_other_logs"
DST="/etc/logrotate.d/fpp_other_logs"

if [ ! -d /etc/logrotate.d ]; then
    echo "  /etc/logrotate.d does not exist; nothing to refresh"
elif [ ! -f "${SRC}" ]; then
    # Not fatal -- the code-level fallbacks (log.cpp's fchown on create,
    # ensureLogFile in scripts/common) still normalise ownership without it.
    # But say so loudly rather than skipping quietly: this file ships with FPP,
    # so its absence means something is wrong with the install.
    echo "  WARNING: ${SRC} is missing -- logrotate config NOT refreshed."
    echo "           fppd.log ownership still self-heals in code, but check the install."
else
    if cmp -s "${SRC}" "${DST}" 2>/dev/null; then
        echo "  ${DST} already current"
    else
        echo "  Installing ${DST}"
        cp "${SRC}" "${DST}"
        chown root:root "${DST}"
        chmod 644 "${DST}"
    fi
fi

#######################################
# 2. Normalise ownership of the logs FPP's own writers share.
#
# `create` only applies from the next rotation onward, so fix what is on disk
# now.  Only FPP-core logs: plugin-written logs (fpp-plugin-*.log and friends)
# belong to their authors, and the apache logs are rotated by fpp_apache2_logs.
#######################################
for LOG in fppd.log fpp_system_upgrades.log fpp_plugin_manager.log; do
    if [ -f "${LOGDIR}/${LOG}" ]; then
        CUROWNER=$(stat -c '%U:%G' "${LOGDIR}/${LOG}" 2>/dev/null)
        if [ "${CUROWNER}" != "${FPPUSER}:${FPPGROUP}" ]; then
            echo "  Fixing ${LOG} (was ${CUROWNER})"
        fi
        chown ${FPPUSER}:${FPPGROUP} "${LOGDIR}/${LOG}" 2>/dev/null || true
        chmod 664 "${LOGDIR}/${LOG}" 2>/dev/null || true
    fi
done

echo ""
echo "Upgrade 117 complete -- fppd.log survives a rotation writable by every FPP writer."
exit 0
