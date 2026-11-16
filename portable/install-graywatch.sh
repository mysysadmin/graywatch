#!/bin/sh
# Install this bundle as a systemd service, or remove it again.
#
#   sudo ./install-graywatch.sh               install or upgrade, start
#   sudo ./install-graywatch.sh --no-capture  without automatic collections
#   sudo ./install-graywatch.sh --uninstall   remove, keep recordings
#   sudo ./install-graywatch.sh --purge       remove, including recordings,
#                                             settings and the service user
#
# Installs to /opt/graywatch. Settings: /etc/default/graywatch (kept on
# upgrade). Recording: /var/log/graywatch. Automatic collections:
# /var/lib/graywatch/captures.
set -eu

prefix=/opt/graywatch
units=/etc/systemd/system
settings=/etc/default/graywatch
action=install
capture=1

usage() {
    echo "usage: $0 [--no-capture | --uninstall | --purge]" >&2
    exit 2
}
while [ "$#" -gt 0 ]; do
    case "$1" in
        --no-capture) capture=0 ;;
        --uninstall) action=uninstall ;;
        --purge) action=purge ;;
        *) usage ;;
    esac
    shift
done

[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo $0)" >&2; exit 1; }
command -v systemctl >/dev/null 2>&1 || { echo "systemd is required" >&2; exit 1; }
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

remove() {
    systemctl disable --now graywatch-capture.path graywatchd.service >/dev/null 2>&1 || true
    systemctl stop graywatch-capture.service >/dev/null 2>&1 || true
    rm -f "$units/graywatchd.service" "$units/graywatch-capture.service" \
        "$units/graywatch-capture.path"
    rm -rf "$units/graywatchd.service.d"
    systemctl daemon-reload
    rm -rf "$prefix"
}

if [ "$action" != install ]; then
    remove
    if [ "$action" = purge ]; then
        rm -rf /var/log/graywatch /var/lib/graywatch "$settings"
        if getent passwd graywatch >/dev/null 2>&1; then userdel graywatch || true; fi
        if getent group graywatch >/dev/null 2>&1; then groupdel graywatch 2>/dev/null || true; fi
        echo "Graywatch removed, including recordings and settings."
    else
        echo "Graywatch removed. Kept: /var/log/graywatch, /var/lib/graywatch and $settings."
    fi
    exit 0
fi

[ -x "$here/bin/graywatchd" ] || { echo "run this from an unpacked Graywatch bundle" >&2; exit 1; }
[ "$here" != "$prefix" ] || { echo "run the installer from the downloaded bundle" >&2; exit 1; }

# The daemon runs unprivileged as its own system user.
if ! getent passwd graywatch >/dev/null 2>&1; then
    nologin=$(command -v nologin || echo /usr/sbin/nologin)
    useradd --system --user-group --no-create-home --home-dir / \
        --shell "$nologin" --comment "Graywatch daemon" graywatch
fi

# Copy only the program files, not archives a test run left in the bundle.
stage="$prefix.new"
rm -rf "$stage"
mkdir -p "$stage"
cp -R "$here/bin" "$here/packaging" "$here/licenses" "$stage/"
for file in collect-graywatch-support.sh install-graywatch.sh README.txt EBPF.txt LICENSE \
        THIRD_PARTY_NOTICES SHA256SUMS; do
    [ -e "$here/$file" ] && cp "$here/$file" "$stage/"
done
rm -rf "$prefix.old"
[ -d "$prefix" ] && mv "$prefix" "$prefix.old"
mv "$stage" "$prefix"
rm -rf "$prefix.old"
chmod -R go-w "$prefix"

if [ ! -f "$settings" ]; then
    cat > "$settings" <<'EOF'
# Graywatch service settings. After a change:
#   systemctl restart graywatchd
#
# How long graywatchd keeps every event it measures, in /var/log/graywatch:
#   30m  flight recorder: what happened in the minutes before a problem was
#        noticed, added to every collection (default)
#   7d   record all the time, for example while a problem comes and goes.
#        Compressed, about 50 MB a day on a node with ten disks, and several
#        hundred MB on a storage server with a hundred.
GRAYWATCH_KEEP=30m
# Upper limit for the recording; the oldest files are deleted first.
GRAYWATCH_MAX_SIZE=2G
# More graywatchd options, for example:
#   --ignore-ib-port mlx5_1/1     a port that is down on purpose
#   --capture-after 120s          how long a problem must last before an
#                                 automatic collection (default 60s)
#   --capture-cooldown 4h         at most one automatic collection per (1h)
GRAYWATCH_OPTIONS=
# Automatic collections in /var/lib/graywatch/captures: how many to keep.
GRAYWATCH_CAPTURE_KEEP=10
# More options for automatic collections, for example --duration 120
GRAYWATCH_CAPTURE_OPTIONS=
EOF
    chmod 0644 "$settings"
fi

for unit in graywatchd.service graywatch-capture.service; do
    sed -e "s|@GRAYWATCH_DAEMON@|$prefix/bin/graywatchd|g" \
        -e "s|@GRAYWATCH_COLLECTOR@|$prefix/collect-graywatch-support.sh|g" \
        -e "s|@GRAYWATCH_DOC@|$prefix/README.txt|g" \
        "$prefix/packaging/$unit.in" > "$units/$unit"
done
cp "$prefix/packaging/graywatch-capture.path" "$units/graywatch-capture.path"
# Lets the unprivileged daemon read FUSE request counts (read access only).
mkdir -p "$units/graywatchd.service.d"
cp "$prefix/packaging/graywatchd-fuse.conf" "$units/graywatchd.service.d/fuse.conf"
mkdir -p /var/lib/graywatch/captures
chmod 0750 /var/lib/graywatch /var/lib/graywatch/captures

systemctl daemon-reload
systemctl enable graywatchd.service >/dev/null 2>&1
systemctl restart graywatchd.service
if [ "$capture" -eq 1 ]; then
    systemctl enable graywatch-capture.path >/dev/null 2>&1
    systemctl restart graywatch-capture.path
else
    systemctl disable --now graywatch-capture.path >/dev/null 2>&1 || true
fi

attempt=0
while [ ! -S /run/graywatch/graywatchd.sock ] && [ "$attempt" -lt 50 ]; do
    attempt=$((attempt + 1))
    sleep 0.1
done
if [ ! -S /run/graywatch/graywatchd.sock ]; then
    echo "graywatchd did not start:" >&2
    systemctl status graywatchd.service --no-pager >&2 || true
    exit 1
fi

echo "Graywatch $(basename "$here" | sed 's/^graywatch-portable-//; s/-linux-.*//') is running."
echo "  Settings:    $settings"
echo "  Recording:   /var/log/graywatch (GRAYWATCH_KEEP, now $(. "$settings"; echo "${GRAYWATCH_KEEP:-30m}"))"
if [ "$capture" -eq 1 ]; then
    echo "  Automatic:   /var/lib/graywatch/captures"
else
    echo "  Automatic:   off"
fi
echo "  Collect now: sudo $prefix/collect-graywatch-support.sh"
