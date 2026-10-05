#!/bin/bash
# Halo VR on the Steam Frame: Lepton runs the APK in a container of its own,
# and installs it anew whenever the APK is newer than the installed one
# (port/android/STEAM_FRAME.md). The game's data: ~/Documents/HaloCE.
# (a container of its own, not the shortcut's id: with that, SteamVR's
# client in the container waited on xrCreateInstance)
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
APK="$HOME/Documents/HaloCE/HaloVR.apk"
LOG="$HOME/Documents/HaloCE/launcher.log"
# (written by the game when the player quits it: no start again)
QUIT="$HOME/Documents/HaloCE/quit_requested.txt"
rm -f "$QUIT"
CONTEXT="steamlaunch-$SteamAppId"

log() { echo "$(date '+%F %T') $*" >> "$LOG"; }
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
LOGCAT="$HOME/Documents/HaloCE/logcat.txt"
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
log "end"
