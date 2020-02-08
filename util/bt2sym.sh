#!/bin/bash

main () {
    [ -n "$1" ] && [ -z "$2" ] || usage
    local exe=$1
    if ! [ -e "$exe" ]; then
        echo "$(basename "$0"): no such file: $exe" >&2
        exit 1
    fi
    if ! [ -x "$exe" ]; then
        echo "$(basename "$0"): not executable: $exe" >&2
        exit 1
    fi
    local date time event args
    while read -r date time event args; do
        echo "$date $time $event $args"
        case "$event" in
            ASYNC-TIMER-BT)
                ;;
            *)
                continue
                ;;
        esac
        if read -r uid bt; then
            addr2line -e "$exe" $(sed 's/^BT=//' <<<"$bt" | tr '`' ' ') |
                sed 's/^/    /'
        fi <<<"$args"
    done
}

usage () {
    cat <<EOF >&2
Usage: $(basename "$0") executable

Reads ASYNC-TIMER-BT trace lines from the standard input.
EOF
    exit 1
}

main "$@"
