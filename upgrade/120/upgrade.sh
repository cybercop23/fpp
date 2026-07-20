#!/bin/bash
#####################################
# Enable the fppd status WebSocket (/fppdws).
#
# fppd now pushes its status over a WebSocket at /fppdws instead of every page
# polling api/system/status on a timer.  Apache reverse-proxies /fppdws to fppd
# with a ProxyPass ws:// line (a WebSocket upgrade cannot go through the
# RewriteRule [P] the other fppd routes use).  On Apache >= 2.4.47 (all
# supported FPP OS images) mod_proxy_http tunnels the upgrade natively, so this
# already works; proxy_wstunnel is enabled as defense for any older/edge config.
# Existing systems need the apache config recopied for the new ProxyPass line.
#
# The reload must be graceful, NOT a restart: this upgrade runs from a request
# apache is serving (the browser's upgrade window), so a restart would drop that
# connection and hang the upgrade.  A graceful reload re-reads the full config
# (loading the newly enabled proxy_wstunnel module) while letting in-flight
# requests finish, so the upgrade keeps streaming.

BINDIR=$(cd $(dirname $0) && pwd)
. ${BINDIR}/../../scripts/common

if [[ -f /etc/debian_version ]]; then
    a2enmod proxy_wstunnel

    # Recopy the site config so the new /fppdws ProxyPass takes effect.
    cat /opt/fpp/etc/apache2.site > /etc/apache2/sites-enabled/000-default.conf

    # Graceful reload (not restart) so the in-flight upgrade connection survives.
    gracefullyReloadApacheConf
fi
