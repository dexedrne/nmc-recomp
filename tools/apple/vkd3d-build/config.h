/* hand-written config.h for the KHNMC private build of vkd3d 2.1 (no autotools on the target Mac) */
#define PACKAGE_NAME "vkd3d"
#define PACKAGE_STRING "vkd3d 2.1"
#define PACKAGE_VERSION "2.1"
#define HAVE_ATOMIC_EXCHANGE_N 1
#define HAVE_BUILTIN_ADD_OVERFLOW 1
#define HAVE_BUILTIN_CLZ 1
#define HAVE_BUILTIN_CTZ 1
#define HAVE_BUILTIN_POPCOUNT 1
#define HAVE_SYNC_ADD_AND_FETCH 1
#define HAVE_SYNC_BOOL_COMPARE_AND_SWAP 1
#define HAVE_DLFCN_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_PTHREAD_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_LIBM 1
#define HAVE_SPIRV_UNIFIED1_SPIRV_H 1
#define HAVE_SPIRV_UNIFIED1_GLSL_STD_450_H 1
#ifdef __APPLE__
#define HAVE_PTHREAD_THREADID_NP 1
#define HAVE_PTHREAD_SETNAME_NP_1 1
#define SONAME_LIBVULKAN "libMoltenVK.dylib"
#else
#define HAVE_GETTID 1
#define HAVE_PTHREAD_SETNAME_NP_2 1
#define HAVE_DECL_PROGRAM_INVOCATION_NAME 1
#define SONAME_LIBVULKAN "libvulkan.so.1"
#endif
