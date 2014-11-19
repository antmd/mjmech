#!/bin/bash
# Last component must be named 'vserver'
TMPDIR=/tmp/odroid-assemble/vserver

if [[ "$1" != "start" && "$1" != "install" && "$1" != "assemble" && "$1" != "exec" ]]; then
    cat <<EOF
Usage: $0 command
Commands:
  start     -- upload server to odroid's RAM and start
  install   -- install server to odroid's flash
  assemble  -- do not upload, just put all files to $TMPDIR
  exec CMD  -- upload, then exec CMD in vserver's dir, with X11 forwarding. Example:
   exec sudo ./install-packages.sh
   exec legtool/legtool.py
   exec legtool/herkulex_tool.py -d /dev/ttyACM99 -b -a 2
EOF
    exit 1
fi

set -e
cd $(dirname $(readlink -f "$0"))
RELEASE=$(lsb_release -sc)
if [[ "$RELEASE" != "precise" ]]; then
    # The program cannot even be imported on precise
    ./vserver/vserver.py --check
fi

mkdir -p $TMPDIR
RSCMD=(rsync -a --exclude '*~' --exclude '*.pyc' --delete )

# Set ssh command used by rsync (we wil also call it directly later)
export RSYNC_RSH="ssh -o BatchMode=yes -o CheckHostIP=no -o HashKnownHosts=no "
# Store hostkey under a fixed name even if IP address is used, use local k_h
RSYNC_RSH="$RSYNC_RSH -o HostKeyAlias=odroid -o UserKnownHostsFile=known_hosts"

"${RSCMD[@]}" --exclude=legtool --exclude=install-packages.sh \
    --exclude=gbulb --exclude=real.cfg --exclude=vui_helpers.py \
    vserver $TMPDIR/..
"${RSCMD[@]}" ../legtool ../install-packages.sh ../gbulb ../real.cfg \
    vui_helpers.py $TMPDIR

if [[ "$1" == "assemble" ]]; then
    echo Assembled
    exit 0
fi

RHOST=odroid@odroid

echo Uploading
rsync -a --delete $TMPDIR $RHOST:/tmp/
if [[ "$1" == "start" ]]; then
    echo Starting vserver
    $RSYNC_RSH $RHOST -t exec /tmp/vserver/start.sh
elif [[ "$1" == "install" ]]; then
    echo Installing vserver to flash
    $RSYNC_RSH $RHOST -t rsync -av --delete /tmp/vserver/ /home/odroid/vserver/
elif [[ "$1" = "exec" ]]; then
    shift
    set -x
    $RSYNC_RSH $RHOST -Yt cd /tmp/vserver \&\& "$@"
fi
