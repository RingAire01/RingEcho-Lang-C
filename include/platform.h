#ifndef RE0_PLATFORM_H
#define RE0_PLATFORM_H

/* Keep host-specific path and executable rules in one place. */
#if defined(_WIN32) || defined(_WIN64)
#define RE0_PLATFORM_WINDOWS 1
#define RE0_PLATFORM_NAME "Windows"
#define RE0_PLATFORM_DEFAULT_C_COMPILER "gcc"
#define RE0_PLATFORM_EXECUTABLE_SUFFIX ".exe"
#define RE0_PLATFORM_PATH_SEPARATOR "\\"
#elif defined(__APPLE__)
#define RE0_PLATFORM_MACOS 1
#define RE0_PLATFORM_NAME "macOS"
#define RE0_PLATFORM_DEFAULT_C_COMPILER "clang"
#define RE0_PLATFORM_EXECUTABLE_SUFFIX ""
#define RE0_PLATFORM_PATH_SEPARATOR "/"
#elif defined(__linux__)
#define RE0_PLATFORM_LINUX 1
#define RE0_PLATFORM_NAME "Linux"
#define RE0_PLATFORM_DEFAULT_C_COMPILER "gcc"
#define RE0_PLATFORM_EXECUTABLE_SUFFIX ""
#define RE0_PLATFORM_PATH_SEPARATOR "/"
#else
#error "Unsupported RingEcho host platform"
#endif

#endif
