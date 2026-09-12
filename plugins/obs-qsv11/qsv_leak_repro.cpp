// Standalone attribution test for the per-session QSV thread leak.
// Loads an Intel media runtime DLL directly (as the dispatcher would),
// then loops [init session -> encode N frames -> close session] and prints
// process thread/handle/memory counts per cycle. No OBS involved:
// a climbing count here is Intel's runtime; flat means the leak lives in
// the OBS qsv plugin teardown instead.
//
//   qsv_leak_repro.exe <path-to-libmfx64-gen.dll|libmfxhw64.dll> [cycles] [frames]

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "vpl/mfxvideo.h"

typedef mfxStatus(MFX_CDECL* PFN_Initialize)(mfxInitializationParam, mfxSession*);
typedef mfxStatus(MFX_CDECL* PFN_InitEx)(mfxInitParam, mfxSession*);
typedef mfxStatus(MFX_CDECL* PFN_Close)(mfxSession);
typedef mfxStatus(MFX_CDECL* PFN_EncQuery)(mfxSession, mfxVideoParam*, mfxVideoParam*);
typedef mfxStatus(MFX_CDECL* PFN_EncInit)(mfxSession, mfxVideoParam*);
typedef mfxStatus(MFX_CDECL* PFN_EncClose)(mfxSession);
typedef mfxStatus(MFX_CDECL* PFN_EncFrame)(mfxSession, mfxEncodeCtrl*, mfxFrameSurface1*, mfxBitstream*, mfxSyncPoint*);
typedef mfxStatus(MFX_CDECL* PFN_Sync)(mfxSession, mfxSyncPoint, mfxU32);

static int threadCount() {
    int n = 0;
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    THREADENTRY32 te = { sizeof(te) };
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) n++;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

static void vitals(const char* tag, int cycle, int encoded) {
    PROCESS_MEMORY_COUNTERS pmc = {};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    printf("%-16s cycle=%2d encoded=%3d threads=%3d handles=%4lu wsMB=%zu\n",
           tag, cycle, encoded, threadCount(), handles, pmc.WorkingSetSize / (1024 * 1024));
    fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: %s <runtime-dll> [cycles] [frames]\n", argv[0]);
        return 2;
    }
    const char* dllPath = argv[1];
    const int cycles = argc > 2 ? atoi(argv[2]) : 10;
    const int frames = argc > 3 ? atoi(argv[3]) : 30;
    const bool legacy = strstr(dllPath, "hw64") != nullptr;

    HMODULE rt = LoadLibraryA(dllPath);
    if (!rt) {
        printf("LoadLibrary(%s) failed: %lu\n", dllPath, GetLastError());
        return 1;
    }
    printf("loaded %s (%s API)\n", dllPath, legacy ? "legacy MFXInitEx" : "VPL MFXInitialize");

    auto pInitialize = (PFN_Initialize)GetProcAddress(rt, "MFXInitialize");
    auto pInitEx = (PFN_InitEx)GetProcAddress(rt, "MFXInitEx");
    auto pClose = (PFN_Close)GetProcAddress(rt, "MFXClose");
    auto pEncInit = (PFN_EncInit)GetProcAddress(rt, "MFXVideoENCODE_Init");
    auto pEncClose = (PFN_EncClose)GetProcAddress(rt, "MFXVideoENCODE_Close");
    auto pEncFrame = (PFN_EncFrame)GetProcAddress(rt, "MFXVideoENCODE_EncodeFrameAsync");
    auto pSync = (PFN_Sync)GetProcAddress(rt, "MFXVideoCORE_SyncOperation");
    if (!pClose || !pEncInit || !pEncClose || !pEncFrame || !pSync || (!pInitialize && !pInitEx)) {
        printf("missing exports: Initialize=%p InitEx=%p Close=%p EncInit=%p EncClose=%p EncFrame=%p Sync=%p\n",
               (void*)pInitialize, (void*)pInitEx, (void*)pClose, (void*)pEncInit,
               (void*)pEncClose, (void*)pEncFrame, (void*)pSync);
        return 1;
    }

    vitals("baseline", 0, 0);

    const int W = 1280, H = 720;
    for (int c = 1; c <= cycles; c++) {
        mfxSession s = nullptr;
        mfxStatus sts;
        if (!legacy && pInitialize) {
            mfxInitializationParam ip = {};
            ip.AccelerationMode = MFX_ACCEL_MODE_VIA_D3D11;
            sts = pInitialize(ip, &s);
        } else {
            mfxInitParam ip = {};
            ip.Implementation = MFX_IMPL_HARDWARE_ANY | MFX_IMPL_VIA_D3D11;
            ip.Version.Major = 1;
            ip.Version.Minor = 0;
            sts = pInitEx(ip, &s);
        }
        if (sts != MFX_ERR_NONE) {
            printf("cycle %d: session init failed: %d\n", c, sts);
            return 1;
        }

        const bool cbr = getenv("REPRO_CBR") != nullptr;
        mfxVideoParam vp = {};
        vp.mfx.CodecId = MFX_CODEC_AVC;
        vp.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;
        if (cbr) {
            vp.mfx.RateControlMethod = MFX_RATECONTROL_CBR;
            vp.mfx.TargetKbps = 2500;
        } else {
            vp.mfx.RateControlMethod = MFX_RATECONTROL_CQP;
            vp.mfx.QPI = vp.mfx.QPP = vp.mfx.QPB = 26;
        }
        vp.mfx.GopPicSize = 30;
        vp.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
        vp.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
        vp.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
        vp.mfx.FrameInfo.Width = W;
        vp.mfx.FrameInfo.Height = H;
        vp.mfx.FrameInfo.CropW = W;
        vp.mfx.FrameInfo.CropH = H;
        vp.mfx.FrameInfo.FrameRateExtN = 30;
        vp.mfx.FrameInfo.FrameRateExtD = 1;
        vp.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;
        vp.AsyncDepth = getenv("REPRO_CBR") ? 4 : 1;

        sts = pEncInit(s, &vp);
        if (sts < MFX_ERR_NONE) {
            printf("cycle %d: encoder init failed: %d\n", c, sts);
            pClose(s);
            return 1;
        }

        // Small surface pool; reuse only unlocked surfaces (runtime may hold
        // a buffered frame past the EncodeFrameAsync call that submitted it).
        const int POOL = 8;
        std::vector<std::vector<mfxU8>> ybufs(POOL), uvbufs(POOL);
        std::vector<mfxFrameSurface1> surfs(POOL);
        for (int i = 0; i < POOL; i++) {
            ybufs[i].assign((size_t)W * H, 128);
            uvbufs[i].assign((size_t)W * H / 2, 128);
            memset(&surfs[i], 0, sizeof(surfs[i]));
            surfs[i].Info = vp.mfx.FrameInfo;
            surfs[i].Data.Y = ybufs[i].data();
            surfs[i].Data.UV = uvbufs[i].data();
            surfs[i].Data.Pitch = W;
        }

        std::vector<mfxU8> bsbuf(4 * 1024 * 1024);
        mfxBitstream bs = {};
        bs.Data = bsbuf.data();
        bs.MaxLength = (mfxU32)bsbuf.size();

        int encoded = 0;
        bool fatal = false;
        for (int f = 0; f < frames && !fatal; f++) {
            mfxFrameSurface1* surf = nullptr;
            for (int i = 0; i < POOL; i++) {
                if (!surfs[i].Data.Locked) { surf = &surfs[i]; break; }
            }
            if (!surf) { Sleep(5); f--; continue; }
            surf->Data.TimeStamp = (mfxU64)f * 90000 / 30;
            if (c <= 2 && f == frames / 2) vitals("mid-encode", c, encoded);

            for (;;) {
                mfxSyncPoint sp = nullptr;
                bs.DataLength = 0;
                bs.DataOffset = 0;
                sts = pEncFrame(s, nullptr, surf, &bs, &sp);
                if (sts == MFX_WRN_DEVICE_BUSY) { Sleep(1); continue; }
                if (sp) { pSync(s, sp, 5000); encoded++; }
                if (sts == MFX_ERR_MORE_DATA || sts >= MFX_ERR_NONE) break;
                printf("cycle %d frame %d: encode failed: %d\n", c, f, sts);
                fatal = true;
                break;
            }
        }
        for (; !fatal;) {  // drain
            mfxSyncPoint sp = nullptr;
            bs.DataLength = 0;
            bs.DataOffset = 0;
            sts = pEncFrame(s, nullptr, nullptr, &bs, &sp);
            if (sts == MFX_WRN_DEVICE_BUSY) { Sleep(1); continue; }
            if (sp) { pSync(s, sp, 5000); encoded++; }
            if (sts == MFX_ERR_MORE_DATA || sts < MFX_ERR_NONE) break;
        }

        pEncClose(s);
        pClose(s);
        Sleep(1500);  // grace period for runtime worker teardown
        vitals("after close", c, encoded);
    }

    vitals("final (loaded)", cycles, 0);
    FreeLibrary(rt);
    Sleep(1500);
    vitals("after unload", cycles, 0);
    return 0;
}
