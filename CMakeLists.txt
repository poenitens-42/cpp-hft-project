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

# ------------------------------------------------------------
# Compiler flags
# NOTE: -ffast-math is intentionally EXCLUDED. (because it can reorder Floating point ops and treats Nan/Inf values incorrectly)
#        so its an instant death in computing PnL values 
#
# -fno-plt:         avoids PLT indirection for external calls (faster)
# -fomit-frame-pointer: frees one register, acceptable in release
# -march=native:    enables AVX2/AVX-512 if your CPU supports it (better if it does) 
# ------------------------------------------------------------
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

target_include_directories(hft_app PRIVATE include                            # your own headers (server.hpp etc.)
	                        ${asio_SOURCE_DIR}/asio/include    # ASIO headers
						   )

# ASIO: standalone mode (no Boost), enable coroutines
target_compile_definitions(hft_app PRIVATE
    ASIO_STANDALONE
    ASIO_HAS_CO_AWAIT
)

add_executable(bench_client src/bench_client.cpp)
target_compile_options(bench_client PRIVATE ${HFT_COMPILE_OPTIONS})
target_include_directories(bench_client PRIVATE include ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(bench_client PRIVATE ASIO_STANDALONE ASIO_HAS_CO_AWAIT)

add_executable(order_book_test src/order_book_test.cpp)
target_include_directories(order_book_test PRIVATE include)
