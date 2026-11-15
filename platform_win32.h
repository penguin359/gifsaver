/*
 * platform_win32.h - resource IDs shared by platform_win32.c and
 *                    platform_win32.rc.
 *
 * Copyright (c) 2026 Loren M. Lang
 * SPDX-License-Identifier: MIT
 */

#ifndef PLATFORM_WIN32_H
#define PLATFORM_WIN32_H

/* String 1 is the name Windows lists in the screensaver drop-down. */
#define IDS_DESCRIPTION     1

#define IDD_CONFIG          2003    /* scrnsave.h's DLG_SCRNSAVECONFIGURE */
#define IDD_ABOUT           2004

#define IDC_STATIC          (-1)
#define IDC_GIF             1001
#define IDC_GIF_BROWSE      1002
#define IDC_SOUND           1003
#define IDC_SOUND_BROWSE    1004
#define IDC_NOTE            1005
#define IDC_SOUND_ON        1006
#define IDC_ABOUT           1007
#define IDC_ABOUT_ICON      1008
#define IDC_ABOUT_VERSION   1009

#endif /* PLATFORM_WIN32_H */
