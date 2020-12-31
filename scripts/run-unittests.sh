#!/bin/bash

main () {
    cd "$(dirname "$(realpath "$0")")/.."
    local os=$(uname -s)
    if [ -n "$FSARCHS" ]; then
        local archs=()
        IFS=, read -ra archs <<< "$FSARCHS"
        for arch in "${archs[@]}" ; do
            run-tests "$arch"
        done
    elif [ "$os" = Linux ]; then
        local cpu=$(uname -m)
        if [ "$cpu" = x86_64 ]; then
            run-tests linux64
        elif [ "$cpu" = i686 ]; then
            run-tests linux32
        else
            echo "$0: Unknown CPU: $cpu" >&2
            exit 1
        fi
    elif [ "$os" = Darwin ]; then
        run-tests darwin
    else
        echo "$0: Unknown OS architecture: $os" >&2
        exit 1
    fi
}

realpath () {
    if [ -x "/bin/realpath" ]; then
        /bin/realpath "$@"
    else
        python -c "import os.path, sys; print os.path.realpath(sys.argv[1])" \
               "$1"
    fi
}

run-tests () {
    local arch=$1
    echo &&
    echo Start Tests on $arch &&
    echo &&
    rm -rf stage/$arch/test/gcov &&
    mkdir -p stage/$arch/test/gcov &&
    FSCCFLAGS="$FSCCFLAGS -fprofile-arcs -ftest-coverage -O0" \
    FSLINKFLAGS="-fprofile-arcs" \
        ${SCONS:-scons} builddir=test &&
    stage/$arch/test/test/fstracecheck &&
    stage/$arch/test/test/asynctest &&
    test-coverage $arch
}

test-coverage () {
    local arch=$1
    echo &&
    echo Test Coverage &&
    echo ============= &&
    echo &&
    find src -name \*.c |
    while read src; do
        ${GCOV:-gcov} -p -o "stage/$arch/test/$(dirname "$src")" "$src" || exit
    done >stage/$arch/test/gcov/gcov.out 2>stage/$arch/test/gcov/gcov.err &&
    (
        pretty-print-out <stage/$arch/test/gcov/gcov.out &&
        pretty-print-err <stage/$arch/test/gcov/gcov.err
    ) >stage/$arch/test/gcov/gcov.report &&
    mv *.gcov stage/$arch/test/gcov/ &&
    LANG=C sort -u stage/$arch/test/gcov/gcov.report &&
    echo
}

pretty-print-out () {
    while read line1; do
        read line2
        read line3
        read line4
        f=$(sed "s/^File .\\([^']*\\)'$/\\1/" <<<"$line1")
        if [[ "$f" =~ \.h$ ]]; then
            continue
        fi
        case "$line2" in
            "No executable lines")
                ;;
            "Lines executed:"*)
                p=$(sed 's/Lines executed:\([0-9.]*\)% .*$/\1/' <<<"$line2")
                printf "%6s%% %s\n" "$p" "$f"
                ;;
        esac
    done
}

pretty-print-err () {
    grep 'gcda:cannot open data file' |
    sed 's!^stage/[^/]*/test/\([^:]*\).gcda:cannot open data file.*!  0.00% \1.c!'
}

main
