# bindings/android/jni/CMakeLists.txt
#
# JNI glue backing the Java/Kotlin binding (bindings/android/bison-lib) --
# only configured when cross-compiling with the Android NDK's CMake
# toolchain file (see the `if(ANDROID)` guard around this add_subdirectory()
# in the root CMakeLists.txt). Mirrors bindings/cpp's shape: a thin wrapper
# over the already-built bison_abi shared library, not a reimplementation of
# anything in it. See docs/bindings.md for the binding's API surface.

add_library(bison_jni SHARED
    jni_util.cpp
    bison_jni.cpp
    rmi_jni.cpp
)

target_include_directories(bison_jni PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}"
)

# The NDK sysroot provides <jni.h> and liblog.so (__android_log_*) without
# needing find_package(JNI) -- that's a host-JDK-only mechanism.
target_link_libraries(bison_jni PRIVATE bison_abi log)
