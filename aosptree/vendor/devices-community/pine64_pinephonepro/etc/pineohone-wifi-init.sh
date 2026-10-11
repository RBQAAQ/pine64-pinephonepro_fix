#!/vendor/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Recover the Broadcom SDIO device when the firmware-vendor submodule was
# not autoloaded. Newer kernels with the integrated vendor code need no repair.

[ -d /sys/class/net/wlan0 ] && exit 0

for attempt in 1 2 3 4 5 6 7 8 9 10; do
    [ -d /sys/module/brcmfmac ] && break
    sleep 1
done
[ -d /sys/module/brcmfmac ] || exit 0

SUBMODULE=/vendor/lib/modules/kernel/drivers/net/wireless/broadcom/brcm80211/brcmfmac/wcc/brcmfmac-wcc.ko
if [ ! -d /sys/module/brcmfmac_wcc ] && [ -f "$SUBMODULE" ]; then
    insmod "$SUBMODULE" || exit 1
fi

if [ ! -d /sys/class/net/wlan0 ]; then
    DRIVER=/sys/bus/sdio/drivers/brcmfmac
    for DEVICE in mmc0:0001:1 mmc0:0001:2; do
        if [ -d "/sys/bus/sdio/devices/$DEVICE" ] && [ ! -L "$DRIVER/$DEVICE" ]; then
            printf '%s' "$DEVICE" > "$DRIVER/bind" || exit 1
        fi
    done
fi
exit 0
