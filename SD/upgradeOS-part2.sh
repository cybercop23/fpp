#!/bin/bash

. /opt/fpp/scripts/common
. /opt/fpp/scripts/functions

cd /

logStage "Updating boot filesystem"
echo "Running rsync to update boot file system:"
case "${FPPPLATFORM}" in
    "Raspberry Pi"|"BeagleBone 64")
        # Both Trixie-era Pi and BB64 use /boot/firmware as the FAT boot
        # mount. The old system being upgraded may still be on the older
        # /boot layout, so detect and remount if needed.
        ROOTDEV=$(findmnt -T /mnt | tail -n 1 | awk '{print $2}')
        # Use findmnt to avoid the ambiguous `mount | grep /boot` (which
        # could match unrelated lines inside the chroot).
        OLD_BOOT_AT_ROOT=false
        if findmnt -n /mnt/boot/firmware >/dev/null 2>&1; then
            BOOTMOUNTDEV=$(findmnt -n -o SOURCE /mnt/boot/firmware)
        elif findmnt -n /mnt/boot >/dev/null 2>&1; then
            BOOTMOUNTDEV=$(findmnt -n -o SOURCE /mnt/boot)
            OLD_BOOT_AT_ROOT=true
        else
            echo "ERROR: could not locate the old boot partition at /mnt/boot or /mnt/boot/firmware"
            exit 1
        fi

        if $OLD_BOOT_AT_ROOT; then
            echo "Remounting old boot partition from /mnt/boot to /mnt/boot/firmware"
            umount /mnt/boot
            mkdir -p /mnt/boot/firmware
            mount "${BOOTMOUNTDEV}" /mnt/boot/firmware
        fi

        rsync --outbuf=N -aAXxvc /boot/ /mnt/boot/ --delete-before

        if $OLD_BOOT_AT_ROOT; then
            echo "Adjusting /etc/fstab: /boot -> /boot/firmware"
            sed -e "s# /boot # /boot/firmware #" -i /mnt/etc/fstab
        fi

        # Pi uses cmdline.txt; BB64 uses extlinux.conf (updated later).
        if [ "${FPPPLATFORM}" = "Raspberry Pi" ]; then
            sed -i "s|root=/dev/[a-zA-Z0-9]* |root=${ROOTDEV} |g" /mnt/boot/firmware/cmdline.txt
        fi
        ;;
    *)
        # BeagleBone Black (still /boot, not /boot/firmware)
        rsync -aAXxvc /boot/ /mnt/boot/ --delete-during
        ;;
esac

echo

if [ "${FPPPLATFORM}" = "BeagleBone Black" ]; then
    # need to install the bootloader that goes with this version of the os
    echo "Updating Beagle boot loader:"
    /opt/fpp/bin.bbb/bootloader/install.sh
    echo
fi

# temporarily copy the ssh keys
echo "Saving system ssh keys"
mkdir tmp/ssh
cp -a mnt/etc/ssh/*key* tmp/ssh
echo

echo "Saving hostname"
mkdir tmp/etc
cp -a mnt/etc/hostname tmp/etc

echo "Saving machine-id"
cp -a mnt/etc/machine-id tmp/etc


#remove some files that rsync won't copy over as they have the same timestamp and size, but are actually different
#possibly due to ACL's or xtended attributes
echo "Force cleaning files which do not sync properly"
# Detect multiarch triplet (arm-linux-gnueabihf, aarch64-linux-gnu, etc.)
# so this script works on both 32-bit and 64-bit Pi and on BB64.
TRIPLE=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || gcc -dumpmachine 2>/dev/null)
rm -f mnt/bin/ping
rm -f mnt/lib/${TRIPLE}/librtmp.so.1
rm -f mnt/usr/bin/dc mnt/usr/bin/bc mnt/usr/bin/hardlink mnt/usr/bin/lua5*

SKIPFPP=""
if [ -f /mnt/home/fpp/media/tmp/keepOptFPP ]
then
    echo "Preserving existing /opt/fpp and ccache (cache dir + XDG config)"
    # ccache 4.12+ uses XDG paths (~/.cache/ccache, ~/.config/ccache);
    # older versions used ~/.ccache. Exclude all three so either layout
    # is preserved across the OS upgrade.
    SKIPFPP="--exclude=opt/fpp --exclude=root/.ccache --exclude=root/.cache/ccache --exclude=root/.config/ccache"
fi

#if kiosk was installed, save that state so after reboot, it can be re-installed
KIOSKMODE=$(getSetting Kiosk)
if [ "${KIOSKMODE}" = "1" ]; then
    touch /tmp/kiosk
fi

#copy everything other than fstab and the persistent net names
logStage "Updating root filesystem (longest step, please wait)"
echo "Running rsync to update / (root) file system:"
stdbuf --output=L --error=L rsync --outbuf=N -aAXxv bin etc lib opt root sbin usr var /mnt --delete-during --exclude=var/lib/php/sessions --exclude=etc/fstab --exclude=etc/systemd/network/10-*.network --exclude=etc/systemd/network/*-fpp-* --exclude=root/.ssh ${SKIPFPP}
echo

# force copy a few libs that rsync won't replace cleanly (same size/mtime
# but different content via xattrs/ACLs). Use globs so we don't hard-code
# package versions that drift with each OS release, and TRIPLE so this
# works on both armhf and aarch64.
force_copy_libs() {
    local pattern
    for pattern in "$@"; do
        local src
        for src in usr/lib/${TRIPLE}/${pattern}; do
            [ -e "$src" ] || continue
            cp -af "$src" "mnt/$src"
        done
    done
}
force_copy_libs 'libzip.so.*' 'libfribidi.so.*' 'libbrotlicommon.so.*'

echo "Adjusting fstab"
sed -i 's|tmpfs\s*/tmp\s*tmpfs.*||g' mnt/etc/fstab
sed -i 's|tmpfs\s*/var/tmp\s*tmpfs.*||g' mnt/etc/fstab

#restore the ssh keys
echo "Restoring system ssh keys"
cp -a tmp/ssh/* mnt/etc/ssh
echo

echo "Restoring hostname"
cp -af tmp/etc/hostname mnt/etc/hostname
rm -f  tmp/etc/hostname
echo 

echo "Restoring machine-id"
cp -af tmp/etc/machine-id mnt/etc/machine-id
rm -f  tmp/etc/machine-id
echo


#create a file in root to mark it as requiring kiosk mode to be installed, will be checked on reboot
if [ -f tmp/kiosk ]; then
    touch mnt/fpp_kiosk
fi

# make sure the stuff in fpp's home directory has correct owner/group
echo "Resetting ownership of files in /home/fpp"
chown -R fpp:fpp mnt/home/fpp

# create a file in root to detect that we just did an FPPOS Upgrade
touch mnt/fppos_upgraded

if [ -f mnt/etc/ssh/ssh_host_dsa_key -a -f mnt/etc/ssh/ssh_host_dsa_key.pub -a -f mnt/etc/ssh/ssh_host_ecdsa_key -a -f mnt/etc/ssh/ssh_host_ecdsa_key.pub -a -f mnt/etc/ssh/ssh_host_ed25519_key -a -f mnt/etc/ssh/ssh_host_ed25519_key.pub -a -f mnt/etc/ssh/ssh_host_rsa_key -a -f mnt/etc/ssh/ssh_host_rsa_key.pub ]
then
    echo "Found all SSH key files, disabling first-boot SSH key regeneration"
    rm mnt/etc/systemd/system/multi-user.target.wants/regenerate_ssh_host_keys.service
fi

if [ "${FPPPLATFORM}" = "Raspberry Pi" ]; then
    logStage "Updating boot loader"
    echo "Updating Raspberry Pi boot loader:"
    /bin/rpi-eeprom-update -a
fi

if [ "${FPPPLATFORM}" = "BeagleBone 64" ]; then
    if [ -f "/dev/mmcblk0p1" ]; then
        # need to install the bootloader to the eMMC that goes with this version of the os
        logStage "Updating boot loader"
        echo "Updating Beagle boot loader:"
        /opt/u-boot/bb-u-boot-pocketbeagle2/install-emmc.sh
        echo
    fi
    NEWROOT=$(findmnt -n -o SOURCE -f /mnt)
    DEVICE="${NEWROOT%p?}"
    sed -i "s|root=/dev/[a-zA-Z0-9]*\([0-9]\) |root=${DEVICE}p3 |g" /mnt/boot/firmware/extlinux/extlinux.conf
    sed -i "s|resume=/dev/[a-zA-Z0-9]*\([0-9]\) |resume=${DEVICE}p2 |g" /mnt/boot/firmware/extlinux/extlinux.conf
fi

echo "Running sync command to flush data"
sync

echo "Sync complete"
sleep 3

exit
