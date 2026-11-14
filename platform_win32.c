/*
 * platform_win32.c - Windows platform layer for gifsaver.
 *
 * Copyright (c) 2026 Loren M. Lang
 * SPDX-License-Identifier: MIT
 *
 * A Windows screensaver (.scr) that tiles the decoded animation across
 * all monitors, optionally looping a WAV through waveOut. Follows the
 * standard screensaver command line:
 *
 *   gifsaver.scr /s [-sound file.wav] [file.gif]   run full screen
 *   gifsaver.scr -window [-sound file.wav] [file.gif]   test window
 *   gifsaver.scr /p <hwnd>        preview (not supported yet)
 *   gifsaver.scr [/c[:<hwnd>]]    settings dialog
 *
 * The GIF is taken from the command line, else the GIFSAVER_GIF
 * environment variable, else the settings dialog's choice (stored under
 * HKCU\Software\gifsaver), else (with GIFSAVER_EMBED_GIF) the built-in
 * GIF. Likewise the WAV comes from -sound, else GIFSAVER_SOUND, else
 * the settings, else (with GIFSAVER_EMBED_WAV) the built-in WAV.
 *
 * Requires: giflib (>= 5.0), provided by vcpkg; see vcpkg.json.
 * Optional: winmm for -sound; see config.h.
 */

#include "config.h"
#include "gifsaver.h"
#include "platform_win32.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

#if GIFSAVER_AUDIO
#include <mmsystem.h>
#include <mmreg.h>
#endif

#if GIFSAVER_EMBED_WAV && !GIFSAVER_AUDIO
#error "GIFSAVER_EMBED_WAV requires GIFSAVER_AUDIO (see config.h)"
#endif

const char *progname = "gifsaver";

#define FRAME_TIMER 1
#define MOUSE_SLOP  4   /* pixels the mouse may drift before we quit */

#define REG_KEY     "Software\\gifsaver"
#define REG_GIF     "GifFile"
#define REG_SOUND   "SoundFile"

#if GIFSAVER_EMBED_GIF && GIFSAVER_EMBED_WAV
#define CONFIG_NOTE "Leave a field empty to use the built-in GIF or sound."
#elif GIFSAVER_EMBED_GIF
#define CONFIG_NOTE "Leave the GIF empty to use the built-in animation."
#elif GIFSAVER_EMBED_WAV
#define CONFIG_NOTE "Leave the sound empty to play the built-in sound."
#else
#define CONFIG_NOTE ""
#endif

static struct {
    HBRUSH *brushes;    /* one tiling pattern brush per frame */
    int *delays;        /* per-frame delay in ms */
    int nframes, cur;
    POINT mouse;        /* cursor position at startup */
    int windowed;       /* -window: ordinary test window, input ignored */
} saver;

static void die(const char *msg)
{
    fprintf(stderr, "%s: %s\n", progname, msg);
    MessageBoxA(NULL, msg, progname, MB_OK | MB_ICONERROR);
    exit(1);
}

/* ------------------------------------------------------------------ */
/* Settings: default media paths in the registry, edited through the  */
/* dialog the screensaver control panel opens with /c:<hwnd>.         */
/* ------------------------------------------------------------------ */

/* Read a non-empty REG_SZ setting into buf; returns 0 if unset. */
static int reg_get(const char *name, char *buf, DWORD size)
{
    if (RegGetValueA(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_SZ,
                     NULL, buf, &size) != ERROR_SUCCESS)
        return 0;
    return buf[0] != '\0';
}

static void reg_set(HKEY key, const char *name, const char *value)
{
    if (*value)
        RegSetValueExA(key, name, 0, REG_SZ, (const BYTE *) value,
                       (DWORD) strlen(value) + 1);
    else
        RegDeleteValueA(key, name);
}

static void config_browse(HWND dlg, int edit_id, const char *filter)
{
    char path[MAX_PATH];
    OPENFILENAMEA ofn;

    GetDlgItemTextA(dlg, edit_id, path, sizeof path);
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = sizeof path;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn))
        SetDlgItemTextA(dlg, edit_id, path);
}

static int config_save(HWND dlg)
{
    char path[MAX_PATH];
    HKEY key;

#if !GIFSAVER_EMBED_GIF
    if (!GetWindowTextLengthA(GetDlgItem(dlg, IDC_GIF))) {
        MessageBoxA(dlg, "Choose an animated GIF to display.", progname,
                    MB_OK | MB_ICONWARNING);
        return -1;
    }
#endif
    if (RegCreateKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) {
        MessageBoxA(dlg, "Cannot save the settings.", progname,
                    MB_OK | MB_ICONERROR);
        return -1;
    }
    GetDlgItemTextA(dlg, IDC_GIF, path, sizeof path);
    reg_set(key, REG_GIF, path);
    GetDlgItemTextA(dlg, IDC_SOUND, path, sizeof path);
    reg_set(key, REG_SOUND, path);
    RegCloseKey(key);
    return 0;
}

static INT_PTR CALLBACK config_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    char path[MAX_PATH];

    (void) lp;
    switch (msg) {
    case WM_INITDIALOG:
        SendDlgItemMessageA(dlg, IDC_GIF, EM_LIMITTEXT, MAX_PATH - 1, 0);
        SendDlgItemMessageA(dlg, IDC_SOUND, EM_LIMITTEXT, MAX_PATH - 1, 0);
        if (reg_get(REG_GIF, path, sizeof path))
            SetDlgItemTextA(dlg, IDC_GIF, path);
        if (reg_get(REG_SOUND, path, sizeof path))
            SetDlgItemTextA(dlg, IDC_SOUND, path);
#if !GIFSAVER_AUDIO
        EnableWindow(GetDlgItem(dlg, IDC_SOUND), FALSE);
        EnableWindow(GetDlgItem(dlg, IDC_SOUND_BROWSE), FALSE);
#endif
        SetDlgItemTextA(dlg, IDC_NOTE, CONFIG_NOTE);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_GIF_BROWSE:
            config_browse(dlg, IDC_GIF,
                          "GIF images (*.gif)\0*.gif\0All files (*.*)\0*.*\0");
            return TRUE;
        case IDC_SOUND_BROWSE:
            config_browse(dlg, IDC_SOUND,
                          "WAV files (*.wav)\0*.wav\0All files (*.*)\0*.*\0");
            return TRUE;
        case IDOK:
            if (config_save(dlg) == 0)
                EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
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

/* ------------------------------------------------------------------ */
/* Looping WAV playback through waveOut. Completed buffers come back  */
/* to the screensaver window as MM_WOM_DONE and are queued again.     */
/* ------------------------------------------------------------------ */

#if GIFSAVER_AUDIO

static const GUID subtype_pcm = { 0x00000001, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID subtype_float = { 0x00000003, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static struct {
    HWAVEOUT wo;        /* NULL when audio is off */
    WavClip clip;
    WAVEHDR hdr[2];     /* both span the whole clip; one is always queued */
} audio;

static void audio_stop(const char *why)
{
    int i;

    fprintf(stderr, "%s: %s, continuing without sound\n", progname, why);
    if (audio.wo) {
        HWAVEOUT wo = audio.wo;
        audio.wo = NULL;
        waveOutReset(wo);
        for (i = 0; i < 2; i++)
            waveOutUnprepareHeader(wo, &audio.hdr[i], sizeof audio.hdr[i]);
        waveOutClose(wo);
    }
    wav_free(&audio.clip);
}

/* Load the requested (or built-in) clip and start looping it. */
static void audio_setup(HWND hwnd, const char *wav_file)
{
    WAVEFORMATEXTENSIBLE wfx;
    const WavClip *clip = &audio.clip;
    unsigned int bits;
    int have = 0, i;

    if (wav_file)
        have = wav_load_file(wav_file, &audio.clip) == 0;
#if GIFSAVER_EMBED_WAV
    else
        have = wav_load_builtin(&audio.clip) == 0;
#endif
    if (!have)
        return;

    bits = clip->frame_bytes / clip->channels * 8;
    memset(&wfx, 0, sizeof wfx);
    wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfx.Format.nChannels = (WORD) clip->channels;
    wfx.Format.nSamplesPerSec = clip->rate;
    wfx.Format.nAvgBytesPerSec = clip->rate * clip->frame_bytes;
    wfx.Format.nBlockAlign = (WORD) clip->frame_bytes;
    wfx.Format.wBitsPerSample = (WORD) bits;
    wfx.Format.cbSize = sizeof wfx - sizeof wfx.Format;
    wfx.Samples.wValidBitsPerSample = (WORD) bits;
    wfx.dwChannelMask = clip->channels == 1 ? SPEAKER_FRONT_CENTER :
                        clip->channels == 2 ? SPEAKER_FRONT_LEFT |
                                              SPEAKER_FRONT_RIGHT : 0;
    wfx.SubFormat = clip->format == WAV_FMT_F32LE ? subtype_float
                                                  : subtype_pcm;

    if (waveOutOpen(&audio.wo, WAVE_MAPPER, &wfx.Format, (DWORD_PTR) hwnd,
                    0, CALLBACK_WINDOW) != MMSYSERR_NOERROR) {
        audio.wo = NULL;
        audio_stop("cannot open audio device");
        return;
    }
    for (i = 0; i < 2; i++) {
        audio.hdr[i].lpData = (LPSTR) clip->data;
        audio.hdr[i].dwBufferLength =
            (DWORD) (clip->nframes * clip->frame_bytes);
        if (waveOutPrepareHeader(audio.wo, &audio.hdr[i],
                                 sizeof audio.hdr[i]) != MMSYSERR_NOERROR ||
            waveOutWrite(audio.wo, &audio.hdr[i],
                         sizeof audio.hdr[i]) != MMSYSERR_NOERROR) {
            audio_stop("cannot queue audio");
            return;
        }
    }
}

#else /* !GIFSAVER_AUDIO */

static void audio_setup(HWND hwnd, const char *wav_file)
{
    (void) hwnd;
    if (wav_file)
        fprintf(stderr, "%s: compiled without audio support "
                "(see config.h), -sound ignored\n", progname);
}

#endif /* GIFSAVER_AUDIO */

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

#if GIFSAVER_AUDIO
    case MM_WOM_DONE:
        if (audio.wo && waveOutWrite(audio.wo, (WAVEHDR *) lp,
                                     sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            audio_stop("audio write failed");
        return 0;
#endif

    case WM_SETCURSOR:
        if (saver.windowed)
            break;
        SetCursor(NULL);
        return TRUE;

    case WM_MOUSEMOVE: {
        POINT pt;
        if (saver.windowed)
            break;
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
        if (saver.windowed)
            break;
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        return 0;

    case WM_ACTIVATEAPP:
        if (!wp && !saver.windowed)
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

static void run_saver(const char *wav_file)
{
    HINSTANCE hinst = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    HWND hwnd;
    MSG m;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = saver_proc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "gifsaver";
    if (!RegisterClassA(&wc))
        die("RegisterClass failed");

    GetCursorPos(&saver.mouse);
    if (saver.windowed)
        hwnd = CreateWindowExA(0, "gifsaver", "gifsaver",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
                               NULL, NULL, hinst, NULL);
    else
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

    audio_setup(hwnd, wav_file);

    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}

int main(int argc, char **argv)
{
    static char reg_gif[MAX_PATH], reg_wav[MAX_PATH];
    Animation anim;
    const char *gif_file = NULL;
    const char *wav_file = NULL;
    const char *env;
    HWND parent = NULL;
    char mode = 'c';    /* no arguments means "settings" */
    int i, r;

    /* Let diagnostics reach the console when started from one. */
    if (AttachConsole(ATTACH_PARENT_PROCESS))
        freopen("CONOUT$", "w", stderr);

    /* Draw at native resolution instead of being bitmap-stretched. */
    SetProcessDPIAware();

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-sound") && i + 1 < argc) {
            wav_file = argv[++i];
        } else if (!strcmp(a, "-window")) {
            saver.windowed = 1;
            mode = 's';
        } else if ((a[0] == '/' || a[0] == '-') && a[1] &&
            (a[2] == '\0' || a[2] == ':')) {
            mode = (char) tolower((unsigned char) a[1]);
            if (mode == 'p' && a[2] == '\0' && i + 1 < argc)
                i++;    /* skip the preview window handle */
            else if (mode == 'c' && a[2] == ':')
                parent = (HWND) (UINT_PTR) strtoull(a + 3, NULL, 10);
        } else {
            gif_file = a;
        }
    }

    if (mode == 'p')
        return 0;
    if (mode != 's') {
        DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_CONFIG),
                        parent, config_proc, 0);
        return 0;
    }

    if (!gif_file) {
        env = getenv("GIFSAVER_GIF");
        if (env && *env)
            gif_file = env;
        else if (reg_get(REG_GIF, reg_gif, sizeof reg_gif))
            gif_file = reg_gif;
    }
    if (!wav_file) {
        env = getenv("GIFSAVER_SOUND");
        if (env && *env)
            wav_file = env;
        else if (reg_get(REG_SOUND, reg_wav, sizeof reg_wav))
            wav_file = reg_wav;
    }

#if GIFSAVER_EMBED_GIF
    r = gif_file ? anim_load_file(gif_file, -1, &anim)
                 : anim_load_builtin(-1, &anim);
#else
    if (!gif_file)
        die("no GIF configured: choose one in the screensaver settings");
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

    run_saver(wav_file);
    return 0;
}
