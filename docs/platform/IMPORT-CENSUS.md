# Import census of kh2fm.exe (368 imports, 35 DLLs)

Static sweep, no code executed. Counts are call sites (direct `call [iat]`, calls through the 382 import stubs, tail jumps, address-taken references). `S3` is the earlier count in `s3-eh-fibers/import_callsites.json`; where it differs the S3 sweep missed leaf functions without .pdata and stub-routed sites (+267 sites over 58 imports).

| DLL | imports | sites | unreferenced |
|---|---|---|---|
| advapi32.dll | 4 | 5 | 0 |
| api-ms-win-crt-convert-l1-1-0.dll | 5 | 202 | 0 |
| api-ms-win-crt-filesystem-l1-1-0.dll | 2 | 9 | 0 |
| api-ms-win-crt-heap-l1-1-0.dll | 10 | 520 | 0 |
| api-ms-win-crt-locale-l1-1-0.dll | 1 | 1 | 0 |
| api-ms-win-crt-math-l1-1-0.dll | 33 | 951 | 0 |
| api-ms-win-crt-runtime-l1-1-0.dll | 29 | 135 | 0 |
| api-ms-win-crt-stdio-l1-1-0.dll | 26 | 271 | 0 |
| api-ms-win-crt-string-l1-1-0.dll | 17 | 358 | 0 |
| api-ms-win-crt-time-l1-1-0.dll | 6 | 19 | 0 |
| api-ms-win-crt-utility-l1-1-0.dll | 4 | 90 | 0 |
| d3d11.dll | 1 | 1 | 0 |
| d3d12.dll | 2 | 3 | 0 |
| d3dcompiler_47.dll | 1 | 51 | 0 |
| dbghelp.dll | 1 | 1 | 0 |
| dinput8.dll | 1 | 1 | 0 |
| dxgi.dll | 1 | 3 | 0 |
| gdi32.dll | 10 | 296 | 0 |
| hid.dll | 11 | 62 | 0 |
| imagehlp.dll | 1 | 1 | 0 |
| kernel32.dll | 94 | 3175 | 0 |
| mfplat.dll | 6 | 10 | 0 |
| mfreadwrite.dll | 1 | 1 | 0 |
| mscoree.dll | 1 | 0 | 1 |
| msvcp140.dll | 11 | 69 | 0 |
| ole32.dll | 8 | 32 | 0 |
| oleaut32.dll | 5 | 6 | 3 |
| setupapi.dll | 4 | 5 | 0 |
| shell32.dll | 2 | 4 | 0 |
| steam_api64.dll | 9 | 60 | 0 |
| user32.dll | 29 | 67 | 7 |
| vcruntime140.dll | 21 | 1337 | 3 |
| winmm.dll | 6 | 33 | 0 |
| xaudio2_9.dll | 3 | 3 | 0 |
| xinput1_4.dll | 2 | 3 | 0 |
| **total** | **368** | **7785** | **14** |


## advapi32.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| RegCloseKey | 2 | 2 | 2 | registry | stub: registry key store -> a small key/value file (3 functions, 5 sites, all in axa-platform 0x148460..0x1485 |
| RegCreateKeyExA | 1 | 1 | 1 | registry | stub: registry key store -> a small key/value file (3 functions, 5 sites, all in axa-platform 0x148460..0x1485 |
| RegQueryValueExA | 1 | 1 | 1 | registry | stub: registry key store -> a small key/value file (3 functions, 5 sites, all in axa-platform 0x148460..0x1485 |
| RegSetValueExA | 1 | 1 | 1 | registry | stub: registry key store -> a small key/value file (3 functions, 5 sites, all in axa-platform 0x148460..0x1485 |

## api-ms-win-crt-convert-l1-1-0.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| atof | 125 | 17 | 125 | crt | libc |
| atoi | 71 | 20 | 71 | crt | libc |
| mbstowcs | 3 | 3 | 3 | crt | libc |
| wcstombs_s | 1 | 1 | 1 | crt | libc |
| wcstoul | 2 | 1 | 2 | crt | libc |

## api-ms-win-crt-filesystem-l1-1-0.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| _chdir | 2 | 2 | 2 | crt | POSIX mkdir/chdir |
| _mkdir | 7 | 7 | 7 | crt | POSIX mkdir/chdir |

## api-ms-win-crt-heap-l1-1-0.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| _aligned_free | 74 | 62 | 66 | crt | libc malloc family |
| _aligned_malloc | 50 | 42 | 49 | crt | libc malloc family |
| _aligned_realloc | 2 | 2 | 2 | crt | libc malloc family |
| _callnewh | 1 | 1 | 1 | crt | libc malloc family |
| _recalloc | 1 | 1 | 1 | crt | libc malloc family |
| _set_new_mode | 1 | 1 | 1 | crt | libc malloc family |
| calloc | 43 | 20 | 43 | crt | libc malloc family |
| free | 214 | 80 | 207 | crt | libc malloc family |
| malloc | 116 | 60 | 113 | crt | libc malloc family |
| realloc | 18 | 15 | 18 | crt | libc malloc family |

## api-ms-win-crt-locale-l1-1-0.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| _configthreadlocale | 1 | 1 | 1 | crt | no-op |

## api-ms-win-crt-math-l1-1-0.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| __setusermatherr | 1 | 1 | 1 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| _finite | 3 | 3 | 3 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| _isnan | 172 | 114 | 172 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| acos | 2 | 1 | 2 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| acosf | 8 | 8 | 8 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| asinf | 3 | 3 | 3 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| atan | 24 | 7 | 24 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| atan2f | 103 | 68 | 101 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| atanf | 15 | 10 | 14 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| ceil | 13 | 4 | 13 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| ceilf | 8 | 5 | 8 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| cos | 9 | 6 | 9 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| cosf | 136 | 101 | 134 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| exp | 8 | 3 | 8 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| expf | 21 | 7 | 20 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| floor | 36 | 23 | 36 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| floorf | 37 | 15 | 37 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| fmodf | 19 | 11 | 19 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| frexp | 1 | 1 | 1 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| ldexp | 6 | 4 | 6 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| log | 12 | 4 | 12 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| log10f | 5 | 5 | 4 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| logf | 3 | 3 | 3 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| modf | 1 | 1 | 1 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| modff | 4 | 2 | 4 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| pow | 4 | 4 | 4 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| powf | 19 | 16 | 18 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| roundf | 1 | 1 | 1 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| sin | 9 | 6 | 9 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| sinf | 146 | 107 | 144 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| sqrt | 34 | 6 | 34 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| sqrtf | 68 | 62 | 61 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |
| tanf | 20 | 20 | 20 | crt | libm (bit-exactness caveat: UCRT sinf/cosf/atan2f 365 sites, see G1 fidelity limits) |

## api-ms-win-crt-runtime-l1-1-0.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| _beginthread | 6 | 4 | 6 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _beginthreadex | 4 | 3 | 4 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _c_exit | 1 | 1 | 1 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _cexit | 2 | 2 | 2 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _configure_narrow_argv | 3 | 3 | 3 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _crt_at_quick_exit | 1 | 1 | None | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _crt_atexit | 1 | 1 | 1 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _endthread | 7 | 4 | 7 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _endthreadex | 3 | 3 | 3 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _errno | 10 | 6 | 10 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _execute_onexit_table | 1 | 1 | 1 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _exit | 1 | 1 | 1 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _get_errno | 1 | 1 | 1 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _get_narrow_winmain_command_line | 2 | 2 | 2 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _initialize_narrow_environment | 3 | 3 | 2 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _initialize_onexit_table | 2 | 1 | 2 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _initterm | 1 | 1 | 1 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _initterm_e | 1 | 1 | 1 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _invalid_parameter_noinfo | 6 | 5 | 6 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _invalid_parameter_noinfo_noreturn | 62 | 54 | 62 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _register_onexit_function | 2 | 2 | 1 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _register_thread_local_exe_atexit_callback | 1 | 1 | 1 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _seh_filter_dll | 1 | 1 | 1 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _seh_filter_exe | 1 | 1 | 1 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| _set_app_type | 2 | 2 | 1 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| abort | 3 | 2 | 3 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| exit | 3 | 3 | 3 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| perror | 1 | 1 | 1 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |
| terminate | 3 | 3 | 3 | crt | runtime owns startup: _initterm/_initterm_e/_register_onexit/_set_app_type/_configure_narrow_argv/_get_narrow_ |

## api-ms-win-crt-stdio-l1-1-0.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| __acrt_iob_func | 9 | 6 | 9 | crt | POSIX/stdio with path mapping |
| __p__commode | 2 | 2 | 2 | crt | POSIX/stdio with path mapping |
| __stdio_common_vfprintf | 3 | 3 | 3 | crt | POSIX/stdio with path mapping |
| __stdio_common_vsnprintf_s | 2 | 2 | 2 | crt | POSIX/stdio with path mapping |
| __stdio_common_vsprintf | 9 | 9 | 9 | crt | POSIX/stdio with path mapping |
| __stdio_common_vsprintf_s | 2 | 2 | 2 | crt | POSIX/stdio with path mapping |
| __stdio_common_vswprintf | 4 | 4 | 4 | crt | POSIX/stdio with path mapping |
| __stdio_common_vswprintf_s | 3 | 3 | 3 | crt | POSIX/stdio with path mapping |
| __stdio_common_vswscanf | 1 | 1 | 1 | crt | POSIX/stdio with path mapping |
| _close | 32 | 28 | 31 | crt | POSIX/stdio with path mapping |
| _lseeki64 | 53 | 29 | 52 | crt | POSIX/stdio with path mapping |
| _read | 34 | 27 | 33 | crt | POSIX/stdio with path mapping |
| _set_fmode | 2 | 2 | 2 | crt | POSIX/stdio with path mapping |
| _sopen_dispatch | 19 | 11 | 19 | crt | POSIX/stdio with path mapping |
| _wfopen_s | 2 | 1 | 2 | crt | POSIX/stdio with path mapping |
| _write | 7 | 7 | 6 | crt | POSIX/stdio with path mapping |
| fclose | 19 | 14 | 18 | crt | POSIX/stdio with path mapping |
| fflush | 5 | 5 | 4 | crt | POSIX/stdio with path mapping |
| fgets | 4 | 2 | 4 | crt | POSIX/stdio with path mapping |
| fopen | 13 | 9 | 13 | crt | POSIX/stdio with path mapping |
| fopen_s | 4 | 2 | 4 | crt | POSIX/stdio with path mapping |
| fputc | 4 | 4 | 4 | crt | POSIX/stdio with path mapping |
| fread | 3 | 3 | 3 | crt | POSIX/stdio with path mapping |
| fseek | 5 | 3 | 5 | crt | POSIX/stdio with path mapping |
| ftell | 2 | 2 | 2 | crt | POSIX/stdio with path mapping |
| fwrite | 28 | 10 | 28 | crt | POSIX/stdio with path mapping |

## api-ms-win-crt-string-l1-1-0.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| _strnicmp | 42 | 1 | 42 | crt | libc |
| isdigit | 14 | 5 | 14 | crt | libc |
| isprint | 1 | 1 | 1 | crt | libc |
| strcat_s | 3 | 3 | 2 | crt | libc |
| strcmp | 170 | 27 | 170 | crt | libc |
| strcpy_s | 9 | 9 | 9 | crt | libc |
| strncat | 4 | 4 | 4 | crt | libc |
| strncmp | 56 | 44 | 56 | crt | libc |
| strncpy | 35 | 26 | 35 | crt | libc |
| strncpy_s | 1 | 1 | 1 | crt | libc |
| tolower | 1 | 1 | 1 | crt | libc |
| toupper | 7 | 4 | 7 | crt | libc |
| towlower | 1 | 1 | 1 | crt | libc |
| wcsncmp | 1 | 1 | 1 | crt | libc |
| wcsncpy | 4 | 4 | 3 | crt | libc |
| wcsncpy_s | 3 | 3 | 3 | crt | libc |
| wcstok | 6 | 4 | 6 | crt | libc |

## api-ms-win-crt-time-l1-1-0.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| _difftime64 | 1 | 1 | 1 | crt | libc time |
| _gmtime64 | 1 | 1 | 1 | crt | libc time |
| _localtime64 | 4 | 2 | 4 | crt | libc time |
| _mktime64 | 3 | 2 | 3 | crt | libc time |
| _time64 | 3 | 2 | 3 | crt | libc time |
| clock | 7 | 6 | 7 | crt | libc time |

## api-ms-win-crt-utility-l1-1-0.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| bsearch | 53 | 46 | 53 | crt | libc (rand/srand: sequence is implementation-defined |
| qsort | 16 | 15 | 16 | crt | libc (rand/srand: sequence is implementation-defined |
| rand | 18 | 12 | 18 | crt | libc (rand/srand: sequence is implementation-defined |
| srand | 3 | 3 | 1 | crt | libc (rand/srand: sequence is implementation-defined |

## d3d11.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| D3D11CreateDevice | 1 | 1 | 1 | gfx-movie | replace: movie decode path -> AVFoundation (graphics-spec/MOVIES.md) |

## d3d12.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| D3D12SerializeRootSignature | 1 | 1 | 1 | gfx | implement: D3D12-on-Metal shim (graphics-spec/D3D12-SHIM.md) |
| ord101 | 2 | 1 | 2 | gfx | implement: D3D12-on-Metal shim (graphics-spec/D3D12-SHIM.md) |

## d3dcompiler_47.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| D3DStripShader | 51 | 2 | 51 | gfx | D3DStripShader: 51 sites |

## dbghelp.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| MiniDumpWriteDump | 1 | 1 | 1 | crash | stub: MiniDumpWriteDump (WinMain filter) -> no-op |

## dinput8.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| DirectInput8Create | 1 | 1 | 1 | input | replace by GameController + keyboard (03-INPUT.md) |

## dxgi.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| CreateDXGIFactory1 | 3 | 3 | 3 | gfx | implement in shim |

## gdi32.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| CreateFontIndirectA | 8 | 8 | 8 | dev-only | drop: all 296 sites are in the sd Layout debug-draw classes (0x446f0..) |
| CreatePen | 23 | 15 | 23 | dev-only | drop: all 296 sites are in the sd Layout debug-draw classes (0x446f0..) |
| DeleteObject | 45 | 15 | 45 | dev-only | drop: all 296 sites are in the sd Layout debug-draw classes (0x446f0..) |
| Ellipse | 21 | 10 | 21 | dev-only | drop: all 296 sites are in the sd Layout debug-draw classes (0x446f0..) |
| GetStockObject | 14 | 14 | 14 | dev-only | drop: all 296 sites are in the sd Layout debug-draw classes (0x446f0..) |
| GetTextExtentPoint32A | 11 | 11 | 11 | dev-only | drop: all 296 sites are in the sd Layout debug-draw classes (0x446f0..) |
| LineTo | 41 | 15 | 41 | dev-only | drop: all 296 sites are in the sd Layout debug-draw classes (0x446f0..) |
| MoveToEx | 29 | 15 | 29 | dev-only | drop: all 296 sites are in the sd Layout debug-draw classes (0x446f0..) |
| SelectObject | 90 | 15 | 90 | dev-only | drop: all 296 sites are in the sd Layout debug-draw classes (0x446f0..) |
| TextOutA | 14 | 13 | 14 | dev-only | drop: all 296 sites are in the sd Layout debug-draw classes (0x446f0..) |

## hid.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| HidD_FreePreparsedData | 26 | 7 | 26 | input | drop: raw DualShock4 driver 0x448000 cluster |
| HidD_GetAttributes | 1 | 1 | 1 | input | drop: raw DualShock4 driver 0x448000 cluster |
| HidD_GetFeature | 10 | 5 | 10 | input | drop: raw DualShock4 driver 0x448000 cluster |
| HidD_GetHidGuid | 1 | 1 | 1 | input | drop: raw DualShock4 driver 0x448000 cluster |
| HidD_GetManufacturerString | 2 | 1 | 2 | input | drop: raw DualShock4 driver 0x448000 cluster |
| HidD_GetPreparsedData | 7 | 7 | 7 | input | drop: raw DualShock4 driver 0x448000 cluster |
| HidD_GetProductString | 2 | 1 | 2 | input | drop: raw DualShock4 driver 0x448000 cluster |
| HidD_GetSerialNumberString | 2 | 1 | 2 | input | drop: raw DualShock4 driver 0x448000 cluster |
| HidD_SetFeature | 2 | 2 | 2 | input | drop: raw DualShock4 driver 0x448000 cluster |
| HidP_GetCaps | 7 | 7 | 7 | input | drop: raw DualShock4 driver 0x448000 cluster |
| HidP_GetValueCaps | 2 | 2 | 2 | input | drop: raw DualShock4 driver 0x448000 cluster |

## imagehlp.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| MakeSureDirectoryPathExists | 1 | 1 | 1 | fs | implement MakeSureDirectoryPathExists = mkdir -p |

## kernel32.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| CloseHandle | 65 | 48 | 59 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| ConvertFiberToThread | 2 | 2 | 2 | kernel32:fiber | implement: stackful coroutines (G1 decision) |
| ConvertThreadToFiber | 2 | 2 | 2 | kernel32:fiber | implement: stackful coroutines (G1 decision) |
| CopyFileA | 4 | 4 | 3 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| CreateDirectoryA | 1 | 1 | 1 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| CreateEventA | 1 | 1 | 1 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| CreateEventExA | 2 | 2 | 2 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| CreateEventW | 5 | 5 | 5 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| CreateFiber | 1 | 1 | 1 | kernel32:fiber | implement: stackful coroutines (G1 decision) |
| CreateFileA | 1 | 1 | 1 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| CreateFileMappingA | 1 | 1 | 1 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| CreateFileW | 2 | 2 | 2 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| CreateMutexA | 1 | 1 | None | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| CreateSemaphoreA | 3 | 3 | 3 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| CreateSemaphoreW | 3 | 3 | 3 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| CreateThread | 2 | 2 | 2 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| CreateToolhelp32Snapshot | 2 | 2 | 2 | kernel32:launcher | drop (WaitTitleProject.exe launcher control, 0x1042e0/0x1046e0) |
| DecodePointer | 1 | 1 | 1 | kernel32:mem | implement: malloc/aligned_alloc |
| DeleteCriticalSection | 79 | 60 | 55 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| DeleteFiber | 1 | 1 | 1 | kernel32:fiber | implement: stackful coroutines (G1 decision) |
| DeleteFileA | 6 | 4 | 5 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| EnterCriticalSection | 1110 | 727 | 1102 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| ExitProcess | 3 | 3 | 3 | kernel32:eh | terminate/stub (G1 decision) |
| FileTimeToLocalFileTime | 2 | 1 | 2 | kernel32:time | implement: mach_absolute_time / clock_gettime |
| FileTimeToSystemTime | 2 | 1 | 2 | kernel32:time | implement: mach_absolute_time / clock_gettime |
| FindClose | 8 | 8 | 8 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| FindFirstFileA | 9 | 9 | 9 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| FindNextFileA | 5 | 5 | 5 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| FormatMessageA | 4 | 3 | 4 | kernel32:debug | stub: log to os_log/no-op |
| GetCurrentProcess | 4 | 4 | 4 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| GetCurrentProcessId | 3 | 3 | 3 | kernel32:loader | resolve against the runtime import table |
| GetCurrentThread | 3 | 3 | 3 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| GetCurrentThreadId | 8 | 8 | 8 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| GetDiskFreeSpaceExA | 1 | 1 | 1 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| GetExitCodeProcess | 2 | 2 | 2 | kernel32:eh | terminate/stub (G1 decision) |
| GetExitCodeThread | 10 | 4 | 10 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| GetLastError | 24 | 14 | 24 | kernel32:debug | stub: log to os_log/no-op |
| GetModuleFileNameA | 3 | 3 | 3 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| GetModuleHandleW | 5 | 3 | 5 | kernel32:loader | resolve against the runtime import table |
| GetProcAddress | 10 | 4 | 10 | kernel32:loader | resolve against the runtime import table |
| GetProcessAffinityMask | 2 | 2 | 2 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| GetProcessHeap | 10 | 2 | 10 | kernel32:mem | implement: malloc/aligned_alloc |
| GetStartupInfoW | 1 | 1 | 1 | kernel32:loader | resolve against the runtime import table |
| GetSystemTimeAsFileTime | 2 | 2 | 2 | kernel32:time | implement: mach_absolute_time / clock_gettime |
| GetThreadPriority | 5 | 5 | 5 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| GlobalMemoryStatus | 2 | 2 | 2 | kernel32:mem | implement: malloc/aligned_alloc |
| HeapAlloc | 5 | 2 | 5 | kernel32:mem | implement: malloc/aligned_alloc |
| HeapFree | 5 | 2 | 5 | kernel32:mem | implement: malloc/aligned_alloc |
| InitializeCriticalSection | 60 | 42 | 60 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| InitializeCriticalSectionAndSpinCount | 2 | 2 | 2 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| InitializeCriticalSectionEx | 1 | 1 | None | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| InitializeSListHead | 1 | 1 | None | kernel32:mem | implement: malloc/aligned_alloc |
| IsDebuggerPresent | 2 | 2 | 2 | kernel32:eh | terminate/stub (G1 decision) |
| IsProcessorFeaturePresent | 4 | 4 | 4 | kernel32:eh | terminate/stub (G1 decision) |
| LeaveCriticalSection | 1270 | 729 | 1256 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| LoadLibraryExW | 2 | 2 | 2 | kernel32:loader | resolve against the runtime import table |
| MapViewOfFile | 1 | 1 | 1 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| MultiByteToWideChar | 2 | 2 | 2 | kernel32:str | implement |
| OpenProcess | 2 | 2 | 2 | kernel32:launcher | drop (WaitTitleProject.exe launcher control, 0x1042e0/0x1046e0) |
| OpenThread | 5 | 5 | 5 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| OutputDebugStringA | 39 | 39 | None | kernel32:debug | stub: log to os_log/no-op |
| OutputDebugStringW | 1 | 1 | 1 | kernel32:debug | stub: log to os_log/no-op |
| Process32First | 2 | 2 | 2 | kernel32:launcher | drop (WaitTitleProject.exe launcher control, 0x1042e0/0x1046e0) |
| Process32Next | 2 | 2 | 2 | kernel32:launcher | drop (WaitTitleProject.exe launcher control, 0x1042e0/0x1046e0) |
| QueryPerformanceCounter | 73 | 55 | 73 | kernel32:time | implement: mach_absolute_time / clock_gettime |
| QueryPerformanceFrequency | 48 | 44 | 48 | kernel32:time | implement: mach_absolute_time / clock_gettime |
| RaiseException | 2 | 2 | 1 | kernel32:eh | terminate/stub (G1 decision) |
| ReadFile | 2 | 2 | 2 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| ReleaseMutex | 37 | 34 | None | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| ReleaseSemaphore | 10 | 10 | 4 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| RemoveDirectoryA | 1 | 1 | 1 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| ResetEvent | 5 | 5 | 5 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| ResumeThread | 3 | 3 | 2 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| RtlCaptureContext | 3 | 3 | 3 | kernel32:eh | terminate/stub (G1 decision) |
| RtlLookupFunctionEntry | 3 | 3 | 3 | kernel32:eh | terminate/stub (G1 decision) |
| RtlVirtualUnwind | 3 | 3 | 3 | kernel32:eh | terminate/stub (G1 decision) |
| SetEvent | 6 | 6 | 6 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| SetThreadIdealProcessor | 1 | 1 | 1 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| SetThreadPriority | 13 | 8 | 7 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| SetUnhandledExceptionFilter | 3 | 3 | 2 | kernel32:eh | terminate/stub (G1 decision) |
| Sleep | 48 | 36 | 16 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| SleepEx | 7 | 6 | 4 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| SuspendThread | 1 | 1 | None | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| SwitchToFiber | 4 | 2 | 4 | kernel32:fiber | implement: stackful coroutines (G1 decision) |
| SwitchToThread | 2 | 2 | 2 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| TerminateProcess | 3 | 3 | 3 | kernel32:eh | terminate/stub (G1 decision) |
| TerminateThread | 3 | 1 | 3 | kernel32:thread | implement: pthread create/join + per-thread TLS block |
| UnhandledExceptionFilter | 2 | 2 | 2 | kernel32:eh | terminate/stub (G1 decision) |
| UnmapViewOfFile | 1 | 1 | 1 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| WaitForSingleObject | 36 | 27 | 32 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| WaitForSingleObjectEx | 1 | 1 | 1 | kernel32:sync | implement: pthread (recursive mutex for CRITICAL_SECTION, cond+mutex for events, dispatch semaphore for semaph |
| WideCharToMultiByte | 1 | 1 | 1 | kernel32:str | implement |
| WriteFile | 1 | 1 | 1 | kernel32:fs | implement: POSIX with game-path mapping (01-FILE-LAYER.md) |
| lstrlenA | 23 | 13 | 23 | kernel32:str | implement |

## mfplat.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| MFCreateAttributes | 1 | 1 | 1 | movie | replace: AVFoundation |
| MFCreateDXGIDeviceManager | 1 | 1 | 1 | movie | replace: AVFoundation |
| MFCreateMediaType | 2 | 2 | 2 | movie | replace: AVFoundation |
| MFShutdown | 1 | 1 | None | movie | replace: AVFoundation |
| MFStartup | 1 | 1 | 1 | movie | replace: AVFoundation |
| MFllMulDiv | 4 | 4 | 4 | movie | replace: AVFoundation |

## mfreadwrite.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| MFCreateSourceReaderFromURL | 1 | 1 | 1 | movie | replace: AVFoundation |

## mscoree.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| _CorExeMain | 0 | 0 | None | startup | _CorExeMain only (entry thunk, 0 call sites): loader never executes it |

## msvcp140.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| ?_Throw_C_error@std@@YAXH@Z | 9 | 5 | 9 | crt-cpp | implement: _Mtx_* -> recursive pthread mutex |
| ?_Xbad_function_call@std@@YAXXZ | 10 | 8 | 10 | crt-cpp | implement: _Mtx_* -> recursive pthread mutex |
| ?_Xlength_error@std@@YAXPEBD@Z | 32 | 32 | 32 | crt-cpp | implement: _Mtx_* -> recursive pthread mutex |
| ?__ExceptionPtrCopy@@YAXPEAXPEBX@Z | 1 | 1 | 1 | crt-cpp | implement: _Mtx_* -> recursive pthread mutex |
| ?__ExceptionPtrDestroy@@YAXPEAX@Z | 1 | 1 | None | crt-cpp | implement: _Mtx_* -> recursive pthread mutex |
| _Mtx_destroy_in_situ | 2 | 2 | 1 | crt-cpp | implement: _Mtx_* -> recursive pthread mutex |
| _Mtx_init_in_situ | 1 | 1 | 1 | crt-cpp | implement: _Mtx_* -> recursive pthread mutex |
| _Mtx_lock | 4 | 4 | 4 | crt-cpp | implement: _Mtx_* -> recursive pthread mutex |
| _Mtx_unlock | 5 | 5 | 5 | crt-cpp | implement: _Mtx_* -> recursive pthread mutex |
| _Thrd_sleep | 2 | 2 | 2 | crt-cpp | implement: _Mtx_* -> recursive pthread mutex |
| _Xtime_get_ticks | 2 | 2 | 2 | crt-cpp | implement: _Mtx_* -> recursive pthread mutex |

## ole32.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| CoCreateInstance | 3 | 3 | 3 | com | stub: CoInitialize*/CoUninitialize no-op |
| CoInitialize | 2 | 2 | 2 | com | stub: CoInitialize*/CoUninitialize no-op |
| CoInitializeEx | 1 | 1 | 1 | com | stub: CoInitialize*/CoUninitialize no-op |
| CoSetProxyBlanket | 1 | 1 | 1 | com | stub: CoInitialize*/CoUninitialize no-op |
| CoTaskMemAlloc | 5 | 4 | 5 | com | stub: CoInitialize*/CoUninitialize no-op |
| CoTaskMemFree | 12 | 9 | 12 | com | stub: CoInitialize*/CoUninitialize no-op |
| CoUninitialize | 6 | 6 | 6 | com | stub: CoInitialize*/CoUninitialize no-op |
| PropVariantClear | 2 | 2 | 2 | com | stub: CoInitialize*/CoUninitialize no-op |

## oleaut32.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| SysAllocString | 3 | 1 | 3 | com | stub (SysAllocString/SysFreeString -> malloc |
| SysAllocStringByteLen | 0 | 0 | None | com | stub (SysAllocString/SysFreeString -> malloc |
| SysFreeString | 3 | 1 | 3 | com | stub (SysAllocString/SysFreeString -> malloc |
| SysStringByteLen | 0 | 0 | None | com | stub (SysAllocString/SysFreeString -> malloc |
| VariantCopy | 0 | 0 | None | com | stub (SysAllocString/SysFreeString -> malloc |

## setupapi.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| SetupDiDestroyDeviceInfoList | 1 | 1 | 1 | input | drop (same cluster) |
| SetupDiEnumDeviceInterfaces | 1 | 1 | 1 | input | drop (same cluster) |
| SetupDiGetClassDevsW | 1 | 1 | 1 | input | drop (same cluster) |
| SetupDiGetDeviceInterfaceDetailW | 2 | 1 | 2 | input | drop (same cluster) |

## shell32.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| SHGetFolderPathA | 2 | 2 | 2 | os | SHGetFolderPathA -> app Documents dir |
| ShellExecuteA | 2 | 2 | 2 | os | SHGetFolderPathA -> app Documents dir |

## steam_api64.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| SteamAPI_GetHSteamUser | 6 | 6 | 6 | steam | facts only (04-STEAM-FACTS.md) |
| SteamAPI_Init | 1 | 1 | 1 | steam | facts only (04-STEAM-FACTS.md) |
| SteamAPI_RegisterCallback | 6 | 2 | 6 | steam | facts only (04-STEAM-FACTS.md) |
| SteamAPI_RestartAppIfNecessary | 1 | 1 | 1 | steam | facts only (04-STEAM-FACTS.md) |
| SteamAPI_RunCallbacks | 2 | 2 | 2 | steam | facts only (04-STEAM-FACTS.md) |
| SteamAPI_Shutdown | 1 | 1 | 1 | steam | facts only (04-STEAM-FACTS.md) |
| SteamAPI_UnregisterCallback | 15 | 11 | 12 | steam | facts only (04-STEAM-FACTS.md) |
| SteamInternal_ContextInit | 21 | 16 | 21 | steam | facts only (04-STEAM-FACTS.md) |
| SteamInternal_FindOrCreateUserInterface | 7 | 7 | 7 | steam | facts only (04-STEAM-FACTS.md) |

## user32.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| AdjustWindowRect | 4 | 4 | 4 | window | replace by native window shell |
| CallNextHookEx | 0 | 0 | None | window | replace by native window shell |
| CreateWindowExA | 1 | 1 | 1 | window | replace by native window shell |
| DefWindowProcA | 1 | 1 | 1 | window | replace by native window shell |
| DestroyWindow | 2 | 2 | 1 | window | replace by native window shell |
| EnumDisplaySettingsA | 1 | 1 | 1 | window | replace by native window shell |
| GetClientRect | 22 | 21 | 22 | window | replace by native window shell |
| GetCursorPos | 1 | 1 | 1 | window | replace by native window shell |
| GetForegroundWindow | 1 | 1 | 1 | window | replace by native window shell |
| GetMonitorInfoA | 4 | 4 | 4 | window | replace by native window shell |
| GetSystemMetrics | 2 | 1 | 2 | window | replace by native window shell |
| GetWindowLongPtrA | 3 | 3 | 3 | window | replace by native window shell |
| IsZoomed | 6 | 4 | 6 | window | replace by native window shell |
| LoadCursorA | 1 | 1 | 1 | window | replace by native window shell |
| LoadIconA | 0 | 0 | None | window | replace by native window shell |
| MonitorFromWindow | 4 | 4 | 4 | window | replace by native window shell |
| PostQuitMessage | 1 | 1 | 1 | window | replace by native window shell |
| RegisterClassExA | 1 | 1 | 1 | window | replace by native window shell |
| RegisterDeviceNotificationA | 0 | 0 | None | window | replace by native window shell |
| ScreenToClient | 1 | 1 | 1 | window | replace by native window shell |
| SendMessageA | 2 | 2 | 2 | window | replace by native window shell |
| SetDlgItemTextW | 0 | 0 | None | window | replace by native window shell |
| SetWindowLongPtrA | 1 | 1 | 1 | window | replace by native window shell |
| SetWindowLongPtrW | 2 | 2 | 2 | window | replace by native window shell |
| SetWindowPos | 4 | 3 | 4 | window | replace by native window shell |
| SetWindowsHookExA | 0 | 0 | None | window | replace by native window shell |
| ShowWindow | 2 | 2 | 2 | window | replace by native window shell |
| UnhookWindowsHookEx | 0 | 0 | None | window | replace by native window shell |
| UnregisterClassA | 0 | 0 | None | window | replace by native window shell |

## vcruntime140.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| _CxxThrowException | 76 | 76 | 76 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| __C_specific_handler | 0 | 0 | None | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| __CxxFrameHandler3 | 1 | 1 | 1 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| __FrameUnwindFilter | 0 | 0 | None | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| __intrinsic_setjmp | 2 | 2 | 2 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| __std_exception_copy | 3 | 3 | 3 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| __std_exception_destroy | 7 | 7 | 4 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| __std_terminate | 1 | 1 | None | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| __std_type_info_destroy_list | 1 | 1 | None | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| _purecall | 0 | 0 | None | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| longjmp | 3 | 3 | 3 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| memchr | 2 | 2 | 2 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| memcmp | 24 | 18 | 24 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| memcpy | 223 | 145 | 221 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| memmove | 38 | 25 | 38 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| memset | 646 | 460 | 637 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| strchr | 53 | 26 | 49 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| strrchr | 42 | 30 | 41 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| strstr | 202 | 87 | 201 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| wcschr | 4 | 4 | 4 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |
| wcsstr | 9 | 5 | 9 | crt-vc | memcpy/memset/memmove/memcmp/memchr/str* -> libc |

## winmm.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| timeBeginPeriod | 7 | 5 | 7 | time | implement: timeGetTime/timeBeginPeriod/timeSetEvent/timeKillEvent on a timer thread |
| timeEndPeriod | 9 | 6 | 9 | time | implement: timeGetTime/timeBeginPeriod/timeSetEvent/timeKillEvent on a timer thread |
| timeGetDevCaps | 5 | 5 | 5 | time | implement: timeGetTime/timeBeginPeriod/timeSetEvent/timeKillEvent on a timer thread |
| timeGetTime | 4 | 4 | 4 | time | implement: timeGetTime/timeBeginPeriod/timeSetEvent/timeKillEvent on a timer thread |
| timeKillEvent | 5 | 4 | 5 | time | implement: timeGetTime/timeBeginPeriod/timeSetEvent/timeKillEvent on a timer thread |
| timeSetEvent | 3 | 3 | 3 | time | implement: timeGetTime/timeBeginPeriod/timeSetEvent/timeKillEvent on a timer thread |

## xaudio2_9.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| ord1 | 1 | 1 | 1 | audio | implement: XAudio2 shim over CoreAudio (02-AUDIO.md) |
| ord2 | 1 | 1 | 1 | audio | implement: XAudio2 shim over CoreAudio (02-AUDIO.md) |
| ord3 | 1 | 1 | 1 | audio | implement: XAudio2 shim over CoreAudio (02-AUDIO.md) |

## xinput1_4.dll

| function | sites | funcs | S3 | category | runtime v0 treatment |
|---|---|---|---|---|---|
| ord2 | 2 | 2 | 2 | input | replace by GameController (03-INPUT.md) |
| ord3 | 1 | 1 | 1 | input | replace by GameController (03-INPUT.md) |
