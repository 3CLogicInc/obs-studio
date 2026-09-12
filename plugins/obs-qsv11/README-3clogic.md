# obs-qsv11 (patched)

3CLogic patch on top of obs-studio 31.0.3 `plugins/obs-qsv11/` with one fix: upstream
never closes the Intel VPL session on encoder teardown, leaking the runtime's
~10-thread worker pool + ~40 MB per encoder instance (= per recording in our
per-session lifecycle). On Windows `ReleaseSessionData()` was an empty stub and
the `mfxLoader` a leaked local. Confirmed NOT an Intel runtime bug:
`qsv_leak_repro.cpp` loads libmfx64-gen.dll / libmfxhw64.dll directly and shows
the pool is fully destroyed when MFXClose is actually called.

## Patch (vs upstream 31.0.3)

- `common_utils_windows.cpp`: carry the loader out via the `sessionData`
  pointer (`win_session_data`); `ReleaseSessionData()` now does
  `MFXUnload` + `bfree`; unload the loader on `MFXCreateSession` failure.
- `QSV_Encoder_Internal.cpp` `ClearData()`: add the missing
  `MFXClose(m_session)` and close the session BEFORE `Release()` tears down
  the shared D3D11 device (upstream order destroyed the device while the
  session scheduler still referenced it).
- `msvc_shim/pthread.h`: SRWLOCK stand-in for OBS's w32-pthreads dep
  (the plugin only uses one static mutex).

## Build

Needs MSVC x64 + the Intel VPL dispatcher built static
(`git clone -b v2.15.0 https://github.com/intel/libvpl`,
`cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
-DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TOOLS=OFF`, target `VPL`).

From a VS x64 dev prompt (paths relative to the obs-studio checkout; <libobs-build> is a configured libobs build tree providing config\obsconfig.h and obs.lib):

```
cl /LD /MD /nologo /EHsc /O2 /W3 /Zi /std:c++17 ^
  /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE ^
  /I plugins\obs-qsv11\msvc_shim ^
  /I libobs ^
  /I <libobs-build>\config ^
  /I <libvpl>\api ^
  plugins\obs-qsv11\common_directx11.cpp ^
  plugins\obs-qsv11\common_utils.cpp ^
  plugins\obs-qsv11\common_utils_windows.cpp ^
  plugins\obs-qsv11\QSV_Encoder.cpp ^
  plugins\obs-qsv11\QSV_Encoder_Internal.cpp ^
  plugins\obs-qsv11\obs-qsv11.c ^
  plugins\obs-qsv11\obs-qsv11-plugin-main.c ^
  /Fe:obs-qsv11.dll /link /DEBUG:FULL ^
  <libobs-build>\libobs\Release\obs.lib ^
  <libvpl>\build\vpl.lib d3d11.lib dxgi.lib dxguid.lib advapi32.lib
```

Output dll+pdb ship in the 3CLogic Screen Recorder installer under `obs-plugins/64bit/`.

## Validation (2026-08-08/09 soak)

Old DLL: +10 threads / +40 MB per session, unbounded (idle self-restart at
80 threads every ~6 sessions). Patched: 166+ sessions overnight on one agent
process, threads oscillating 14-32 with zero growth, memory flat ~72-187 MB,
zero crash dumps, stops clean in ~0.45s. `qsv_leak_repro.cpp` documents the
Intel-side exoneration for the upstream report.
