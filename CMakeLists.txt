cmake_minimum_required(VERSION 3.16)
project(komodo-telegram-alerter VERSION 2.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SANITIZER "none" CACHE STRING "Sanitizer to enable: none, asan, tsan")
set_property(CACHE SANITIZER PROPERTY STRINGS none asan tsan)

add_library(alerter_sanitizers INTERFACE)
if(SANITIZER STREQUAL "asan")
    target_compile_options(alerter_sanitizers INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-optimize-sibling-calls -g
    )
    target_link_options(alerter_sanitizers INTERFACE
        -fsanitize=address,undefined
    )
elseif(SANITIZER STREQUAL "tsan")
    target_compile_options(alerter_sanitizers INTERFACE
        -fsanitize=thread -fno-omit-frame-pointer -g
    )
    target_link_options(alerter_sanitizers INTERFACE
        -fsanitize=thread
    )
elseif(NOT SANITIZER STREQUAL "none")
    message(FATAL_ERROR "Unsupported SANITIZER='${SANITIZER}'. Expected: none, asan, or tsan")
endif()

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

include(CheckCSourceCompiles)
set(_ALERTER_ATOMIC_TEST_SRC "
#include <stdatomic.h>
int main(void) {
    atomic_bool b = ATOMIC_VAR_INIT(0);
    _Atomic(void *) p = (void *)0;
    atomic_store(&b, 1);
    atomic_store(&p, (void *)1);
    return (int)atomic_load(&b) + (atomic_load(&p) != (void *)0 ? 0 : 1);
}
")
check_c_source_compiles("${_ALERTER_ATOMIC_TEST_SRC}" ALERTER_HAVE_BUILTIN_ATOMICS)
if(NOT ALERTER_HAVE_BUILTIN_ATOMICS)
    set(CMAKE_REQUIRED_LIBRARIES atomic)
    check_c_source_compiles("${_ALERTER_ATOMIC_TEST_SRC}" ALERTER_HAVE_LIBATOMIC)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(ALERTER_HAVE_LIBATOMIC)
        set(ALERTER_ATOMIC_LIB atomic)
    endif()
endif()

find_package(json-c CONFIG QUIET)
if(NOT TARGET json-c::json-c)
    find_path(JSON_C_INCLUDE_DIR
        NAMES json.h json-c/json.h
        HINTS /opt/homebrew/include /usr/local/include /usr/include
        PATH_SUFFIXES json-c
    )
    find_library(JSON_C_LIBRARY
        NAMES json-c
        HINTS /opt/homebrew/lib /usr/local/lib /usr/lib
    )
    if(NOT JSON_C_INCLUDE_DIR OR NOT JSON_C_LIBRARY)
        message(FATAL_ERROR "json-c library or headers not found")
    endif()
    add_library(json-c::json-c UNKNOWN IMPORTED)
    set_target_properties(json-c::json-c PROPERTIES
        IMPORTED_LOCATION "${JSON_C_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${JSON_C_INCLUDE_DIR}"
    )
endif()

find_package(llhttp CONFIG QUIET)
if(TARGET llhttp::llhttp_shared)
    set(LLHTTP_TARGET llhttp::llhttp_shared)
elseif(TARGET llhttp::llhttp_static)
    set(LLHTTP_TARGET llhttp::llhttp_static)
else()
    find_path(LLHTTP_INCLUDE_DIR
        NAMES llhttp.h
        HINTS /opt/homebrew/include /usr/local/include /usr/include
    )
    find_library(LLHTTP_LIBRARY
        NAMES llhttp
        HINTS /opt/homebrew/lib /usr/local/lib /usr/lib
    )
    if(NOT LLHTTP_INCLUDE_DIR OR NOT LLHTTP_LIBRARY)
        message(FATAL_ERROR "llhttp library or headers not found")
    endif()
    add_library(llhttp::llhttp_shared UNKNOWN IMPORTED)
    set_target_properties(llhttp::llhttp_shared PROPERTIES
        IMPORTED_LOCATION "${LLHTTP_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LLHTTP_INCLUDE_DIR}"
    )
    set(LLHTTP_TARGET llhttp::llhttp_shared)
endif()

add_library(alerter_core STATIC
    src/config.c
    src/alert.c
    src/telegram.c
    src/http_server.c
    src/logging.c
)

target_include_directories(alerter_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(alerter_core PUBLIC
    CURL::libcurl
    json-c::json-c
    ${LLHTTP_TARGET}
    Threads::Threads
    ${ALERTER_ATOMIC_LIB}
    alerter_sanitizers
)

target_compile_options(alerter_core PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wformat=2 -Wshadow
)
target_compile_definitions(alerter_core PUBLIC
    $<$<NOT:$<PLATFORM_ID:Darwin>>:_POSIX_C_SOURCE=200809L>
)

if(UNIX AND NOT APPLE)
    target_compile_options(alerter_core PRIVATE -fstack-protector-strong -fPIE)
    if(SANITIZER STREQUAL "none")
        target_compile_definitions(alerter_core PRIVATE
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:_FORTIFY_SOURCE=2>
        )
    endif()
endif()

add_executable(komodo-telegram-alerter src/main.c)
target_link_libraries(komodo-telegram-alerter PRIVATE alerter_core alerter_sanitizers)
target_compile_options(komodo-telegram-alerter PRIVATE
    -Wall -Wextra -Werror -Wpedantic -Wformat=2 -Wshadow
)
if(UNIX AND NOT APPLE)
    target_compile_options(komodo-telegram-alerter PRIVATE -fstack-protector-strong -fPIE)
    if(SANITIZER STREQUAL "none")
        target_compile_definitions(komodo-telegram-alerter PRIVATE
            $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:_FORTIFY_SOURCE=2>
        )
    endif()
    target_link_options(komodo-telegram-alerter PRIVATE -pie -Wl,-z,relro,-z,now)
endif()

enable_testing()
option(BUILD_TESTING "Build unit and integration tests" ON)

if(BUILD_TESTING)
    add_executable(test_alert tests/test_alert.c)
    target_link_libraries(test_alert PRIVATE alerter_core alerter_sanitizers)
    target_compile_options(test_alert PRIVATE
        -Wall -Wextra -Werror -Wpedantic -Wformat=2 -Wshadow
    )
    add_test(NAME test_alert COMMAND test_alert)

    add_executable(test_concurrency tests/test_concurrency.c)
    target_link_libraries(test_concurrency PRIVATE alerter_core alerter_sanitizers)
    target_compile_options(test_concurrency PRIVATE
        -Wall -Wextra -Werror -Wpedantic -Wformat=2 -Wshadow
    )
    add_test(NAME test_concurrency COMMAND test_concurrency)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        add_test(
            NAME test_integration
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_integration.py
                    $<TARGET_FILE:komodo-telegram-alerter>
        )
    endif()
endif()

set(ALERTER_LINT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/alert.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http_server.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/logging.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/telegram.c
)

set(ALERTER_FORMAT_FILES
    ${ALERTER_LINT_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/alert.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/http_server.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/logging.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/telegram.h
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_alert.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_concurrency.c
)

file(GLOB HOMEBREW_LLVM_BINS
    "/opt/homebrew/opt/llvm*/bin"
    "/usr/local/opt/llvm*/bin"
)
list(SORT HOMEBREW_LLVM_BINS ORDER DESCENDING)

find_program(CLANG_FORMAT_EXE NAMES clang-format HINTS ${HOMEBREW_LLVM_BINS})
find_program(CLANG_TIDY_EXE NAMES clang-tidy HINTS ${HOMEBREW_LLVM_BINS})
find_program(CPPCHECK_EXE NAMES cppcheck HINTS /opt/homebrew/bin /usr/local/bin)

set(CLANG_TIDY_EXTRA_ARGS "")
if(APPLE)
    if(CMAKE_OSX_SYSROOT)
        set(LINT_OSX_SYSROOT "${CMAKE_OSX_SYSROOT}")
    else()
        execute_process(
            COMMAND xcrun --show-sdk-path
            OUTPUT_VARIABLE LINT_OSX_SYSROOT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
    if(LINT_OSX_SYSROOT)
        set(CLANG_TIDY_EXTRA_ARGS
            --extra-arg=-isysroot
            --extra-arg=${LINT_OSX_SYSROOT}
        )
    endif()
endif()

if(CLANG_FORMAT_EXE)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i ${ALERTER_FORMAT_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting C files with clang-format"
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${ALERTER_FORMAT_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking C formatting with clang-format"
        VERBATIM
    )
else()
    add_custom_target(format
        COMMAND sh -c "echo 'Error: clang-format not found' >&2; exit 1"
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND sh -c "echo 'Error: clang-format not found' >&2; exit 1"
        VERBATIM
    )
endif()

if(CLANG_TIDY_EXE)
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR} ${CLANG_TIDY_EXTRA_ARGS} ${ALERTER_LINT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy on src/"
        VERBATIM
    )
else()
    add_custom_target(tidy
        COMMAND sh -c "echo 'Error: clang-tidy not found' >&2; exit 1"
        VERBATIM
    )
endif()

if(CPPCHECK_EXE)
    add_custom_target(cppcheck
        COMMAND ${CPPCHECK_EXE}
            --project=${CMAKE_BINARY_DIR}/compile_commands.json
            --enable=warning,style,performance,portability
            --std=c11
            --error-exitcode=1
            --inline-suppr
            --suppress=missingIncludeSystem
            --suppress=unmatchedSuppression
            -i ${CMAKE_CURRENT_SOURCE_DIR}/tests
            --file-filter=${CMAKE_CURRENT_SOURCE_DIR}/src/*
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running cppcheck on src/"
        VERBATIM
    )
else()
    add_custom_target(cppcheck
        COMMAND sh -c "echo 'Error: cppcheck not found' >&2; exit 1"
        VERBATIM
    )
endif()

add_custom_target(lint
    DEPENDS format-check tidy cppcheck
)
