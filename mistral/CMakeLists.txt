set(SOURCES
    arch.cc
    archdefs.cc
    archdefs.h
    arch.h
    arch_pybindings.cc
    arch_pybindings.h
    bitstream.cc
    vup_cram.cc
    constids.inc
    delay.cc
    globals.cc
    io.cc
    dsp.cc
    lab.cc
    lab_control_abi.h
    lab_control_plan.h
    lab_control_plan.cc
    lab_control_edits.h
    lab_control_edits.cc
    lab_preparation.h
    lab_preparation.cc
    lab_model.h
    lab_model.cc
    lab_snapshot.h
    lab_legality.cc
    lab_dispatch.h
    lab_profile.h
    lab_profile.cc
    lab_replay.h
    lab_replay.cc
    lab_v2_abi.h
    lab_v2.h
    lab_resident.cc
    lab_v2.cc
    lab_v2_replay.h
    lab_v2_replay.cc
    lab_frozen_batch.h
    lab_frozen_batch.cc
    lab_reuse.h
    lab_reuse.cc
    placement_revision.h
    placement_revision.cc
    placement_transaction.h
    placement_transaction.cc
    checkpoint.h
    placement_coordinator.h
    checkpoint.cc
    placement_coordinator.cc
    placement_reuse.h
    placement_reuse.cc
    route_reuse.h
    route_reuse.cc
    alm_pairing.cc
    register_packing.cc
    reuse_plan.h
    reuse_plan.cc
    build_state.h
    build_state.cc
    m10k.cc
    pll.cc
    pack.cc
    pins.cc
    qsf.cc
)

add_nextpnr_architecture(${family}
    CORE_SOURCES ${SOURCES}
    MAIN_SOURCE  main.cc
    TEST_SOURCES tests/lab_legality.cc
)

if (BUILD_TESTS)
    target_compile_definitions(nextpnr-${family}-test PRIVATE
        LAB_CONTROL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
endif()

if (BUILD_RUST)
    target_link_libraries(nextpnr-${family}-core INTERFACE npnr_mistral_lab_ffi)
    if (BUILD_TESTS)
        target_sources(nextpnr-${family}-test PRIVATE tests/lab_ffi.cc)
        add_executable(nextpnr-mistral-lab-ffi-replay
            tests/lab_control_oracle.cc lab_model.cc lab_replay.cc)
        target_compile_definitions(nextpnr-mistral-lab-ffi-replay PRIVATE LAB_CONTROL_FFI_DRIVER)
        target_include_directories(nextpnr-mistral-lab-ffi-replay PRIVATE
            . ${CMAKE_SOURCE_DIR}/common/kernel ${CMAKE_SOURCE_DIR}/3rdparty/json11)
        target_link_libraries(nextpnr-mistral-lab-ffi-replay PRIVATE npnr_mistral_lab_ffi json11)
        add_executable(nextpnr-mistral-lab-bench tests/lab_bench.cc)
        target_link_libraries(nextpnr-mistral-lab-bench PRIVATE nextpnr-${family}-core)
        add_executable(nextpnr-mistral-lab-frozen-bench tests/lab_frozen_bench.cc)
        target_link_libraries(nextpnr-mistral-lab-frozen-bench PRIVATE nextpnr-${family}-core)
    endif()
endif()

set(MISTRAL_ROOT "" CACHE STRING "Mistral install path")
set(MISTRAL_DONT_INSTALL ON)

add_subdirectory(${MISTRAL_ROOT}/tools ${CMAKE_CURRENT_BINARY_DIR}/tools)
add_subdirectory(${MISTRAL_ROOT}/generator ${CMAKE_CURRENT_BINARY_DIR}/generator)
add_subdirectory(${MISTRAL_ROOT}/libmistral ${CMAKE_CURRENT_BINARY_DIR}/libmistral)

find_package(LibLZMA REQUIRED)

target_include_directories(nextpnr-${family}-core INTERFACE
    ${MISTRAL_ROOT}/libmistral
    ${CMAKE_CURRENT_BINARY_DIR}/tools
    ${CMAKE_CURRENT_BINARY_DIR}/libmistral
    ${LIBLZMA_INCLUDE_DIRS}
)

target_link_libraries(nextpnr-${family}-core INTERFACE
    mistral
    ${LIBLZMA_LIBRARIES}
)
