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
        tests/test_rlgc.cpp)
    target_link_libraries(test_faraday PRIVATE faraday Catch2::Catch2WithMain)
    target_compile_definitions(test_faraday PRIVATE
        FARADAY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
endif()
