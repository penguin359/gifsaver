/*
 * platform_win32.c - Windows platform layer for gifsaver.
 *
 * Copyright (c) 2026 Loren M. Lang
 * SPDX-License-Identifier: MIT
 *
 * A Windows screensaver (.scr) that tiles the decoded animation across
 * all monitors. Follows the standard screensaver command line:
 *
 *   gifsaver.scr /s [file.gif]    run full screen
 *   gifsaver.scr /p <hwnd>        preview (not supported yet)
 *   gifsaver.scr [/c[:<hwnd>]]    settings (not supported yet)
 *
 * The GIF is taken from the command line, else the GIFSAVER_GIF
 * environment variable, else (with GIFSAVER_EMBED_GIF) the built-in GIF.
 *
 * Requires: giflib (>= 5.0), provided by vcpkg; see vcpkg.json.
 */

#include "config.h"
#include "gifsaver.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

const char *progname = "gifsaver";

#define FRAME_TIMER 1
#define MOUSE_SLOP  4   /* pixels the mouse may drift before we quit */

static struct {
    HBRUSH *brushes;    /* one tiling pattern brush per frame */
    int *delays;        /* per-frame delay in ms */
    int nframes, cur;
    POINT mouse;        /* cursor position at startup */
} saver;

static void die(const char *msg)
{
    fprintf(stderr, "%s: %s\n", progname, msg);
    MessageBoxA(NULL, msg, progname, MB_OK | MB_ICONERROR);
    exit(1);
}

/* Turn one RGB canvas into a pattern brush that tiles it. */
static HBRUSH frame_to_brush(HDC hdc, const unsigned int *rgb, int w, int h)
{
    BITMAPINFO bmi;
    HBITMAP bm;
    HBRUSH br;

    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;            /* top-down rows */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    /* 0xRRGGBB as a little-endian DWORD is already BI_RGB's B,G,R,x. */
    bm = CreateDIBitmap(hdc, &bmi.bmiHeader, CBM_INIT, rgb, &bmi,
                        DIB_RGB_COLORS);
    if (!bm)
        die("CreateDIBitmap failed");
    br = CreatePatternBrush(bm);
    if (!br)
        die("CreatePatternBrush failed");
    return br;
}

static void upload_frames(const Animation *anim)
{
    HDC hdc = GetDC(NULL);
    int i;

    saver.nframes = anim->nframes;
    saver.brushes = calloc((size_t) anim->nframes, sizeof *saver.brushes);
    saver.delays = calloc((size_t) anim->nframes, sizeof *saver.delays);
    if (!saver.brushes || !saver.delays)
        die("out of memory");
    for (i = 0; i < anim->nframes; i++) {
        saver.delays[i] = anim->frames[i].delay_ms;
        saver.brushes[i] = frame_to_brush(hdc, anim->frames[i].rgb,
                                          anim->width, anim->height);
    }
    ReleaseDC(NULL, hdc);
}

static LRESULT CALLBACK saver_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        SetBrushOrgEx(hdc, 0, 0, NULL);
        FillRect(hdc, &ps.rcPaint, saver.brushes[saver.cur]);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        saver.cur = (saver.cur + 1) % saver.nframes;
        SetTimer(hwnd, FRAME_TIMER, (UINT) saver.delays[saver.cur], NULL);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_SETCURSOR:
        SetCursor(NULL);
        return TRUE;

    case WM_MOUSEMOVE: {
        POINT pt;
        GetCursorPos(&pt);
        if (abs(pt.x - saver.mouse.x) > MOUSE_SLOP ||
            abs(pt.y - saver.mouse.y) > MOUSE_SLOP)
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_MOUSEWHEEL:
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0;

    case WM_ACTIVATEAPP:
        if (!wp)
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        break;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_SCREENSAVE)
            return 0;   /* already running */
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void run_fullscreen(void)
{
    HINSTANCE hinst = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    HWND hwnd;
    MSG m;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = saver_proc;
    wc.hInstance = hinst;
    wc.lpszClassName = "gifsaver";
    if (!RegisterClassA(&wc))
        die("RegisterClass failed");

    GetCursorPos(&saver.mouse);
    hwnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                           "gifsaver", "gifsaver", WS_POPUP | WS_VISIBLE,
                           GetSystemMetrics(SM_XVIRTUALSCREEN),
                           GetSystemMetrics(SM_YVIRTUALSCREEN),
                           GetSystemMetrics(SM_CXVIRTUALSCREEN),
                           GetSystemMetrics(SM_CYVIRTUALSCREEN),
                           NULL, NULL, hinst, NULL);
    if (!hwnd)
        die("cannot create screensaver window");
    SetForegroundWindow(hwnd);

    if (saver.nframes > 1)
        SetTimer(hwnd, FRAME_TIMER, (UINT) saver.delays[0], NULL);

    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}

int main(int argc, char **argv)
{
    Animation anim;
    const char *gif_file = NULL;
    const char *env;
    char mode = 'c';    /* no arguments means "settings" */
    int i, r;

    /* Let diagnostics reach the console when started from one. */
    if (AttachConsole(ATTACH_PARENT_PROCESS))
        freopen("CONOUT$", "w", stderr);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((a[0] == '/' || a[0] == '-') && a[1] &&
            (a[2] == '\0' || a[2] == ':')) {
            mode = (char) tolower((unsigned char) a[1]);
            if (mode == 'p' && a[2] == '\0' && i + 1 < argc)
                i++;    /* skip the preview window handle */
        } else {
            gif_file = a;
        }
    }

    if (mode == 'p')
        return 0;
    if (mode != 's') {
        MessageBoxA(NULL,
                    "gifsaver has no settings dialog yet.\n\n"
                    "Set the GIFSAVER_GIF environment variable to the "
                    "path of an animated GIF to display.",
                    progname, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (!gif_file) {
        env = getenv("GIFSAVER_GIF");
        if (env && *env)
            gif_file = env;
    }

    /* Draw at native resolution instead of being bitmap-stretched. */
    SetProcessDPIAware();

#if GIFSAVER_EMBED_GIF
    r = gif_file ? anim_load_file(gif_file, -1, &anim)
                 : anim_load_builtin(-1, &anim);
#else
    if (!gif_file)
        die("no GIF given: pass file.gif or set GIFSAVER_GIF");
    r = anim_load_file(gif_file, -1, &anim);
#endif
    if (r < 0) {
        char msg[MAX_PATH + 64];
        snprintf(msg, sizeof msg, "cannot load %s",
                 gif_file ? gif_file : "built-in GIF");
        die(msg);
    }

    upload_frames(&anim);
    anim_free(&anim);           /* pixel data lives in GDI bitmaps now */

    run_fullscreen();
    return 0;
}
