#!/bin/bash

# logStage "<message>": emit a "===== <message> =====" section header. Mirrors
# the helper in scripts/common (which upgradeOS-part2.sh sources) and drives the
# streaming FPP OS Upgrade dialog's status line via ParseLastStageMarker() in
# www/js/fpp.js. Defined locally rather than sourcing scripts/common because this
# script is copied to and run from /home/fpp/media/tmp, where common's $0-based
# path detection would misresolve FPPDIR.
logStage() {
    echo "===== $1 ====="
}

logStage "Verifying image"

FPPOS=`/usr/bin/basename $1`
GITHUBSIZE=`curl -fsSL http://127.0.0.1/api/git/releases/sizes | grep ${FPPOS} | awk -F, '{print $2}'`
OURSIZE=`/usr/bin/stat -c %s $1`

FPPBOOTDIR=/boot
if [ -d "/boot/firmware" ]
then
    FPPBOOTDIR=/boot/firmware
fi

if ! [[ $GITHUBSIZE =~ ^-?[0-9]+$ ]];
then
  echo "Couldn't get fppos size from Github, attempting upgrade anyway"
else
  if [ "$OURSIZE" -lt "$GITHUBSIZE" ];
  then
    echo "Download size seems too small. Our size: $OURSIZE, Github size: $GITHUBSIZE deleting $1"
    echo "Please try to download the fppos again"
    rm $1
    exit 1;
  else
    echo "fppos size matches Github, continuing"
  fi
fi

mount $1 /mnt

ORIGTYPE=$(</etc/fpp/platform)
NEWTYPE=$(</mnt/etc/fpp/platform)

if [ "$ORIGTYPE" != "$NEWTYPE" ]; then
    echo "New image type '${NEWTYPE}' does not match existing '${ORIGTYPE}'"
    umount /mnt
    exit 1;
fi

# Architecture check: FPPPLATFORM doesn't distinguish 32-bit Pi from 64-bit
# Pi (both are "Raspberry Pi"), so applying a Pi64 .fppos on a Pi32 system
# (or vice versa) would brick the device. /etc/fpp/arch was added in FPP 10
# specifically for this guard. Only enforced when both sides have the marker.
if [ -f /etc/fpp/arch ] && [ -f /mnt/etc/fpp/arch ]; then
    ORIGARCH=$(</etc/fpp/arch)
    NEWARCH=$(</mnt/etc/fpp/arch)
    if [ "$ORIGARCH" != "$NEWARCH" ]; then
        echo "New image arch '${NEWARCH}' does not match existing '${ORIGARCH}'"
        umount /mnt
        exit 1;
    fi
fi

#make sure settings are re-applied after boot
echo "BootActions = \"settings\"" >> /home/fpp/media/settings

#remove files where the binary may not have changed (so rsync won't recopy)
#but the caps (getcap) might be different
rm -f /bin/ping

logStage "Preparing filesystems"
echo "Mounting filesystems for copy"
mount -o bind / /mnt/mnt
mount -o bind ${FPPBOOTDIR} /mnt/mnt${FPPBOOTDIR}
mount -t tmpfs tmpfs /mnt/tmp
mount -o bind /dev /mnt/dev
mount -o bind /proc /mnt/proc

if [ -f /home/fpp/media/tmp/keepOptFPP ]
then
    # If we are on master and keeping /opt/fpp, run the existing part2 script
    echo "keepOptFPP flag exists, script will not copy /opt/fpp from image."
    echo "Passing control to existing upgradeOS-part2.sh from /opt/fpp"
    stdbuf --output=0 --error=0 chroot /mnt /mnt/opt/fpp/SD/upgradeOS-part2.sh
elif [ -d "/boot/firmware" -a ! -d "/mnt/boot/firmware" ]
then
    # Downgrading from Raspbian 12 or higher to a pre-12 version without /boot/firmware
    echo "Downgrading to OS version without /boot/firmware."
    echo "Passing control to upgradeOS-part2.sh from current version."
    cp /opt/fpp/SD/upgradeOS-part2.sh /home/fpp/media/tmp/upgradeOS-part2.sh
    stdbuf --output=0 --error=0 chroot /mnt /mnt/home/fpp/media/tmp/upgradeOS-part2.sh
    rm /home/fpp/media/tmp/upgradeOS-part2.sh
else
    echo "Passing control to upgradeOS-part2.sh from fppos image"
    stdbuf --output=0 --error=0 chroot /mnt /opt/fpp/SD/upgradeOS-part2.sh
fi

echo "----------"
echo "Control returned from upgradeOS-part2.sh script, resuming upgradeOS-part1.sh"

logStage "Finalizing"
echo "Copy done, unmounting filesystems"
sync
umount /mnt/proc
umount /mnt/dev
umount /mnt/tmp
umount /mnt/mnt${FPPBOOTDIR}
umount /mnt/mnt

sync

echo "Please reboot if the system does not do so automatically"

exec 0>&- # close stdin
exec 1>&- # close stdout
exec 2>&- # close stderr
sleep 1
sync
