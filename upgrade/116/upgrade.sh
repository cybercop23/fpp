#!/bin/bash
#####################################
# Upgrade 116: Migrate NTP daemon from ntpsec to chrony
#
# ntpsec sources its client queries from UDP source port 123, which some
# consumer routers / ISP gateways (e.g. newer Comcast gateways) drop as
# suspected NTP reflection/amplification -- so time never syncs on those
# networks (see issue #2736).  chrony sources from ephemeral ports and syncs
# cleanly.  This also preserves FPP's long-standing behavior of running an
# always-on NTP server that other controllers can point at (the "Override
# default NTP Server" setting), including serving from the local clock/RTC when
# there is no reachable upstream.
#
# Fresh installs get all of this from SD/FPP_Install.sh (configure_ntp); this
# upgrade brings already-installed systems over.  Idempotent -- safe to re-run.
#####################################

BINDIR=$(cd $(dirname $0) && pwd)
. ${BINDIR}/../../scripts/common

echo "FPP - Upgrade 116: Migrate NTP daemon from ntpsec to chrony"
echo "==========================================================="

if [ "${FPPPLATFORM}" = "MacOS" ]; then
    echo "  Skipping on MacOS (uses the system time service)"
    exit 0
fi

#######################################
# 1. Remove ntpsec and any competing time daemon (all bind UDP/123).
#######################################
echo "  Stopping/removing ntpsec and systemd-timesyncd (they conflict on port 123)"
systemctl disable --now ntpsec.service > /dev/null 2>&1 || true
if dpkg -l ntpsec > /dev/null 2>&1; then
    apt-get -y purge ntpsec > /dev/null 2>&1 || true
fi
systemctl disable --now systemd-timesyncd.service > /dev/null 2>&1 || true
systemctl mask systemd-timesyncd.service > /dev/null 2>&1 || true

#######################################
# 2. Install chrony.
#######################################
if ! dpkg -l chrony 2>/dev/null | grep -q '^ii'; then
    echo "  Installing chrony"
    apt-get update
    apt-get install -y chrony
else
    echo "  chrony already installed"
fi
mkdir -p /var/log/chrony && chown _chrony:_chrony /var/log/chrony 2>/dev/null || true

#######################################
# 3. Write FPP's managed chrony.conf, preserving any custom ntpServer setting.
#######################################
echo "  Writing /etc/chrony/chrony.conf"
NTPSERVER=$(getSetting "ntpServer")
if [ -n "${NTPSERVER}" ]; then
    SOURCE_LINE="server ${NTPSERVER} iburst"
else
    SOURCE_LINE="pool falconplayer.pool.ntp.org iburst minpoll 8 maxpoll 12"
fi

cat > /etc/chrony/chrony.conf <<EOF
# FPP-managed chrony configuration.
# The pool/server source line below is rewritten by the "Override default NTP
# Server" web setting; the rest is regenerated on install.

# Time source. FPP's public pool by default; replaced by the ntpServer setting.
${SOURCE_LINE}

# Record the clock's drift rate so time stays reasonable across reboots even
# without an RTC or a reachable upstream.
driftfile /var/lib/chrony/chrony.drift

# ntpd "-g" equivalent: step (rather than slew) the clock for the first few
# updates so an RTC-less board that boots with a wildly wrong time corrects fast.
makestep 1.0 3

# Read from / write back to the hardware RTC when one is present.
rtcsync

# DHCP-provided NTP servers are dropped into this dir by the networkd-dispatcher
# chrony hook, but only when the UseNTPFromDHCP setting is enabled. Empty/absent
# otherwise, which is harmless.
sourcedir /run/chrony-dhcp

# --- FPP as an NTP server for the rest of the fleet -------------------------
# Users run on arbitrary IP ranges, so the server is NOT restricted to a subnet.
allow all
# Serve our own clock even with no synced upstream, so a controller acting as
# the site time source keeps answering when the internet path is down/filtered.
local stratum 10
EOF

#######################################
# 4. Ensure /run/chrony-dhcp exists at every boot (tmpfs is empty at boot).
#######################################
cat > /etc/tmpfiles.d/fpp-chrony.conf <<'EOF'
d /run/chrony-dhcp 0755 root root -
EOF
mkdir -p /run/chrony-dhcp

#######################################
# 5. Start chrony after the fast clock set in fpp_postnetwork (matches install).
#######################################
mkdir -p /etc/systemd/system/chrony.service.d
cat > /etc/systemd/system/chrony.service.d/fpp-defer.conf <<'EOF'
[Unit]
After=fpp_postnetwork.service
EOF

#######################################
# 6. Replace the old routable.d/ntpd time-sync hook with the chrony one.
#######################################
if [ -f /opt/fpp/etc/networkd-dispatcher/routable.d/chrony ]; then
    rm -f /etc/networkd-dispatcher/routable.d/ntpd
    cp /opt/fpp/etc/networkd-dispatcher/routable.d/chrony /etc/networkd-dispatcher/routable.d/chrony
    chown root:root /etc/networkd-dispatcher/routable.d/chrony
    chmod 755 /etc/networkd-dispatcher/routable.d/chrony
    systemctl restart networkd-dispatcher 2>/dev/null || true
fi

#######################################
# 7. Enable + (re)start chrony.
#######################################
systemctl daemon-reload
systemctl enable chrony > /dev/null 2>&1 || true
systemctl restart chrony

echo ""
echo "Upgrade 116 complete -- FPP now uses chrony for time sync and NTP serving."
exit 0
