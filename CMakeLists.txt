cmake_minimum_required(VERSION 3.10)
project(zl_language VERSION 0.1.0 LANGUAGES CXX)

set(ZL_VERSION_STRING "${PROJECT_VERSION}")
# Keep the build self-contained: the release archive intentionally excludes
# the cmake/ helper directory, so generate the stdlib version file directly.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/stdlib")
file(WRITE "${CMAKE_BINARY_DIR}/stdlib/VERSION" "${ZL_VERSION_STRING}\n")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Keep the public headers visible to every target, including standalone tools.
# This also makes the project robust against older/stale per-target CMake state.
include_directories(${CMAKE_SOURCE_DIR}/include)

# Add source files
# Collect all .cpp sources under src/ recursively and create executable
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.cpp)

# Create executable
add_executable(zl_language ${SOURCES})

# Include directories
target_include_directories(zl_language PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(zl_language PRIVATE ZL_VERSION_STRING="${ZL_VERSION_STRING}")

if(WIN32)
    # src/vm/native.cpp exposes Network.resolve() through Winsock.
    target_link_libraries(zl_language PRIVATE ws2_32)
endif()

# Bundle stdlib/ next to the built binary, so ZL_STDLIB_ROOT-less runs
# (main.cpp falls back to "<executable's own dir>/stdlib") find it without
# any extra setup, matching how a real install would ship them together.
add_custom_command(TARGET zl_language POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/stdlib
            $<TARGET_FILE_DIR:zl_language>/stdlib
    COMMENT "Copying stdlib/ next to zl_language"
)

add_custom_command(TARGET zl_language POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:zl_language>/stdlib
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/stdlib/VERSION $<TARGET_FILE_DIR:zl_language>/stdlib/VERSION
    COMMENT "Writing stdlib version metadata"
)

# --- zlpkg: the package manager CLI (Phase 6) -------------------------------
# Deliberately its own target/sources under zlpkg/ (NOT under src/, which
# the glob above already sweeps up) - it's a standalone tool that talks to
# zl_language only as a subprocess (see zlpkg/commands.cpp), so it has no
# business linking against the interpreter's own sources/headers.
file(GLOB ZLPKG_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/zlpkg/*.cpp)
add_executable(zlpkg ${ZLPKG_SOURCES})
target_include_directories(zlpkg PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(zlpkg PRIVATE ZL_VERSION_STRING="${ZL_VERSION_STRING}")

# zlpkg finds zl_language by looking next to its own binary at runtime, so
# both need to land in the same output directory.
set_target_properties(zlpkg PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:zl_language>
)
# --- zl-bind: restricted C/C++ header binding generator (Phase 17) ---------
file(GLOB ZLBIND_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/zl-bind/*.cpp)
add_executable(zl-bind ${ZLBIND_SOURCES})
target_include_directories(zl-bind PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Focused runtime metadata regression. Kept separate from the main language
# executable so the invariant can be exercised without a ZL source program.
add_executable(zl-runtime-type-tests tests/runtime_type_tests.cpp
    src/vm/value.cpp
    src/vm/gc.cpp
    src/vm/runtime_task.cpp)
# Controlled collector seam: exercises GC parking/reactivation and collector
# election without depending on allocation volume or thread scheduling luck.
find_package(Threads REQUIRED)
add_executable(zl-gc-safepoint-tests tests/gc_safepoint_tests.cpp src/vm/gc_safepoint.cpp)
target_link_libraries(zl-gc-safepoint-tests PRIVATE Threads::Threads)

add_executable(zl-gc-tests tests/gc_tests.cpp
    src/vm/value.cpp
    src/vm/gc.cpp
    src/vm/runtime_task.cpp)
target_include_directories(zl-gc-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(zl-runtime-type-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_executable(zl-runtime-task-executor-tests tests/runtime_task_executor_tests.cpp
    src/vm/runtime_task.cpp
    src/vm/runtime_task_executor.cpp
    src/vm/runtime_thread.cpp
    src/vm/value.cpp
    src/vm/gc.cpp)
target_include_directories(zl-runtime-task-executor-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_executable(zl-runtime-sync-tests tests/runtime_sync_tests.cpp)
target_include_directories(zl-runtime-sync-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(zl-ir-tests tests/ir_tests.cpp src/compiler/ir.cpp src/compiler/ir_optimizer.cpp)
add_executable(zl-machine-code-tests tests/machine_code_tests.cpp src/compiler/machine_code.cpp src/compiler/ir.cpp)
target_include_directories(zl-machine-code-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(zl-ir-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(zl-regex-engine-tests tests/regex_engine_tests.cpp src/regex/regex.cpp)
target_include_directories(zl-regex-engine-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
add_executable(zl-regex-unicode-tests tests/regex_unicode_tests.cpp src/regex/regex.cpp)
target_include_directories(zl-regex-unicode-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(zl-runtime-scheduler-tests tests/runtime_scheduler_tests.cpp
    src/vm/runtime_scheduler.cpp
    src/vm/runtime_task.cpp
    src/vm/runtime_task_executor.cpp
    src/vm/value.cpp
    src/vm/gc.cpp)
target_include_directories(zl-runtime-scheduler-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

# VM boundary-hardening regression target. It links the interpreter sources
# directly so malformed reflection handles can be exercised without a ZL
# program manufacturing an impossible internal Function object.
set(ZL_RUNTIME_VM_SOURCES ${SOURCES})
list(FILTER ZL_RUNTIME_VM_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
add_executable(zl-runtime-hardening-vm-tests tests/runtime_hardening_vm_tests.cpp ${ZL_RUNTIME_VM_SOURCES})
target_include_directories(zl-runtime-hardening-vm-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

if(WIN32)
    target_link_libraries(zl-runtime-hardening-vm-tests PRIVATE ws2_32)
endif()


# --- Installation (Phase 12A foundation) -----------------------------------
# The installed runtime, package manager, and stdlib share one prefix-relative
# layout so the runtime can locate its stdlib without ZL_STDLIB_ROOT.
include(GNUInstallDirs)
install(TARGETS zl_language
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
if(WIN32)
    install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/zl.bat
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME zl.bat
    )
else()
    install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/zl
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME zl
    )
endif()
install(TARGETS zlpkg
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/stdlib/
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/zl/stdlib
    PATTERN "VERSION.in" EXCLUDE
)
install(FILES ${CMAKE_BINARY_DIR}/stdlib/VERSION
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/zl/stdlib
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/zl/licenses
    OPTIONAL
)
install(FILES ${CMAKE_SOURCE_DIR}/README.md ${CMAKE_SOURCE_DIR}/ROADMAP.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/zl
)


# --- Packaging (Phase 12A distribution) ------------------------------------
# CPack keeps the release layout identical across native build hosts. Native
# Windows/macOS runners can invoke the same configuration to produce their
# platform packages; the Linux build also emits TGZ/DEB/ZIP artifacts.
set(CPACK_PACKAGE_NAME "zl")
set(CPACK_PACKAGE_VENDOR "ZL Language")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ZL programming language toolchain")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ZL")
set(CPACK_GENERATOR "TGZ;ZIP")
if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "TGZ;DEB;ZIP")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "ZL Language")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "")

    # RPM is enabled when rpmbuild is available on the build host. This keeps
    # ordinary Linux development environments free of a hard RPM dependency.
    find_program(RPMBUILD_EXECUTABLE rpmbuild)
    if(RPMBUILD_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "RPM")
        set(CPACK_RPM_PACKAGE_LICENSE "Proprietary")
        set(CPACK_RPM_PACKAGE_GROUP "Development/Languages")
        set(CPACK_RPM_PACKAGE_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
endif()
include(CPack)

# Phase 10 native backend smoke tests: validates the source emitter without invoking an
# external native toolchain. The emitted C++ is intentionally restricted to @native simple functions.
add_executable(zl-native-compiler-tests tests/native_compiler_tests.cpp
    src/compiler/native_compiler.cpp
    src/compiler/ir_lowering.cpp
    src/compiler/ir.cpp
    src/compiler/ir_optimizer.cpp
    src/parser/parser.cpp
    src/parser/expression_parser.cpp
    src/parser/type_lookahead.cpp
    src/parser/ast.cpp
    src/lexer/lexer.cpp)
target_include_directories(zl-native-compiler-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(zl-native-ffi-metadata-tests tests/native_ffi_metadata_tests.cpp)
target_include_directories(zl-native-ffi-metadata-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(zl-native-resource-tests tests/native_resource_tests.cpp
    src/vm/native_resource.cpp
)
target_include_directories(zl-native-resource-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(zl-native-boundary-tests tests/native_boundary_tests.cpp
    src/vm/native_boundary.cpp
    src/vm/native_resource.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_task.cpp)
target_include_directories(zl-native-boundary-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(zl-native-runtime-registry-tests tests/native_runtime_registry_tests.cpp
    src/vm/native_boundary.cpp
    src/vm/native_resource.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_task.cpp)
target_include_directories(zl-native-runtime-registry-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(zl-native-ffi-transport-tests tests/native_ffi_transport_tests.cpp
    src/vm/native_resource.cpp
    src/vm/native_boundary.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_task.cpp)
target_include_directories(zl-native-ffi-transport-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(zl-native-ffi-lifetime-tests tests/native_ffi_lifetime_adapter_tests.cpp
    src/vm/native_resource.cpp
    src/vm/native_boundary.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_task.cpp)
target_include_directories(zl-native-ffi-lifetime-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(zl-native-resource-stress-tests tests/native_resource_stress_tests.cpp
    src/vm/native_resource.cpp)
target_include_directories(zl-native-resource-stress-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)


# Phase 16 native struct/library runtime tests
add_executable(zl-native-struct-tests tests/native_struct_tests.cpp
    src/vm/native_resource.cpp
    src/vm/native_boundary.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_task.cpp)
target_include_directories(zl-native-struct-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(zl-native-library-tests tests/native_library_tests.cpp
    src/vm/native_library.cpp)
target_include_directories(zl-native-library-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(zl-native-library-tests PRIVATE ${CMAKE_DL_LIBS})

add_executable(zl-native-ffi-declaration-tests tests/native_ffi_declaration_tests.cpp
    src/compiler/native_ffi_declarations.cpp)
target_include_directories(zl-native-ffi-declaration-tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
