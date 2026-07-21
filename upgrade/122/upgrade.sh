#!/bin/bash
#####################################

BINDIR=$(cd $(dirname $0) && pwd)
. ${BINDIR}/../../scripts/common

# Default MultiSync mode is changing from Multicast to Unicast-to-known-remotes
# for new/fresh installs (see www/settings.json MultiSyncMulticast/MultiSyncUnicast).
# Pin existing multisync-enabled systems to their current effective mode
# (Multicast) so this upgrade does not silently change their sync behavior.
if [ "$(getSetting MultiSyncEnabled)" == "1" ]; then
    if [ "$(getSetting MultiSyncMulticast)" != "1" ] && [ "$(getSetting MultiSyncBroadcast)" != "1" ] && [ "$(getSetting MultiSyncUnicast)" != "1" ]; then
        setSetting MultiSyncMulticast 1
        setSetting MultiSyncUnicast 0
    fi
fi
