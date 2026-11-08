#!/bin/bash
# Mjolnir VR on the Steam Frame: Lepton runs the APK in a container of its own,
# and installs it anew whenever the APK is newer than the installed one
# (port/android/STEAM_FRAME.md). The game's data: ~/Documents/RingfallMjolnirVR.
# (a container of its own, not the shortcut's id: with that, SteamVR's
# client in the container waited on xrCreateInstance)
DATA="$HOME/Documents/RingfallMjolnirVR"
APK="$DATA/RingfallMjolnirVR.apk"
LOG="$DATA/launcher.log"
# (written by the game when the player quits it: no start again)
QUIT="$DATA/quit_requested.txt"
log() { echo "$(date '+%F %T') $*" >> "$LOG"; }

# Updates (update.auto in config.toml; the game's own updater, the Quest's,
# does nothing here: Lepton installs the APK in this folder each start).
# While the game runs, the latest release is looked for; a newer one's
# bundle is fetched into update/, checked, and put in place as the next
# start begins, before Lepton runs the APK (it installs the newer one).
# A build that knows no number of its own (build.txt: a local one) never
# looks. Nothing changes without the internet, or when anything fails;
# launcher.log says what the look found.
# The fetch runs as a systemd user unit of its own, outside Steam's reaper:
# it goes on when the game ends (a short game would end it, and the next
# start begin again), and Steam does not take the game for still running.
# update.lock keeps a start from putting a bundle in place, or fetching one
# more, while a fetch is under way. (Without systemd-run, the fetch runs
# here, and is given up when the game ends.)
REPOSITORY="@REPOSITORY@"
UPDATE="$DATA/update"
UPDATE_LOCK="$DATA/update.lock"
BUILD_FILE="$DATA/build.txt"
SELF="$DATA/ringfall-mjolnir-vr.sh"
updater=0
updated=0

update_apply()
{
    local new="$UPDATE/bundle" build
    exec 8>"$UPDATE_LOCK"
    if command -v flock >/dev/null && ! flock -n 8; then
        log "update: a fetch is still under way, its build is put in place at a later start"
        exec 8>&-
        return 0
    fi
    if [ -f "$UPDATE/ready" ]; then
        build=$(cat "$new/build.txt" 2>/dev/null)
        # (the launcher renamed over this one: bash goes on reading its own copy)
        if cp "$new/ringfall-mjolnir-vr.sh" "$SELF.tmp" && chmod 755 "$SELF.tmp" &&
            cp "$new/RingfallMjolnirVR.apk" "$APK.tmp" && chmod 666 "$APK.tmp" &&
            mv -f "$SELF.tmp" "$SELF" && mv -f "$APK.tmp" "$APK" && touch "$APK" &&
            cp "$new/build.txt" "$BUILD_FILE"
        then
            log "update: build $build put in place"
            updated=1
        else
            log "update: build $build could not be put in place"
            rm -f "$SELF.tmp" "$APK.tmp"
        fi
    fi
    rm -rf "$UPDATE"
    exec 8>&-
}

update_wanted()
{
    # (update.auto = false in config.toml's [update]: never)
    ! awk '/^\[/ { section = $0 } section == "[update]" && /^[ \t]*auto[ \t]*=/ { print $3 }' \
        "$DATA/config.toml" 2>/dev/null | grep -q false
}

update_fetch()
{
    local installed latest answer error
    exec 9>"$UPDATE_LOCK"
    # (another fetch under way: that one goes on)
    command -v flock >/dev/null && { flock -n 9 || return 0; }
    installed=$(cat "$BUILD_FILE" 2>/dev/null)
    [[ "$installed" =~ ^[0-9]+$ ]] && (( installed > 0 )) || return 0
    if ! update_wanted; then
        log "update: off (update.auto = false in config.toml)"
        return 0
    fi
    if ! answer=$(curl -fsSL --max-time 20 -H "Accept: application/vnd.github+json" \
            "https://api.github.com/repos/$REPOSITORY/releases/latest" 2>&1); then
        log "update: no answer from GitHub (${answer:-no message})"
        return 0
    fi
    latest=$(printf '%s' "$answer" |
        python3 -I -c 'import json, sys; print(json.load(sys.stdin)["tag_name"].rpartition("-")[2])' 2>/dev/null)
    if ! [[ "$latest" =~ ^[0-9]+$ ]]; then
        log "update: GitHub's answer names no build"
        return 0
    fi
    if (( latest <= installed )); then
        log "update: none, build $installed is the latest"
        return 0
    fi
    log "update: build $latest found (this is $installed), fetching it"
    rm -rf "$UPDATE" && mkdir -p "$UPDATE" || return 0
    if error=$(curl -fsSL --retry 2 --max-time 900 -o "$UPDATE/bundle.zip" \
            "https://github.com/$REPOSITORY/releases/download/build-$latest/mjolnir-vr-steam-frame.zip" 2>&1) &&
        error="the bundle does not unpack" &&
        python3 -I -m zipfile -e "$UPDATE/bundle.zip" "$UPDATE/bundle" &&
        error="the bundle is not build $latest's" &&
        [ "$(cat "$UPDATE/bundle/build.txt" 2>/dev/null)" = "$latest" ] &&
        [ -s "$UPDATE/bundle/RingfallMjolnirVR.apk" ] && [ -s "$UPDATE/bundle/ringfall-mjolnir-vr.sh" ]
    then
        rm -f "$UPDATE/bundle.zip"
        touch "$UPDATE/ready"
        log "update: build $latest fetched, put in place at the next start"
    else
        log "update: build $latest could not be fetched (${error:-no message})"
        rm -rf "$UPDATE"
    fi
}

update_start()
{
    # (a start that has just put a build in place does not look: that one
    # is the latest, and its launcher, not this one, is what $0 names now)
    (( updated )) && return 0
    # (systemd's own unit, not a child of this script; --collect drops it,
    # failed too, once it ends)
    if command -v systemd-run >/dev/null &&
        systemd-run --user --quiet --collect --unit=mjolnir-vr-update \
            /bin/bash "$(readlink -f "$0")" --fetch-update </dev/null >/dev/null 2>&1
    then
        return 0
    fi
    update_fetch </dev/null >/dev/null 2>&1 &
    updater=$!
}

# (the fetch alone, as update_start runs it)
if [ "${1-}" = --fetch-update ]; then
    update_fetch </dev/null >/dev/null 2>&1
    exit 0
fi

export SteamAppId="${HALO_LEPTON_APP_ID:-4290000001}"
# (what Steam gives an Android title of its own, and a non-Steam game not:
# the app's data in the container links to the compat data path's
# internal/, a dangling link without it; and the shader paths: Lepton reads
# the dump path against the other)
export STEAM_COMPAT_DATA_PATH="$HOME/.local/share/Steam/steamapps/compatdata/$SteamAppId"
export STEAM_COMPAT_SHADER_PATH="$HOME/.local/share/Steam/steamapps/shadercache/$SteamAppId"
mkdir -p "$STEAM_COMPAT_DATA_PATH/internal" "$STEAM_COMPAT_SHADER_PATH"
unset STEAM_FOSSILIZE_DUMP_PATH LD_PRELOAD
LEPTON="$HOME/.local/share/Steam/steamapps/common/Lepton/lepton"
rm -f "$QUIT"
CONTEXT="steamlaunch-$SteamAppId"

log "start (pid $$, parent $PPID)"

# Steam's "Exit game" ends the game's processes as Steam knows them (this
# script, under Steam's reaper), not the container, which podman runs: a
# signal here is passed on to Lepton, and a watcher of its own takes the
# container down once this script is gone (killed)
stopping=0
child=0
stop()
{
    log "signal $1"
    stopping=1
    [ "$child" -gt 0 ] && kill -TERM "$child" 2>/dev/null
}
trap 'stop TERM' TERM
trap 'stop INT' INT
trap 'stop HUP' HUP

script=$$
setsid bash -c '
    while kill -0 '"$script"' 2>/dev/null; do sleep 1; done
    echo "$(date "+%F %T") watcher: the game was ended, so its container" >> "'"$LOG"'"
    "'"$LEPTON"'" kill "'"$CONTEXT"'" >/dev/null 2>&1
' </dev/null >/dev/null 2>&1 &

# the game's log while it runs (Lepton's logcat of its container), for
# looking into problems: logcat.txt, anew each start
LOGCAT="$DATA/logcat.txt"
: > "$LOGCAT"
setsid bash -c '
    while kill -0 '"$script"' 2>/dev/null; do
        "'"$LEPTON"'" logcat "'"$CONTEXT"'" 2>/dev/null | grep -v "^ERROR: \|^steamlaunch-\|^$" >> "'"$LOGCAT"'"
        sleep 3
    done
' </dev/null >/dev/null 2>&1 &

run()
{
    "$LEPTON" waitforexitandrun -- "$APK" &
    child=$!
    # (wait returns early when a signal comes: until Lepton is gone)
    while kill -0 "$child" 2>/dev/null; do
        wait "$child"
    done
    child=0
}

update_apply
update_start

# (a new APK is only installed on its first start, not started; and a start
# right after a game that was ended hard can fail in SteamVR, which then
# needs a moment: again, up to three times, while it ends within 30 s)
for attempt in 1 2 3; do
    started=$SECONDS
    run
    if (( stopping || SECONDS - started >= 30 )) || [ -e "$QUIT" ]; then
        break
    fi
    log "ended within $((SECONDS - started)) s: again"
    sleep 5
done
rm -f "$QUIT"
# (a fetch here, without systemd-run, still under way is given up: the next
# start looks again)
if (( updater > 0 )); then
    pkill -P "$updater" 2>/dev/null
    kill "$updater" 2>/dev/null
fi
log "end"
