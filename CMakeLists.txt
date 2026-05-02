cmake_minimum_required(VERSION 3.14)
project(MPISanitizer VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD   11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Find LLVM
# ---------------------------------------------------------------------------
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
message(STATUS "LLVM include dirs: ${LLVM_INCLUDE_DIRS}")
message(STATUS "LLVM library dirs: ${LLVM_LIBRARY_DIRS}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)
include(HandleLLVMOptions)

add_definitions(${LLVM_DEFINITIONS})
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})

# ---------------------------------------------------------------------------
# Find MPI
# ---------------------------------------------------------------------------
find_package(MPI REQUIRED)
message(STATUS "MPI include: ${MPI_C_INCLUDE_DIRS}")
message(STATUS "MPI libs: ${MPI_C_LIBRARIES}")

# ---------------------------------------------------------------------------
# LLVM Pass (shared plugin)
# ---------------------------------------------------------------------------
add_library(MPISanitizerPass MODULE
    src/pass/MPISanitizerPass.cpp
)
target_include_directories(MPISanitizerPass PRIVATE
    src/pass
    ${LLVM_INCLUDE_DIRS}
)
# LLVM plugins must NOT link against libLLVM directly on Linux
set_target_properties(MPISanitizerPass PROPERTIES
    CXX_STANDARD 17
    PREFIX ""
)
# Suppress warnings from LLVM headers
target_compile_options(MPISanitizerPass PRIVATE -fno-rtti)

# ---------------------------------------------------------------------------
# Runtime library (static)
# ---------------------------------------------------------------------------
add_library(mpisan_rt STATIC
    src/runtime/mpisan_rt.c
)
target_include_directories(mpisan_rt PRIVATE
    src/runtime
    ${MPI_C_INCLUDE_DIRS}
)
target_link_libraries(mpisan_rt
    ${MPI_C_LIBRARIES}
    pthread
)
target_compile_options(mpisan_rt PRIVATE -g -O0)

# Also build a shared version for LD_PRELOAD usage
add_library(mpisan_rt_shared SHARED
    src/runtime/mpisan_rt.c
)
target_include_directories(mpisan_rt_shared PRIVATE
    src/runtime
    ${MPI_C_INCLUDE_DIRS}
)
target_link_libraries(mpisan_rt_shared
    ${MPI_C_LIBRARIES}
    pthread
)
set_target_properties(mpisan_rt_shared PROPERTIES OUTPUT_NAME "mpisan_rt")
target_compile_options(mpisan_rt_shared PRIVATE -g -O0)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS MPISanitizerPass mpisan_rt mpisan_rt_shared
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(FILES src/runtime/mpisan_rt.h DESTINATION include)

# ---------------------------------------------------------------------------
# Print summary
# ---------------------------------------------------------------------------
message(STATUS "Build dir: ${CMAKE_BINARY_DIR}")
message(STATUS "Pass plugin: MPISanitizerPass.so")
message(STATUS "Runtime:     libmpisan_rt.a / libmpisan_rt.so")