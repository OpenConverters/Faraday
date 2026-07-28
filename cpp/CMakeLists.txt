cmake_minimum_required(VERSION 3.24)
project(faraday LANGUAGES CXX)

option(FARADAY_BUILD_TESTS "Build the Catch2 test binary" ON)

include(FetchContent)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(json)

add_library(faraday INTERFACE)
target_include_directories(faraday INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(faraday INTERFACE cxx_std_20)
target_link_libraries(faraday INTERFACE nlohmann_json::nlohmann_json)

add_executable(faraday_cli tools/faraday_cli.cpp)
target_link_libraries(faraday_cli PRIVATE faraday)

# ---- Optional field-solver tier -------------------------------------------
# faraday_solve links OMFEM (and therefore MFEM) to run the real 2D
# electrostatic extraction. It is OFF by default so the ordinary build — and
# the WASM engine the browser loads — stay free of that dependency.
#   cmake -S cpp -B build -DFARADAY_OMFEM_ROOT=$HOME/OpenMagnetics/OMFEM
set(FARADAY_OMFEM_ROOT "" CACHE PATH "OMFEM checkout with a built libomfem")
set(FARADAY_KIRCHHOFF_ROOT "" CACHE PATH "Kirchhoff checkout")
set(FARADAY_KIRCHHOFF_BUILD "" CACHE PATH "Kirchhoff build dir with libkirchhoff")
if(FARADAY_OMFEM_ROOT)
    find_library(OMFEM_LIBRARY omfem PATHS ${FARADAY_OMFEM_ROOT}/build NO_DEFAULT_PATH)
    find_library(MFEM_LIBRARY mfem
                 PATHS ${FARADAY_OMFEM_ROOT}/build/_deps/mfem-build NO_DEFAULT_PATH)
    if(NOT OMFEM_LIBRARY OR NOT MFEM_LIBRARY)
        message(FATAL_ERROR
            "FARADAY_OMFEM_ROOT=${FARADAY_OMFEM_ROOT} but libomfem/libmfem were not "
            "found under it — build OMFEM with -DOMFEM_USE_MFEM=ON first")
    endif()
    add_executable(faraday_solve tools/faraday_solve.cpp)
    target_include_directories(faraday_solve PRIVATE
        ${FARADAY_OMFEM_ROOT}/include
        ${FARADAY_OMFEM_ROOT}/build/_deps/mfem-src
        ${FARADAY_OMFEM_ROOT}/build/_deps/mfem-build)
    target_link_libraries(faraday_solve PRIVATE faraday ${OMFEM_LIBRARY} ${MFEM_LIBRARY})
    message(STATUS "Faraday: building faraday_solve against OMFEM at ${FARADAY_OMFEM_ROOT}")

    # Optional time-domain run through Kirchhoff's IN-PROCESS libngspice. The
    # external ngspice binary is never used — house rule, and prod must run
    # without it installed.
    #   -DFARADAY_KIRCHHOFF_ROOT=$HOME/OpenConverters/Kirchhoff
    #   -DFARADAY_KIRCHHOFF_BUILD=<a build dir configured with ENABLE_NGSPICE=ON>
    set(FARADAY_KIRCHHOFF_ROOT "" CACHE PATH "Kirchhoff checkout")
    set(FARADAY_KIRCHHOFF_BUILD "" CACHE PATH "Kirchhoff build dir with libkirchhoff")
    if(FARADAY_KIRCHHOFF_ROOT AND FARADAY_KIRCHHOFF_BUILD)
        find_library(KIRCHHOFF_LIBRARY kirchhoff
                     PATHS ${FARADAY_KIRCHHOFF_BUILD} NO_DEFAULT_PATH)
        find_library(NGSPICE_LIBRARY ngspice)
        if(NOT KIRCHHOFF_LIBRARY OR NOT NGSPICE_LIBRARY)
            message(FATAL_ERROR
                "FARADAY_KIRCHHOFF_* set but libkirchhoff/libngspice were not found "
                "— build Kirchhoff with -DENABLE_NGSPICE=ON first")
        endif()
        # A SEPARATE binary: linking MFEM and libngspice into one process
        # segfaults (ngspice's shared-library mode is not re-entrant and does
        # not coexist with the solver runtime). faraday_solve writes the deck,
        # faraday_spice runs it — still through Kirchhoff in-process, never the
        # external ngspice binary.
        add_executable(faraday_spice tools/faraday_spice.cpp)
        target_include_directories(faraday_spice PRIVATE ${FARADAY_KIRCHHOFF_ROOT}/src)
        target_link_libraries(faraday_spice PRIVATE faraday ${KIRCHHOFF_LIBRARY} ${NGSPICE_LIBRARY})
        message(STATUS "Faraday: building faraday_spice (Kirchhoff in-process ngspice)")
    endif()
endif()

# ---- Cross-check the browser's transient solver against real ngspice -------
# Native only, and independent of the OMFEM tier: this needs Kirchhoff alone.
#   cmake -S cpp -B build -DFARADAY_KIRCHHOFF_ROOT=$HOME/OpenConverters/Kirchhoff \
#                         -DFARADAY_KIRCHHOFF_BUILD=$HOME/OpenConverters/Kirchhoff/build-latest
if(FARADAY_KIRCHHOFF_ROOT AND FARADAY_KIRCHHOFF_BUILD)
    find_library(XCHECK_KIRCHHOFF kirchhoff PATHS ${FARADAY_KIRCHHOFF_BUILD} NO_DEFAULT_PATH)
    find_library(XCHECK_NGSPICE ngspice)
    if(NOT XCHECK_KIRCHHOFF OR NOT XCHECK_NGSPICE)
        message(FATAL_ERROR
            "FARADAY_KIRCHHOFF_* set but libkirchhoff/libngspice were not found "
            "— build Kirchhoff with -DENABLE_NGSPICE=ON first")
    endif()
    add_executable(faraday_xcheck tools/faraday_xcheck.cpp)
    target_include_directories(faraday_xcheck PRIVATE ${FARADAY_KIRCHHOFF_ROOT}/src)
    target_link_libraries(faraday_xcheck PRIVATE faraday ${XCHECK_KIRCHHOFF} ${XCHECK_NGSPICE})
    message(STATUS "Faraday: building faraday_xcheck (mna vs in-process ngspice)")
endif()

if(FARADAY_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.8.1)
    FetchContent_MakeAvailable(Catch2)

    add_executable(test_faraday
        tests/test_sexpr.cpp
        tests/test_importer.cpp
        tests/test_tline.cpp
        tests/test_screener.cpp
        tests/test_real_boards.cpp
        tests/test_hyp.cpp
        tests/test_ipc.cpp
        tests/test_gerber.cpp
        tests/test_odb.cpp
        tests/test_rlgc.cpp
        tests/test_bem.cpp
        tests/test_mna.cpp
        tests/test_bench.cpp
        tests/test_emissions.cpp
        tests/test_returnpath.cpp
        tests/test_nearfield.cpp
        tests/test_nfmap.cpp
        tests/test_shielding.cpp
        tests/test_pdn.cpp)
    target_link_libraries(test_faraday PRIVATE faraday Catch2::Catch2WithMain)
    target_compile_definitions(test_faraday PRIVATE
        FARADAY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
endif()
