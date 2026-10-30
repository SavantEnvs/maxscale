#!/usr/bin/env bash
#
# mayhem/build.sh — builds MaxScale's core (libmaxscale-common + the pp_sqlite parser plugin) twice:
#   build/fuzz  sanitized + coverage-instrumented, linked into the libFuzzer harnesses
#   build/test  upstream's normal flags, used by mayhem/test.sh (oracle + upstream test_pp_sqlite)
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
export MAYHEM_COMPAT_DIR="$SRC/build/compat"

# mariadb-connector-c is a git submodule that `git clone` leaves empty; the pinned revision
# (77bdf5a5725e) is vendored under mayhem/vendor so the build never needs the network.
if [ ! -f mariadb-connector-c/CMakeLists.txt ]; then
  mkdir -p mariadb-connector-c
  cp -r mayhem/vendor/mariadb-connector-c/. mariadb-connector-c/
fi

# Only the core + parser plugin are built; every optional module is off.
CMAKE_COMMON=(-G Ninja -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX"
  -DCMAKE_CXX_COMPILER_LAUNCHER="$SRC/mayhem/cxx-launcher.sh"
  -DFORCE_BUNDLE=OFF -DWITH_LTO=OFF -DBUILD_TESTS=Y -DBUILD_GUI=N -DBUILD_MAXCTRL=N -DBUILD_BINLOG=N
  -DBUILD_CDC=N -DBUILD_KAFKACDC=N -DBUILD_KAFKAIMPORTER=N -DBUILD_KMIP_KEY_MANAGER=N -DBUILD_MIRROR=N
  -DBUILD_NOSQL=N -DBUILD_STORAGE_MEMCACHED=N -DBUILD_STORAGE_REDIS=N -DBUILD_VAULT_KEY_MANAGER=N
  -DBUILD_TOOLS=N -DBUILD_LDI=N -DBUILD_GSSAPI=N -DBUILD_FILTERS=N -DWITH_SCRIPTS=N
  -DWITH_MAXSCALE_CNF=N)
# clang rejects a few GCC-isms in upstream's C++ (initialized VLA, unqualified ::nullptr_t) and upstream
# builds with -Werror, so warnings are silenced and compat.hh supplies ::nullptr_t.
WARN_FLAGS="-w -include $SRC/mayhem/compat.hh"

# MaxScale calls a module's entry point through a cast `void*(*)()` (server/core/load_utils.cc), which
# UBSan's `function` check halts on for every plugin load; that one check is relaxed, the rest stays on.
SAN_RELAX="-fno-sanitize=function"

# A cache configured with another compiler path (e.g. a ccache shim vs /usr/bin) would make CMake discard
# it and reconfigure from scratch, dropping the -D options above; start such a tree over instead.
reset_if_compiler_changed() {
  local cache="$1/CMakeCache.txt" want
  want="$(command -v "$CXX")"
  if [ -f "$cache" ] && ! grep -qx "CMAKE_CXX_COMPILER:[A-Z]*=$want" "$cache"; then
    rm -rf "$1"
  fi
}
reset_if_compiler_changed build/fuzz
reset_if_compiler_changed build/test

# 1) sanitized build for the fuzzers
cmake -S . -B build/fuzz "${CMAKE_COMMON[@]}" -DCMAKE_BUILD_TYPE=None \
  -DCMAKE_C_FLAGS="-O1 $SANITIZER_FLAGS $SAN_RELAX $DEBUG_FLAGS -fsanitize=fuzzer-no-link -w" \
  -DCMAKE_CXX_FLAGS="-O1 $SANITIZER_FLAGS $SAN_RELAX $DEBUG_FLAGS -fsanitize=fuzzer-no-link $WARN_FLAGS"
ninja -C build/fuzz connector-c
ninja -C build/fuzz -j"$MAYHEM_JOBS" maxscale-common pp_sqlite

# 2) harnesses (libFuzzer + standalone reproducer)
F="$SRC/build/fuzz"
INCS=(-I"$F/connector-c/install/include/mariadb" -I"$SRC/jwt-cpp/include" -I"$SRC/include"
  -I"$SRC/server/modules/include" -I"$F/include" -I"$SRC/maxutils/maxbase/include"
  -I"$SRC/maxutils/maxsql/include" -I"$SRC/maxutils/maxsimd/include")
DEFS=(-DHAVE_GLIBC=1 -DHAVE_SYSTEMD=1 -DHAVE_TGKILL=1 -DOPENSSL_1_1 -D_GNU_SOURCE=1)
STATIC_LIBS=("$F/maxutils/maxsql/src/libmaxsql.a" "$F/maxutils/maxsimd/src/libmaxsimd.a"
  "$F/maxutils/maxsimd/src/impl/libsimdimpl.a" "$F/maxutils/maxbase/src/libmaxbase.a")
SYS_LIBS=(-lodbc -lsystemd -lcurl -lpam -lzstd "$F/connector-c/install/lib/mariadb/libmariadbclient.a"
  -lpcre2-8 -ljansson -lssl -lpthread -lcrypt -lcrypto -lz -lrt -lm -lsqlite3 -lstdc++ -lmicrohttpd
  -lgnutls -latomic -luuid -ldl)
COMMON_LIB="$F/server/core/libmaxscale-common.so"

$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$F/standalone_main.o"
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -std=gnu++17 -c "$SRC/mayhem/lsan_off.cc" -o "$F/lsan_off.o"

build_harness() {
  local name="$1" src="$2"; shift 2
  local libs=("$@")
  $CXX -O1 $SANITIZER_FLAGS $SAN_RELAX $DEBUG_FLAGS -std=gnu++17 -w -include "$SRC/mayhem/compat.hh" \
    "${DEFS[@]}" "${INCS[@]}" -c "$src" -o "$F/$name.o"
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$F/$name.o" "$F/lsan_off.o" \
    -o "$SRC/$name" -rdynamic -Wl,-rpath,"$F/server/core" "${libs[@]}"
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS "$F/$name.o" "$F/lsan_off.o" "$F/standalone_main.o" \
    -o "$SRC/$name-standalone" -rdynamic -Wl,-rpath,"$F/server/core" "${libs[@]}"
}

build_harness fuzz_parser "$SRC/mayhem/harnesses/fuzz_parser.cc" "$COMMON_LIB" "${STATIC_LIBS[@]}" "${SYS_LIBS[@]}"
build_harness fuzz_canonical "$SRC/mayhem/harnesses/fuzz_canonical.cc" "${STATIC_LIBS[@]}" "${SYS_LIBS[@]}"
build_harness fuzz_ini "$SRC/mayhem/harnesses/fuzz_ini.cc" "${STATIC_LIBS[@]}" "${SYS_LIBS[@]}"

# 3) test build with upstream's normal flags (no sanitizer, no -gdwarf-3)
cmake -S . -B build/test "${CMAKE_COMMON[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS="-w $COVERAGE_FLAGS" -DCMAKE_CXX_FLAGS="$WARN_FLAGS $COVERAGE_FLAGS"
ninja -C build/test connector-c
ninja -C build/test -j"$MAYHEM_JOBS" maxscale-common pp_sqlite test_pp_sqlite

T="$SRC/build/test"
TINCS=("${INCS[@]/$F/$T}")
$CXX -O1 -std=gnu++17 -w -include "$SRC/mayhem/compat.hh" $COVERAGE_FLAGS "${DEFS[@]}" "${TINCS[@]}" \
  "$SRC/mayhem/oracle.cc" -o "$T/oracle" -rdynamic -Wl,-rpath,"$T/server/core" \
  "$T/server/core/libmaxscale-common.so" "$T/maxutils/maxsql/src/libmaxsql.a" \
  "$T/maxutils/maxsimd/src/libmaxsimd.a" "$T/maxutils/maxsimd/src/impl/libsimdimpl.a" \
  "$T/maxutils/maxbase/src/libmaxbase.a" "${SYS_LIBS[@]/$F/$T}"
