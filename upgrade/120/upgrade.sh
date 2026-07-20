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
# Existing systems need the apache config recopied for the new ProxyPass line;
# enabling a module requires a full apache restart, not just a reload.

BINDIR=$(cd $(dirname $0) && pwd)
. ${BINDIR}/../../scripts/common

if [[ -f /etc/debian_version ]]; then
    a2enmod proxy_wstunnel

    # Recopy the site config so the new /fppdws ProxyPass takes effect.
    cat /opt/fpp/etc/apache2.site > /etc/apache2/sites-enabled/000-default.conf

    # A newly-enabled module needs a restart; a graceful reload won't load it.
    if systemctl is-active --quiet apache2; then
        systemctl restart apache2
        echo "Enabled proxy_wstunnel and restarted apache for /fppdws."
    fi
fi
