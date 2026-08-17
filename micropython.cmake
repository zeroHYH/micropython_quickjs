add_library(usermod_quickjs INTERFACE)

# QuickJS-NG engine is fetched from GitHub, not vendored (see get_quickjs.sh).
# If it is not present yet, fetch it now at configure time (network once).
if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/src/quickjs.h)
    message(STATUS "[quickjs] QuickJS-NG not found; running get_quickjs.sh ...")
    execute_process(
        COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/get_quickjs.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        RESULT_VARIABLE QJS_FETCH_RESULT
        OUTPUT_QUIET
    )
    if(NOT QJS_FETCH_RESULT EQUAL 0)
        message(FATAL_ERROR
            "[quickjs] failed to fetch QuickJS-NG; run get_quickjs.sh manually")
    endif()
endif()

target_sources(usermod_quickjs INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modquickjs.c
    ${CMAKE_CURRENT_LIST_DIR}/src/quickjs.c
    ${CMAKE_CURRENT_LIST_DIR}/src/libregexp.c
    ${CMAKE_CURRENT_LIST_DIR}/src/libunicode.c
    ${CMAKE_CURRENT_LIST_DIR}/src/dtoa.c
)

target_include_directories(usermod_quickjs INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
)

target_compile_definitions(usermod_quickjs INTERFACE
    -D_GNU_SOURCE
)

# Suppress strict GCC 14 warnings
target_compile_options(usermod_quickjs INTERFACE
    -Wno-error
    -Wno-incompatible-pointer-types
    -Wno-format
    -Wno-unused-function
    -Wno-maybe-uninitialized
    -Wno-sign-compare
    -Wno-unused-variable
    -Wno-unused-parameter
    -Wno-implicit-fallthrough
)

target_link_libraries(usermod INTERFACE usermod_quickjs)
