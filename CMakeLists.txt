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

# --- Optional focused C++ regression targets ---------------------------------
# Some lightweight/source-only checkouts do not include the full C++ regression
# corpus. Keep ordinary runtime/tool builds configurable by registering each
# regression target only when its primary fixture source is present.
find_package(Threads REQUIRED)

function(zl_add_optional_test target primary_source)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${primary_source}")
        message(STATUS "Skipping ${target}: missing ${primary_source}")
        return()
    endif()
    add_executable(${target} ${primary_source} ${ARGN})
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/include)
endfunction()

zl_add_optional_test(zl-runtime-type-tests tests/runtime_type_tests.cpp
    src/vm/value.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

# Controlled collector seam: exercises GC parking/reactivation and collector
# election without depending on allocation volume or thread scheduling luck.
zl_add_optional_test(zl-gc-safepoint-tests tests/gc_safepoint_tests.cpp src/vm/gc_safepoint.cpp)
if(TARGET zl-gc-safepoint-tests)
    target_link_libraries(zl-gc-safepoint-tests PRIVATE Threads::Threads)
endif()

# Program graph roots and blocking reclamation across consecutive collections.
zl_add_optional_test(zl-gc-lifetime-tests tests/gc_lifetime_tests.cpp
    src/vm/gc.cpp src/vm/gc_safepoint.cpp src/vm/value.cpp
    src/vm/runtime_task.cpp src/vm/runtime_exception.cpp src/vm/runtime_thread.cpp)
if(TARGET zl-gc-lifetime-tests)
    target_link_libraries(zl-gc-lifetime-tests PRIVATE Threads::Threads)
endif()

zl_add_optional_test(zl-gc-tests tests/gc_tests.cpp
    src/vm/value.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

zl_add_optional_test(zl-runtime-task-executor-tests tests/runtime_task_executor_tests.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp
    src/vm/runtime_task_executor.cpp
    src/vm/runtime_thread.cpp
    src/vm/value.cpp
    src/vm/gc.cpp)

zl_add_optional_test(zl-runtime-sync-tests tests/runtime_sync_tests.cpp)

zl_add_optional_test(zl-ir-tests tests/ir_tests.cpp src/compiler/ir.cpp src/compiler/ir_optimizer.cpp)
zl_add_optional_test(zl-machine-code-tests tests/machine_code_tests.cpp src/compiler/machine_code.cpp src/compiler/ir.cpp)

zl_add_optional_test(zl-regex-engine-tests tests/regex_engine_tests.cpp src/regex/regex.cpp)
zl_add_optional_test(zl-regex-unicode-tests tests/regex_unicode_tests.cpp src/regex/regex.cpp)

zl_add_optional_test(zl-runtime-scheduler-tests tests/runtime_scheduler_tests.cpp
    src/vm/runtime_scheduler.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp
    src/vm/runtime_task_executor.cpp
    src/vm/value.cpp
    src/vm/gc.cpp)

# VM boundary-hardening regression target. It links the interpreter sources
# directly so malformed reflection handles can be exercised without a ZL
# program manufacturing an impossible internal Function object.
set(ZL_RUNTIME_VM_SOURCES ${SOURCES})
list(FILTER ZL_RUNTIME_VM_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
zl_add_optional_test(zl-runtime-hardening-vm-tests tests/runtime_hardening_vm_tests.cpp ${ZL_RUNTIME_VM_SOURCES})
if(TARGET zl-runtime-hardening-vm-tests AND WIN32)
    target_link_libraries(zl-runtime-hardening-vm-tests PRIVATE ws2_32)
endif()

# --- Native backend (MIR -> native IR -> machine code) ----------------------
# Drives the whole native pipeline and *executes* the emitted machine code, so
# it links the front end (its end-to-end cases lower real ZL source) plus the
# native sources themselves.
zl_add_optional_test(zl-native-backend-tests tests/native_backend_tests.cpp ${ZL_RUNTIME_VM_SOURCES})
if(TARGET zl-native-backend-tests AND WIN32)
    target_link_libraries(zl-native-backend-tests PRIVATE ws2_32)
endif()
if(TARGET zl-native-backend-tests)
    target_link_libraries(zl-native-backend-tests PRIVATE Threads::Threads)
endif()

# --- MIR (mid-level IR) -----------------------------------------------------
# Two targets, split by what they need to link.
#
# zl-mir-tests builds MIR by hand and checks the verifier rejects each class of
# malformed module. It only needs the MIR sources plus the two compiler
# utilities the verifier delegates to, so it stays fast and does not depend on
# the front end.
# CFG queries, data-flow analyses, and slot -> block-parameter construction.
zl_add_optional_test(zl-mir-ssa-tests tests/mir_ssa_tests.cpp
    src/mir/folding.cpp
    src/mir/effects.cpp
    src/mir/analysis.cpp
    src/mir/builder.cpp
    src/mir/dataflow.cpp
    src/mir/function.cpp
    src/mir/instruction.cpp
    src/mir/printer.cpp
    src/mir/ssa.cpp
    src/mir/type.cpp
    src/mir/verifier.cpp
    src/mir/safety.cpp
    src/compiler/operator_rules.cpp
    src/compiler/semantic_types.cpp
    src/common/type_name.cpp)
zl_add_optional_test(zl-mir-tests tests/mir_tests.cpp
    src/mir/analysis.cpp
    src/mir/builder.cpp
    src/mir/function.cpp
    src/mir/instruction.cpp
    src/mir/printer.cpp
    src/mir/reachability.cpp
    src/mir/type.cpp
    src/mir/verifier.cpp
    src/mir/safety.cpp
    src/compiler/operator_rules.cpp
    src/compiler/native_catalog.cpp
    src/compiler/semantic_types.cpp
    src/common/type_name.cpp)

# The optimiser framework: pass manager, analysis manager, snapshots, the
# passes themselves, and the differential validator. It needs no front end, so
# it stays a fast target like zl-mir-ssa-tests.
zl_add_optional_test(zl-mir-opt-tests tests/mir_pass_tests.cpp
    src/mir/analysis.cpp
    src/mir/builder.cpp
    src/mir/dataflow.cpp
    src/mir/differential.cpp
    src/mir/effects.cpp
    src/mir/folding.cpp
    src/mir/function.cpp
    src/mir/instruction.cpp
    src/mir/opt_constant.cpp
    src/mir/opt_dead.cpp
    src/mir/opt_simplify.cpp
    src/mir/pass_manager.cpp
    src/mir/printer.cpp
    src/mir/ssa.cpp
    src/mir/type.cpp
    src/mir/verifier.cpp
    src/mir/safety.cpp
    src/compiler/operator_rules.cpp
    src/compiler/semantic_types.cpp
    src/common/type_name.cpp)

# zl-mir-opt-pipeline-tests runs the optimiser over real ZL programs and diffs
# the two modules twice - statically (compareModules) and by executing both
# through the bytecode backend with their output captured - so it links the
# whole compiler minus main(), like the lowering target.
zl_add_optional_test(zl-mir-opt-pipeline-tests tests/mir_opt_pipeline_tests.cpp ${ZL_RUNTIME_VM_SOURCES})
if(TARGET zl-mir-opt-pipeline-tests AND WIN32)
    target_link_libraries(zl-mir-opt-pipeline-tests PRIVATE ws2_32)
endif()
if(TARGET zl-mir-opt-pipeline-tests)
    target_link_libraries(zl-mir-opt-pipeline-tests PRIVATE Threads::Threads)
endif()

# zl-pipeline-tests owns the compiler pipeline itself: stage order, the
# invariant that no backend sees unverified MIR, backend selection as a
# code-generation decision only, and the exit-code contract. It links the whole
# compiler minus main() (ZL_RUNTIME_VM_SOURCES) because these are end-to-end
# properties: a pipeline assembled out of mocks would prove nothing about the
# one that ships.
zl_add_optional_test(zl-pipeline-tests tests/pipeline_tests.cpp ${ZL_RUNTIME_VM_SOURCES})
if(TARGET zl-pipeline-tests AND WIN32)
    target_link_libraries(zl-pipeline-tests PRIVATE ws2_32)
endif()
if(TARGET zl-pipeline-tests)
    target_link_libraries(zl-pipeline-tests PRIVATE Threads::Threads)
endif()

# zl-mir-lowering-tests drives the real pipeline - ModuleLoader, TypeChecker,
# lowerProgram, verifyModule - so it links the whole compiler minus main().
zl_add_optional_test(zl-mir-lowering-tests tests/mir_lowering_tests.cpp ${ZL_RUNTIME_VM_SOURCES})

# Type-system integration: the type table, sum types, canonical nested
# generics through lowering, dynamic-to-static boundaries, and the verifier
# rules that keep invalid type assumptions detectable. Links the whole front
# end because most of its cases drive the real pipeline.
zl_add_optional_test(zl-mir-type-tests tests/mir_type_tests.cpp ${ZL_RUNTIME_VM_SOURCES})

# Ownership/lifetime integration: storage contracts and the move/borrow/drop
# events, with the verifier's flow rules pinning each invalid state. Links the
# whole front end for its pipeline case.
zl_add_optional_test(zl-mir-ownership-tests tests/mir_ownership_tests.cpp ${ZL_RUNTIME_VM_SOURCES})
if(TARGET zl-mir-lowering-tests AND WIN32)
    target_link_libraries(zl-mir-lowering-tests PRIVATE ws2_32)
endif()
if(TARGET zl-mir-lowering-tests)
    target_link_libraries(zl-mir-lowering-tests PRIVATE Threads::Threads)
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
install(PROGRAMS ${CMAKE_SOURCE_DIR}/tools/zl-lsp/zl_lsp.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME zl-lsp
)
install(PROGRAMS ${CMAKE_SOURCE_DIR}/tools/zl-test/zl_test.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME zl-test
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
    OPTIONAL
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

# Phase 10+ native/backend smoke tests. These are optional for lightweight
# checkouts for the same reason as the focused tests above.
zl_add_optional_test(zl-native-compiler-tests tests/native_compiler_tests.cpp
    src/compiler/native_compiler.cpp
    src/compiler/ir_lowering.cpp
    src/compiler/ir.cpp
    src/compiler/ir_optimizer.cpp
    src/parser/parser.cpp
    src/parser/expression_parser.cpp
    src/parser/type_lookahead.cpp
    src/parser/ast.cpp
    src/lexer/lexer.cpp)

zl_add_optional_test(zl-native-ffi-metadata-tests tests/native_ffi_metadata_tests.cpp)

zl_add_optional_test(zl-native-resource-tests tests/native_resource_tests.cpp
    src/vm/native_resource.cpp)

zl_add_optional_test(zl-native-boundary-tests tests/native_boundary_tests.cpp
    src/vm/native_boundary.cpp
    src/vm/native_resource.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

zl_add_optional_test(zl-native-runtime-registry-tests tests/native_runtime_registry_tests.cpp
    src/vm/native_boundary.cpp
    src/vm/native_resource.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

zl_add_optional_test(zl-native-ffi-transport-tests tests/native_ffi_transport_tests.cpp
    src/vm/native_resource.cpp
    src/vm/native_boundary.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

zl_add_optional_test(zl-native-ffi-lifetime-tests tests/native_ffi_lifetime_adapter_tests.cpp
    src/vm/native_resource.cpp
    src/vm/native_boundary.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

zl_add_optional_test(zl-native-resource-stress-tests tests/native_resource_stress_tests.cpp
    src/vm/native_resource.cpp)

# Phase 16 native struct/library runtime tests.
zl_add_optional_test(zl-native-struct-tests tests/native_struct_tests.cpp
    src/vm/native_resource.cpp
    src/vm/native_boundary.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

zl_add_optional_test(zl-native-library-tests tests/native_library_tests.cpp
    src/vm/native_library.cpp)
if(TARGET zl-native-library-tests)
    target_link_libraries(zl-native-library-tests PRIVATE ${CMAKE_DL_LIBS})
endif()

zl_add_optional_test(zl-native-ffi-declaration-tests tests/native_ffi_declaration_tests.cpp
    src/compiler/native_ffi_declarations.cpp)

# Safety analysis regression: malformed MIR is measured separately from source
# corpus rejection layers, because it tests compiler/pass contracts.
zl_add_optional_test(zl-mir-safety-tests tests/mir_safety_tests.cpp
    src/mir/analysis.cpp src/mir/builder.cpp src/mir/function.cpp
    src/mir/instruction.cpp src/mir/printer.cpp src/mir/type.cpp
    src/mir/verifier.cpp src/mir/safety.cpp
    src/compiler/operator_rules.cpp src/compiler/semantic_types.cpp src/common/type_name.cpp)

include(CTest)
if(BUILD_TESTING AND TARGET zl-pipeline-tests)
    add_test(NAME pipeline COMMAND zl-pipeline-tests)
endif()
# The compiler boundary, enforced as a check rather than as a convention: the
# backends cannot see the AST, the lowerer cannot infer, the front end has one
# driver, and the legacy IR cannot grow a second consumer.
if(BUILD_TESTING)
    find_program(ZL_BASH NAMES bash)
    if(ZL_BASH)
        add_test(NAME boundary-lint COMMAND ${ZL_BASH}
            ${CMAKE_SOURCE_DIR}/tools/boundary_lint.sh ${CMAKE_SOURCE_DIR})
        # The lint's own regressions: the rules must fail on the broken trees
        # they exist for, with a diagnostic that names the file and line.
        add_test(NAME boundary-lint-regressions COMMAND ${ZL_BASH}
            ${CMAKE_SOURCE_DIR}/tests/boundary_lint_tests.sh ${CMAKE_SOURCE_DIR})
    endif()
endif()
if(BUILD_TESTING AND TARGET zl-mir-safety-tests)
    add_test(NAME mir-safety COMMAND zl-mir-safety-tests)
    find_program(ZL_SAFETY_PYTHON NAMES python3)
    if(ZL_SAFETY_PYTHON)
        add_test(NAME safety-pipeline COMMAND ${ZL_SAFETY_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/safety_pipeline.py $<TARGET_FILE:zl_language>)
        set_tests_properties(safety-pipeline PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
endif()
# The --pipeline-report CLI contract: a runnable program's pipeline, or a
# precise refusal (library without an entry point, invalid entry point).
if(BUILD_TESTING)
    find_program(ZL_REPORT_PYTHON NAMES python3)
    if(ZL_REPORT_PYTHON)
        add_test(NAME pipeline-report COMMAND ${ZL_REPORT_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/pipeline_report_cli.py $<TARGET_FILE:zl_language>)
        set_tests_properties(pipeline-report PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
endif()
