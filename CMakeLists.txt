cmake_minimum_required(VERSION 3.24)
project(NamToClo VERSION 2.9.4 LANGUAGES C CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "NAM to CLO targets Windows x64.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "A 64-bit build is required.")
endif()

set(NAM_CORE_SOURCE_DIR "" CACHE PATH "Optional local NeuralAmpModelerCore source tree (avoids network FetchContent)")
set(R8BRAIN_SOURCE_DIR "" CACHE PATH "Optional local r8brain-free-src source tree (avoids network FetchContent)")
set(NTC_OOURA_SOURCE_DIR "" CACHE PATH "Optional source tree containing src/fft4g.c (SoXR/Ooura FFT4G)")
set(NTC_R8BRAIN_REF "version-3.7" CACHE STRING "Pinned r8brain revision used by the converter")

add_executable(NamToClo WIN32
    src/gui.cpp
    src/stimulus.cpp
    src/stimulus.hpp
    src/corrective_ir.cpp
    src/corrective_ir.hpp
    src/clo_refiner.cpp
    src/clo_refiner.hpp
    src/common.cpp
    src/common.hpp
    src/gp200_clo_upload.cpp
    src/gp200_clo_upload.hpp
    src/gp200_midi.cpp
    src/gp200_midi.hpp
    src/gp5_clo_upload.cpp
    src/gp5_clo_upload.hpp
    src/gp5_midi.cpp
    src/gp5_midi.hpp
    src/tone3000_client.cpp
    src/tone3000_client.hpp
    src/nam_preview_player.cpp
    src/nam_preview_player.hpp
    src/resources.rc
    src/resource.h
)

target_compile_features(NamToClo PRIVATE cxx_std_20)
target_compile_definitions(NamToClo PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
# winmm also provides the v2.9.1 buffered real-time NAM preview output.
target_link_libraries(NamToClo PRIVATE comdlg32 shell32 ole32 dbghelp advapi32 comctl32 winmm winhttp bcrypt ws2_32 crypt32)

if(NAM_CORE_SOURCE_DIR)
    set(_ntc_namcore_dir "${NAM_CORE_SOURCE_DIR}")
else()
    include(FetchContent)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Declare(
        ntc_namcore_src
        GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore.git
        GIT_TAG v0.5.4
        GIT_SHALLOW TRUE
        GIT_SUBMODULES_RECURSE TRUE
    )
    FetchContent_GetProperties(ntc_namcore_src)
    if(NOT ntc_namcore_src_POPULATED)
        FetchContent_Populate(ntc_namcore_src)
    endif()
    set(_ntc_namcore_dir "${ntc_namcore_src_SOURCE_DIR}")
endif()

if(NOT EXISTS "${_ntc_namcore_dir}/NAM/get_dsp.h")
    message(FATAL_ERROR
        "NeuralAmpModelerCore was not found. "
        "Provide -DNAM_CORE_SOURCE_DIR=<path> or allow FetchContent network access.")
endif()

# The official converter uses r8b::CDSPResampler24. r8brain-free-src is
# header-only and is compiled into this executable; there is no runtime DLL.
if(R8BRAIN_SOURCE_DIR)
    set(_ntc_r8brain_dir "${R8BRAIN_SOURCE_DIR}")
else()
    include(FetchContent)
    FetchContent_Declare(
        ntc_r8brain_src
        GIT_REPOSITORY https://github.com/avaneev/r8brain-free-src.git
        GIT_TAG ${NTC_R8BRAIN_REF}
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(ntc_r8brain_src)
    if(NOT ntc_r8brain_src_POPULATED)
        FetchContent_Populate(ntc_r8brain_src)
    endif()
    set(_ntc_r8brain_dir "${ntc_r8brain_src_SOURCE_DIR}")
endif()
if(NOT EXISTS "${_ntc_r8brain_dir}/CDSPResampler.h")
    message(FATAL_ERROR
        "r8brain-free-src was not found. "
        "Provide -DR8BRAIN_SOURCE_DIR=<path> or allow FetchContent network access.")
endif()

# HTUSBTools/GP-200 spectral estimator uses Takuya Ooura's FFT4G RDFT.
# Compile the C implementation shipped by SoXR 0.1.3 directly into the
# executable; this is a build-time source dependency, not a runtime DLL.
if(NTC_OOURA_SOURCE_DIR)
    set(_ntc_ooura_dir "${NTC_OOURA_SOURCE_DIR}")
else()
    include(FetchContent)
    FetchContent_Declare(
        ntc_ooura_src
        GIT_REPOSITORY https://github.com/chirlu/soxr.git
        GIT_TAG 0.1.3
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(ntc_ooura_src)
    if(NOT ntc_ooura_src_POPULATED)
        FetchContent_Populate(ntc_ooura_src)
    endif()
    set(_ntc_ooura_dir "${ntc_ooura_src_SOURCE_DIR}")
endif()
if(NOT EXISTS "${_ntc_ooura_dir}/src/fft4g.c")
    message(FATAL_ERROR
        "Ooura FFT4G source was not found. "
        "Provide -DNTC_OOURA_SOURCE_DIR=<soxr-source-path> or allow FetchContent network access.")
endif()

file(GLOB_RECURSE NTC_NAMCORE_SOURCES CONFIGURE_DEPENDS
    "${_ntc_namcore_dir}/NAM/*.cpp"
)
# NAMCore v0.5.x registers architecture/config parsers (WaveNet, LSTM,
# ConvNet, SlimmableContainer, ...) from translation-unit static
# initializers.  When the sources are packed into a normal STATIC library,
# MSVC only extracts objects that satisfy referenced symbols, so parser-only
# objects can be discarded and get_dsp() then reports e.g.
# "No config parser registered for architecture: WaveNet" at runtime.
#
# Use an OBJECT library so every NAMCore translation unit is linked into the
# executable and all built-in parser registrations execute before main().
add_library(ntc_namcore OBJECT ${NTC_NAMCORE_SOURCES})
target_compile_features(ntc_namcore PUBLIC cxx_std_20)
target_include_directories(ntc_namcore PUBLIC
    "${_ntc_namcore_dir}"
    "${_ntc_namcore_dir}/Dependencies/eigen"
    "${_ntc_namcore_dir}/Dependencies/nlohmann"
    "${_ntc_namcore_dir}/Dependencies/AudioDSPTools"
)
target_compile_definitions(ntc_namcore PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX NAM_ENABLE_A2_FAST NAM_SAMPLE_FLOAT
)

target_sources(NamToClo PRIVATE
    src/native_converter.cpp
    src/native_converter.hpp
    "${_ntc_ooura_dir}/src/fft4g.c"
)
target_include_directories(NamToClo PRIVATE "${_ntc_ooura_dir}/src")
# Tone3000 client uses the nlohmann JSON headers bundled with NAMCore.
# Add it explicitly to the executable target; OBJECT-library usage does not
# reliably propagate this include directory with all CMake/MSVC combinations.
target_include_directories(NamToClo PRIVATE "${_ntc_namcore_dir}/Dependencies/nlohmann")
# r8brain releases before v7 are not fully header-only; the historical
# template-era branch used by the official converter ships r8bbase.cpp.
# Build that third-party translation unit separately so our /W4 policy does
# not turn third-party optimizer diagnostics into a failure of NamToClo.
if(EXISTS "${_ntc_r8brain_dir}/r8bbase.cpp")
    add_library(ntc_r8brain_base OBJECT "${_ntc_r8brain_dir}/r8bbase.cpp")
    target_include_directories(ntc_r8brain_base PRIVATE "${_ntc_r8brain_dir}")
    target_sources(NamToClo PRIVATE $<TARGET_OBJECTS:ntc_r8brain_base>)
endif()
target_include_directories(NamToClo PRIVATE "${_ntc_r8brain_dir}")
target_link_libraries(NamToClo PRIVATE ntc_namcore)
target_compile_definitions(NamToClo PRIVATE NAM_SAMPLE_FLOAT)

# Preserve license notices when distributing a statically linked build.
if(EXISTS "${_ntc_namcore_dir}/LICENSE")
    install(FILES "${_ntc_namcore_dir}/LICENSE" DESTINATION . RENAME NEURALAMPMODELERCORE_LICENSE.txt)
endif()
if(EXISTS "${_ntc_namcore_dir}/Dependencies/nlohmann/LICENSE.MIT")
    install(FILES "${_ntc_namcore_dir}/Dependencies/nlohmann/LICENSE.MIT" DESTINATION . RENAME NLOHMANN_JSON_LICENSE.txt)
endif()
if(EXISTS "${_ntc_namcore_dir}/Dependencies/eigen/COPYING.MPL2")
    install(FILES "${_ntc_namcore_dir}/Dependencies/eigen/COPYING.MPL2" DESTINATION . RENAME EIGEN_LICENSE.txt)
endif()
if(EXISTS "${_ntc_namcore_dir}/Dependencies/AudioDSPTools/LICENSE")
    install(FILES "${_ntc_namcore_dir}/Dependencies/AudioDSPTools/LICENSE" DESTINATION . RENAME AUDIODSPTOOLS_LICENSE.txt)
endif()
if(EXISTS "${_ntc_r8brain_dir}/LICENSE")
    install(FILES "${_ntc_r8brain_dir}/LICENSE" DESTINATION . RENAME R8BRAIN_LICENSE.txt)
elseif(EXISTS "${_ntc_r8brain_dir}/License.txt")
    install(FILES "${_ntc_r8brain_dir}/License.txt" DESTINATION . RENAME R8BRAIN_LICENSE.txt)
endif()

if(MSVC)
    # Explicit /WX- makes the release independent of runner/global warning-as-error settings.
    target_compile_options(NamToClo PRIVATE /W4 /WX- /permissive- /EHsc /utf-8 /fp:precise)
    set_property(TARGET NamToClo PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    if(TARGET ntc_namcore)
        target_compile_options(ntc_namcore PRIVATE /W3 /WX- /permissive- /EHsc /utf-8 /fp:precise)
        set_property(TARGET ntc_namcore PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    if(TARGET ntc_r8brain_base)
        target_compile_options(ntc_r8brain_base PRIVATE /W0 /WX- /permissive- /EHsc /utf-8 /fp:precise)
        set_property(TARGET ntc_r8brain_base PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

install(TARGETS NamToClo RUNTIME DESTINATION .)
install(FILES README.md LICENSE THIRD_PARTY.md DESTINATION .)
