cmake_minimum_required(VERSION 3.0.0)
project("Lily" C)

if(MSVC)
    add_definitions(/W4)
    add_definitions(/wd4201)
    add_definitions(/wd4204)
    add_definitions(/wd4214)
    add_definitions(/MP)
    set(CMAKE_CTEST_COMMAND	copy lily.exe ..)
else()
    set(CMAKE_CTEST_COMMAND	cp lily ..)
    if(DEBUG)
        set(CMAKE_C_FLAGS      "${CMAKE_C_FLAGS} -g3")
    endif(DEBUG)

    if(NOT QUICK_BUILD)
        set(CMAKE_C_FLAGS      "${CMAKE_C_FLAGS} -O2")
    endif(NOT QUICK_BUILD)

    set(CMAKE_C_FLAGS          "${CMAKE_C_FLAGS} -Wall")
endif()

set(LIBRARY_OUTPUT_PATH    "${PROJECT_BINARY_DIR}/lib")
set(EXECUTABLE_OUTPUT_PATH "${PROJECT_BINARY_DIR}")

set(LILY_MAJOR             "1")
set(LILY_MINOR             "1")

add_definitions(-DLILY_VERSION_DIR="${LILY_MAJOR}_${LILY_MINOR}")

# BSD libc includes the dl* functions and there's no libdl on them.
# Unfortunately, CMake doesn't seem to distinguish *BSD from the other *nixen.
STRING(REGEX MATCH "BSD" IS_BSD ${CMAKE_SYSTEM_NAME})

if(IS_BSD OR MINGW OR MSVC)
    set(LILY_NEED_DL 0)
else()
    set(LILY_NEED_DL 1)
endif()

add_subdirectory(src)
add_subdirectory(run)
add_subdirectory(test)

if(WITH_SANDBOX)
    add_subdirectory(sandbox)
endif(WITH_SANDBOX)

set(TEST_COMMAND cd .. && ./pre-commit-tests)
add_custom_target(check ${CMAKE_CTEST_COMMAND}
  COMMAND ${TEST_COMMAND}
  DEPENDS lily VERBATIM)
