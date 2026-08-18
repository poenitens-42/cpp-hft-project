cmake_minimum_required(VERSION 3.20)
project(HFT_Project VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for clangd / IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(HFT_COMPILE_OPTIONS
    -O3
    -march=native
    -fno-plt
    -fomit-frame-pointer
    -Wall
    -Wextra
    -Wpedantic
)

# ------------------------------------------------------------
# Dependencies — ASIO (header-only, no Boost)
# ------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-24-0
)
FetchContent_MakeAvailable(asio)

# ------------------------------------------------------------
# kdb+ C API static object — must exist locally, see README
# (third_party/kdb/c.o is NOT committed; download per-platform
#  from https://github.com/KxSystems/kdb/tree/master/c/c)
# ------------------------------------------------------------
set(KDB_C_OBJ ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kdb/c.o)
if(NOT EXISTS ${KDB_C_OBJ})
    message(FATAL_ERROR
        "Missing ${KDB_C_OBJ}. Download the platform c.o from "
        "https://github.com/KxSystems/kdb/tree/master/c/c and place it "
        "at third_party/kdb/c.o before configuring.")
endif()

# ------------------------------------------------------------
# Sources — explicit list, NOT GLOB
# GLOB anti-pattern: cmake won't re-run when you add a new .cpp,
# so the build silently misses the file until you re-cmake manually.
# ------------------------------------------------------------
set(HFT_SOURCES
    src/main.cpp
    # Add new translation units here explicitly:
    # src/order_book.cpp
    # src/market_data.cpp
     )

add_executable(hft_app ${HFT_SOURCES})
target_compile_options(hft_app PRIVATE ${HFT_COMPILE_OPTIONS})
target_include_directories(hft_app PRIVATE
    include
    ${asio_SOURCE_DIR}/asio/include
)
target_compile_definitions(hft_app PRIVATE
    ASIO_STANDALONE
    ASIO_HAS_CO_AWAIT
)

add_executable(bench_client src/benchmark/bench_client.cpp)
target_compile_options(bench_client PRIVATE ${HFT_COMPILE_OPTIONS})
target_include_directories(bench_client PRIVATE include ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(bench_client PRIVATE ASIO_STANDALONE ASIO_HAS_CO_AWAIT)

# NOTE: CMake's default Release build type injects -DNDEBUG, which
# compiles away every assert(). These three targets are ASSERTION-BASED
# tests, not benchmarks, so -UNDEBUG is mandatory here regardless of
# the overall build type — otherwise they "pass" by doing nothing.
add_executable(order_book_test
    src/tests/order_book_test.cpp
)
target_compile_options(order_book_test PRIVATE -UNDEBUG)
target_include_directories(order_book_test PRIVATE include)

add_executable(itch_messages_test
    src/tests/itch_messages_test.cpp
)
target_compile_options(itch_messages_test PRIVATE ${HFT_COMPILE_OPTIONS} -UNDEBUG)
target_include_directories(itch_messages_test PRIVATE include)

add_executable(itch_stats
    src/tools/itch_stats.cpp
)
target_compile_options(itch_stats PRIVATE ${HFT_COMPILE_OPTIONS})
target_include_directories(itch_stats PRIVATE include)

# ------------------------------------------------------------
# kdb+ smoke test — its OWN executable, separate from hft_app,i
# so it doesn't collide with main.cpp's main() and doesn't force
# every hft_app source to depend on k.h / KXVER.
# ------------------------------------------------------------
add_executable(kdb_test
    src/tests/kdb_test.cpp
    src/analytics/kdb_client.cpp
)
target_compile_options(kdb_test PRIVATE ${HFT_COMPILE_OPTIONS} -UNDEBUG)
target_include_directories(kdb_test PRIVATE
    include
    third_party/kdb
)
target_compile_definitions(kdb_test PRIVATE
    KXVER=3
)
target_link_libraries(kdb_test PRIVATE ${KDB_C_OBJ})
