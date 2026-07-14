#!/usr/bin/bash
#
# Scan for available wireless networks on each wifi device and list them.
#
# We run BOTH scan tools for every adapter and show their results separately:
#   - `iw` (nl80211) is the modern path and the one FPP uses at runtime.
#   - `iwlist` (legacy WEXT) still works on some out-of-tree Realtek USB
#     drivers (e.g. RTL8812BU / RTL8822BU on 88x2bu) whose nl80211 scan is
#     broken and returns nothing.
# Showing them side by side is what tells a support person whether an adapter's
# nl80211 scan is the thing that's broken.
#
# Scan output is kept in shell variables (no temp files), so concurrent
# invocations can never interfere with each other.

# Parse raw `iw dev ... scan` output into numbered per-network lines.
parse_iw() {
    awk '
        function emit() {
            if (bss == "") return
            if (signal_dbm == "") pct = 0
            else {
                pct = int(2 * (signal_dbm + 100))
                if (pct < 0) pct = 0
                if (pct > 100) pct = 100
            }
            enc = secure ? "(secure)" : "(open)  "
            con = associated ? " - Connected" : ""
            freq_disp = (freq != "") ? sprintf("%.3f GHz", freq/1000) : ""
            printf("%5d : %s  %s %s %s (Signal strength: %d%%)%s\n",
                   ++n, ssid, freq_disp, bss, enc, pct, con)
        }
        /^BSS / {
            emit()
            bss=""; ssid=""; freq=""; signal_dbm=""; secure=0; associated=0
            if (match($0, /[0-9a-fA-F:]{17}/)) {
                bss = toupper(substr($0, RSTART, RLENGTH))
            }
            if ($0 ~ /associated/) associated = 1
        }
        /^\tfreq:/       { freq = $2 }
        /^\tsignal:/     { signal_dbm = $2 + 0 }
        /^\tSSID:/       { sub(/^\tSSID: ?/, ""); ssid = $0 }
        /^\tRSN:/        { secure = 1 }
        /^\tWPA:/        { secure = 1 }
        /capability:.*Privacy/ { secure = 1 }
        END { emit() }
    '
}

# Parse raw `iwlist ... scan` (WEXT) output into the same numbered line shape.
parse_iwlist() {
    awk '
        function emit() {
            if (bss == "") return
            enc = secure ? "(secure)" : "(open)  "
            printf("%5d : %s  %s %s %s (Signal strength: %d%%)\n",
                   ++n, ssid, freq_disp, bss, enc, pct)
        }
        /Cell [0-9]+ - Address:/ {
            emit()
            bss=""; ssid=""; freq_disp=""; pct=0; secure=0
            if (match($0, /[0-9a-fA-F:]{17}/)) {
                bss = toupper(substr($0, RSTART, RLENGTH))
            }
        }
        /ESSID:/ { if (match($0, /ESSID:"[^"]*"/)) ssid = substr($0, RSTART+7, RLENGTH-8) }
        /Frequency:[0-9.]+ GHz/ {
            if (match($0, /Frequency:[0-9.]+/))
                freq_disp = sprintf("%.3f GHz", substr($0, RSTART+10, RLENGTH-10))
        }
        /Signal level=.*dBm/ {
            if (match($0, /Signal level=-?[0-9]+/)) {
                dbm = substr($0, RSTART+13, RLENGTH-13) + 0
                pct = int(2 * (dbm + 100))
                if (pct < 0) pct = 0
                if (pct > 100) pct = 100
            }
        }
        /Encryption key:on/ { secure = 1 }
        /IE:.*WPA/          { secure = 1 }
        END { emit() }
    '
}

# Print one scan tool's section: its parsed network list, or the raw output if
# nothing parsed (so a scan error or empty result stays visible).
show() {
    local label="$1" marker="$2" formatter="$3" raw="$4"
    printf -- "----- %s -----\n" "$label"
    if grep -q "$marker" <<< "$raw"; then
        "$formatter" <<< "$raw"
    else
        printf "%s\n" "$raw"
    fi
    printf "\n"
}

while read -r wifi_device || [[ -n $wifi_device ]]; do
    [ -z "$wifi_device" ] && continue

    UPORDOWN=$(cat "/sys/class/net/$wifi_device/operstate" 2>/dev/null || echo down)
    ip link set "$wifi_device" up 2>/dev/null

    iw_scan=$(iw dev "$wifi_device" scan 2>&1)
    iwlist_scan=$(iwlist "$wifi_device" scan 2>&1)

    [ "$UPORDOWN" = "down" ] && ip link set "$wifi_device" down 2>/dev/null

    printf "Wifi Device: %s\n" "$wifi_device"
    show "iw dev $wifi_device scan (nl80211)" '^BSS ' parse_iw "$iw_scan"
    show "iwlist $wifi_device scan (WEXT)" 'Cell ' parse_iwlist "$iwlist_scan"
done < <(iw dev | awk '$1=="Interface"{print $2}')
