#include <universal/q_shared.h>
#include "ui_shared.h"

#include <database/database.h>
#include <universal/profile.h>
#include <universal/com_files.h>

#ifdef KISAK_MP
#include <client_mp/client_mp.h>
#elif KISAK_SP
#include "ui.h"
#endif

double __cdecl UI_LoadBarProgress_LoadObj()
{
    float v2; // [esp+4h] [ebp-14h]
    double v3; // [esp+Ch] [ebp-Ch]
    float v4; // [esp+14h] [ebp-4h]

    if (com_expectedHunkUsage <= 0)
        return 0.0;
    v3 = (double)com_expectedHunkUsage;
    v4 = (double)Hunk_Used() / v3;
    v2 = v4 - 1.0;
    if (v2 < 0.0)
        return v4;
    else
        return (float)1.0;
}

void __cdecl UI_DrawHandlePic(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float w,
    float h,
    int horzAlign,
    int vertAlign,
    const float *color,
    Material *material)
{
    PROF_SCOPED("UI_DrawHandlePic");

    float t0; // [esp+30h] [ebp-10h]
    float t1; // [esp+34h] [ebp-Ch]
    float s1; // [esp+38h] [ebp-8h]
    float s0; // [esp+3Ch] [ebp-4h]

    if (w >= 0.0)
    {
        s0 = 0.0;
        s1 = 1.0;
    }
    else
    {
        w = -w;
        s0 = 1.0;
        s1 = 0.0;
    }
    if (h >= 0.0)
    {
        t0 = 0.0;
        t1 = 1.0;
    }
    else
    {
        h = -h;
        t0 = 1.0;
        t1 = 0.0;
    }
    CL_DrawStretchPic(scrPlace, x, y, w, h, horzAlign, vertAlign, s0, t0, s1, t1, color, material);
}

void __cdecl UI_DrawLoadBar(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float w,
    float h,
    int horzAlign,
    int vertAlign,
    const float *color,
    Material *material)
{
    float v9; // [esp+30h] [ebp-20h]
    double (*v10)(void); // [esp+34h] [ebp-1Ch]
    float percentDone; // [esp+40h] [ebp-10h]

    if (IsFastFileLoad())
        v10 = UI_LoadBarProgress_FastFile;
    else
        v10 = UI_LoadBarProgress_LoadObj;
    percentDone = v10();
#if defined(KISAK_SP) && defined(__SWITCH__)
    // switch_loadBarDiag 1: one LOADBAR line whenever the drawn fraction
    // moves by >= 1% (or goes backwards) and at least every 500 ms, with the
    // walk's raw counters, so a log shows what the bar did over the load.
    {
        static const dvar_t *loadBarDiag;
        if (!loadBarDiag)
            loadBarDiag = Dvar_FindVar("switch_loadBarDiag");
        if (loadBarDiag && loadBarDiag->current.enabled)
        {
            static float lastLogged = -1.0f;
            static int lastLoggedMs;
            const int now = Sys_Milliseconds();
            if (percentDone < lastLogged || percentDone - lastLogged >= 0.01f || now - lastLoggedMs >= 500)
            {
                uint64_t readBytes = 0, fileBytes = 0;
                uint32_t externalBytes = 0;
                const bool sized = FS_GetRetailLoadProgress(&readBytes, &fileBytes, &externalBytes);
                Com_Printf(CON_CHANNEL_UI, "LOADBAR t=%d frac=%.4f sized=%d read=%llu file=%llu external=%u\n", now,
                           percentDone, sized, (unsigned long long)readBytes, (unsigned long long)fileBytes,
                           externalBytes);
                lastLogged = percentDone;
                lastLoggedMs = now;
            }
        }
    }
#endif
    v9 = w * percentDone;
    CL_DrawStretchPic(scrPlace, x, y, v9, h, horzAlign, vertAlign, 0.0, 0.0, percentDone, 1.0, color, material);
}

double __cdecl UI_LoadBarProgress_FastFile()
{
    return DB_GetLoadedFraction();
}

void __cdecl UI_FillRectPhysical(float x, float y, float width, float height, const float *color)
{
    if (sharedUiInfo.assets.whiteMaterial)
        CL_DrawStretchPicPhysical(x, y, width, height, 0.0, 0.0, 0.0, 0.0, color, sharedUiInfo.assets.whiteMaterial);
}

void __cdecl UI_FillRect(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float width,
    float height,
    int horzAlign,
    int vertAlign,
    const float *color)
{
    if (sharedUiInfo.assets.whiteMaterial)
        CL_DrawStretchPic(
            scrPlace,
            x,
            y,
            width,
            height,
            horzAlign,
            vertAlign,
            0.0,
            0.0,
            0.0,
            0.0,
            color,
            sharedUiInfo.assets.whiteMaterial);
}

