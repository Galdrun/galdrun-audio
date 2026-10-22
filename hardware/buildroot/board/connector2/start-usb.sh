#!/bin/sh
set -e
/usr/sbin/galdrun-install --install
echo 'Rebooting into the installed firmware.'
sync
reboot -f
