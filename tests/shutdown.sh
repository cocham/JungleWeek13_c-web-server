#!/usr/bin/env bash
# Run in a disposable front container. Signals target only this script's children.
set -euo pipefail
logs=$(mktemp -d)
guestbook-backend 2>"$logs/backend" &
backend_pid=$!
guestbook --static-root "${GUESTBOOK_STATIC_ROOT:-/app/public}" 2>"$logs/front" &
front_pid=$!
cleanup() {
    for pid in "$front_pid" "$backend_pid"; do
        if [[ -n $pid ]]; then
            kill -TERM "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -f "$logs/front" "$logs/backend"
    rmdir "$logs"
}
trap cleanup EXIT
for service in front backend; do
    for ((i=0; i<100; i++)); do
        if grep -q 'with 4 workers' "$logs/$service"; then break; fi
        sleep .03
    done
    grep -q 'with 4 workers' "$logs/$service"
done

fd_count() {
    local -a fds=(/proc/"$1"/fd/[0-9]*)
    echo "${#fds[@]}"
}

front_base=$(fd_count "$front_pid")
backend_base=$(fd_count "$backend_pid")
for ((i=0; i<32; i++)); do
    for path in / /api/messages; do
        exec 3<>/dev/tcp/127.0.0.1/8080
        printf 'GET %s HTTP/1.1\r\nHost: localhost\r\n\r\n' "$path" >&3
        IFS= read -r -t 4 line <&3
        [[ $line == $'HTTP/1.1 200 OK\r' ]]
        timeout 4 cat <&3 >/dev/null
        exec 3>&-
    done
done
# The last worker can be between its final send() and close().
for ((i=0; i<50; i++)); do
    if [[ $(fd_count "$front_pid") == "$front_base" && $(fd_count "$backend_pid") == "$backend_base" ]]; then break; fi
    sleep .02
done
[[ $(fd_count "$front_pid") == "$front_base" && $(fd_count "$backend_pid") == "$backend_base" ]]
printf '64 GETs: front FD %s -> %s, backend FD %s -> %s\n' \
    "$front_base" "$(fd_count "$front_pid")" "$backend_base" "$(fd_count "$backend_pid")"

check_stop() {
    local service=$1 pid=$2 port=$3 signal=$4 baseline=$5 fd rc
    local -a held=()
    for ((i=0; i<8; i++)); do
        exec {fd}<>"/dev/tcp/127.0.0.1/$port"
        printf 'GET / HTTP/1.1\r\nHost: localhost\r\n' >&"$fd"
        held+=("$fd")
    done
    for ((i=0; i<50; i++)); do
        if [[ $(fd_count "$pid") -eq $((baseline + 8)) ]]; then break; fi
        sleep .01
    done
    [[ $(fd_count "$pid") -eq $((baseline + 8)) ]]
    kill -s "$signal" "$pid"
    # A stopped process disappears from /proc after the shell reaps it.
    for ((i=0; i<100; i++)); do
        if ! kill -0 "$pid" 2>/dev/null; then break; fi
        sleep .05
    done
    if kill -0 "$pid" 2>/dev/null; then
        printf '%s did not stop within 5 s\n' "$service" >&2
        return 1
    fi
    wait "$pid"
    for fd in "${held[@]}"; do
        rc=0
        timeout 1 cat <&"$fd" >/dev/null 2>/dev/null || rc=$?
        [[ $rc == 0 || $rc == 1 ]]  # EOF or reset when a queued request is discarded
        exec {fd}>&-
    done
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then return 1; fi
    local accepted closed
    accepted=$(grep -c "^$service client_fd=.* accepted\$" "$logs/$service")
    closed=$(grep -c "^$service client_fd=.* closing\$" "$logs/$service")
    [[ $accepted -eq $closed && $closed -ge 8 ]]
    printf '%s SIG%s: exit 0, 8 client sockets closed, listener closed, accepted/closing %s/%s\n' \
        "$service" "$signal" "$accepted" "$closed"
}

check_stop front "$front_pid" 8080 TERM "$front_base"
front_pid=''
check_stop backend "$backend_pid" 8001 INT "$backend_base"
backend_pid=''
