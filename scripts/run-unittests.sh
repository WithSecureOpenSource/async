#!/usr/bin/env bash

main () {
    cd "$(dirname "$(realpath "$0")")/.."
    if [ -n "$FSARCHS" ]; then
        local archs=()
        IFS=, read -ra archs <<< "$FSARCHS"
        for arch in "${archs[@]}" ; do
            run-tests "$arch" "$@"
        done
    else
        local os=$(uname -m -s)
        case $os in
            "Darwin arm64")
                run-tests darwin "$@";;
            "Darwin x86_64")
                run-tests darwin "$@";;
            "FreeBSD amd64")
                run-tests freebsd_amd64 "$@";;
            "Linux i686")
                run-tests linux32 "$@";;
            "Linux x86_64")
                run-tests linux64 "$@";;
            "Linux aarch64")
                run-tests linux64_arm64 "$@";;
            "OpenBSD amd64")
                run-tests openbsd_amd64 "$@";;
            *)
                echo "$0: Unknown OS architecture: $os" >&2
                exit 1
        esac
    fi
}

realpath () {
    if [ -x /bin/realpath ]; then
        /bin/realpath "$@"
    else
        python -c "import os.path, sys; print(os.path.realpath(sys.argv[1]))" \
               "$1"
    fi
}

run-tests () {
    local arch=$1
    shift &&
    echo &&
    echo Start Tests on $arch &&
    echo &&
    if [ "$arch" = openbsd_amd64 ]; then
        stage/$arch/build/test/fstracecheck &&
        stage/$arch/build/test/asynctest
        return
    fi
    # The generated .gcda and .gcno files are not rewritten on
    # rebuild, which leads to errors and/or bad stats. I don't know a
    # better way around the problem but to get rid of the whole target
    # directory each time:
    rm -rf stage/$arch/test &&
    mkdir -p stage/$arch/test/gcov &&
    local fix_gcc_O0_warning_bug=-Wno-maybe-uninitialized &&
    local test_flags="-fprofile-arcs -ftest-coverage" &&
    if ! FSCCFLAGS="$fix_gcc_O0_warning_bug $FSCCFLAGS $test_flags -O0" \
         FSLINKFLAGS="-fprofile-arcs" \
         ${SCONS:-scons} builddir=test "$@"; then
        echo "Did you forget to specify prefix=<prefix> to $0?" >&2
        false
    fi &&
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

main "$@"
