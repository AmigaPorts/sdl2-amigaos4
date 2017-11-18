/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2017 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_AMIGAOS4

#include <proto/wb.h>

#include "SDL_os4video.h"
#include "SDL_os4shape.h"
#include "SDL_os4window.h"
#include "SDL_os4modes.h"
#include "SDL_os4opengl.h"

#include "SDL_syswm.h"
#include "../../events/SDL_keyboard_c.h"

#define DEBUG
#include "../../main/amigaos4/SDL_os4debug.h"

extern SDL_bool (*OS4_ResizeGlContext)(_THIS, SDL_Window * window);

static void OS4_CloseWindowInternal(_THIS, struct Window * window);

static SDL_bool
OS4_IsFullscreen(SDL_Window * window)
{
    return (window->flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP));
}

static void
OS4_RemoveAppWindow(_THIS, SDL_WindowData *data)
{
    if (data->appWin) {
        if (IWorkbench->RemoveAppWindow(data->appWin) == FALSE) {
            dprintf("Failed to remove AppWindow\n");
        }
        data->appWin = NULL;
    }
}

static int
OS4_SetupWindowData(_THIS, SDL_Window * sdlwin, struct Window * syswin)
{
    SDL_VideoData *videodata = (SDL_VideoData *) _this->driverdata;

    SDL_WindowData *data = (SDL_WindowData *) SDL_calloc(1, sizeof(*data));
    if (!data) {
        return SDL_OutOfMemory();
    }

    data->sdlwin = sdlwin;
    data->syswin = syswin;
    data->pointerGrabTicks = 0;

    sdlwin->driverdata = data;

    if (data->syswin) {
        int width = 0;
        int height = 0;

        LONG ret = IIntuition->GetWindowAttrs(
            data->syswin,
            WA_InnerWidth, &width,
            WA_InnerHeight, &height,
            TAG_DONE);

        if (ret) {
            dprintf("GetWindowAttrs() returned %d\n", ret);
        }

        dprintf("'%s' dimensions %d*%d\n", sdlwin->title, width, height);

        sdlwin->w = width;
        sdlwin->h = height;
    }

    // Pass SDL window as user data
    data->appWin = IWorkbench->AddAppWindow(0, (ULONG)sdlwin, syswin, videodata->appMsgPort, TAG_DONE);

    if (!data->appWin) {
        dprintf("Couldn't create AppWindow\n");
    }

    return 0;
}

static uint32
OS4_GetIDCMPFlags(SDL_Window * window, SDL_bool fullscreen)
{
    uint32 IDCMPFlags  = IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE
                       | IDCMP_DELTAMOVE | IDCMP_RAWKEY | IDCMP_ACTIVEWINDOW
                       | IDCMP_INACTIVEWINDOW | IDCMP_INTUITICKS
                       | IDCMP_EXTENDEDMOUSE;

    dprintf("Called\n");

    if (!fullscreen) {
        if (!(window->flags & SDL_WINDOW_BORDERLESS)) {
            IDCMPFlags  |= IDCMP_CLOSEWINDOW;
        }

        if (window->flags & SDL_WINDOW_RESIZABLE) {
            //IDCMPFlags  |= IDCMP_SIZEVERIFY; no handling so far
            IDCMPFlags |= IDCMP_NEWSIZE;
        }
    }

    return IDCMPFlags;
}

static uint32
OS4_GetWindowFlags(SDL_Window * window, SDL_bool fullscreen)
{
    uint32 windowFlags = WFLG_REPORTMOUSE | WFLG_RMBTRAP;

    dprintf("Called\n");

    if (fullscreen) {
        windowFlags |= WFLG_BORDERLESS | WFLG_SIMPLE_REFRESH | WFLG_BACKDROP;
    } else {
        windowFlags |= WFLG_SMART_REFRESH | WFLG_NOCAREREFRESH | WFLG_NEWLOOKMENUS;

        if (window->flags & SDL_WINDOW_BORDERLESS) {
            windowFlags |= WFLG_BORDERLESS;
        } else {
            windowFlags |= WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET;

            if (window->flags & SDL_WINDOW_RESIZABLE) {
                windowFlags |= WFLG_SIZEGADGET | WFLG_SIZEBBOTTOM;
            }
        }
    }

    return windowFlags;
}

static struct Screen *
OS4_GetScreenForWindow(_THIS, SDL_VideoDisplay * display)
{
    if (display) {
        SDL_DisplayData *displaydata = (SDL_DisplayData *) display->driverdata;

        dprintf("Fullscreen\n");
        return displaydata->screen;
    } else {
        SDL_VideoData *videodata = (SDL_VideoData *) _this->driverdata;

        dprintf("Window mode (public screen)\n");
        return videodata->publicScreen;
    }
}

static ULONG
OS4_BackFill(const struct Hook *hook, struct RastPort *rastport, struct BackFillMessage *message)
{
    struct Rectangle *rect = &message->Bounds;
    struct GraphicsIFace *igfx = hook->h_Data;

    struct RastPort bfRastport;

    igfx->InitRastPort(&bfRastport);
    bfRastport.BitMap = rastport->BitMap;

    igfx->RectFillColor(&bfRastport, rect->MinX, rect->MinY, rect->MaxX, rect->MaxY, 0xFF000000);

    return 0;
}

static struct Hook OS4_BackFillHook = {
    {0, 0},       /* h_MinNode */
    OS4_BackFill, /* h_Entry */
    0,            /* h_SubEntry */
    0             /* h_Data */
};

static void
OS4_CenterWindow(struct Screen * screen, SDL_Window * window)
{
    if (SDL_WINDOWPOS_ISCENTERED(window->x) || SDL_WINDOWPOS_ISUNDEFINED(window->x)) {
        window->x = (screen->Width - window->w) / 2;
        dprintf("X centered\n");
    }

    if (SDL_WINDOWPOS_ISCENTERED(window->y) || SDL_WINDOWPOS_ISUNDEFINED(window->y)) {
        window->y = (screen->Height - window->h) / 2;
        dprintf("Y centered\n");
    }
}

static struct Window *
OS4_CreateWindowInternal(_THIS, SDL_Window * window, SDL_VideoDisplay * display)
{
    SDL_VideoData *videodata = (SDL_VideoData *) _this->driverdata;

    struct Window *syswin;

    SDL_bool fullscreen = display ? SDL_TRUE : SDL_FALSE;

    uint32 IDCMPFlags = OS4_GetIDCMPFlags(window, fullscreen);
    uint32 windowFlags = OS4_GetWindowFlags(window, fullscreen);

    struct Screen *screen = OS4_GetScreenForWindow(_this, display);

    OS4_BackFillHook.h_Data = IGraphics; // Smuggle interface ptr for the hook

    OS4_CenterWindow(screen, window);

    dprintf("Opening window '%s' at (%d,%d) of size (%dx%d) on screen %p\n",
        window->title, window->x, window->y, window->w, window->h, screen);

    syswin = IIntuition->OpenWindowTags(
        NULL,
        WA_PubScreen, screen,
        WA_Title, fullscreen ? NULL : window->title,
        WA_ScreenTitle, window->title,
        WA_Left, window->x,
        WA_Top, window->y,
        WA_InnerWidth, window->w,
        WA_InnerHeight, window->h,
        WA_Flags, windowFlags,
        WA_IDCMP, IDCMPFlags,
        WA_Hidden, (window->flags & SDL_WINDOW_HIDDEN) ? TRUE : FALSE,
        WA_GrabFocus, (window->flags & SDL_WINDOW_INPUT_GRABBED) ? POINTER_GRAB_TIMEOUT : 0,
        WA_UserPort, videodata->userport,
        WA_BackFill, &OS4_BackFillHook,
        TAG_DONE);

    if (syswin) {
        dprintf("Window address %p\n", syswin);
    } else {
        dprintf("Couldn't create window\n");
        return NULL;
    }

    if (window->flags & SDL_WINDOW_RESIZABLE) {

        // If this window is resizable, reset window size limits
        // so that the user can actually resize it.
        BOOL ret = IIntuition->WindowLimits(syswin,
            syswin->BorderLeft + syswin->BorderRight + 100,
            syswin->BorderTop + syswin->BorderBottom + 100,
            -1,
            -1);

        if (!ret) {
            dprintf("Failed to set window limits\n");
        }
    }

    return syswin;
}

int
OS4_CreateWindow(_THIS, SDL_Window * window)
{
    struct Window *syswin = NULL;

    if (OS4_IsFullscreen(window)) {
        // We may not have the screen opened yet, so let's wait that SDL calls us back with
        // SDL_SetWindowFullscreen() and open the window then.
        dprintf("Open fullscreen window with delay\n");
    } else {
        if (!(syswin = OS4_CreateWindowInternal(_this, window, NULL))) {
            return SDL_SetError("Failed to create system window");
        }
    }

    if (OS4_SetupWindowData(_this, window, syswin) < 0) {

        OS4_RemoveAppWindow(_this, window->driverdata);

        if (syswin) {
            OS4_CloseWindowInternal(_this, syswin);
        }

        return SDL_SetError("Failed to setup window data");
    }

    return 0;
}

int
OS4_CreateWindowFrom(_THIS, SDL_Window * window, const void * data)
{
    struct Window *syswin = (struct Window *) data;

    dprintf("Called for native window %p (flags 0x%X)\n", data, window->flags);

    if (syswin->Title && SDL_strlen(syswin->Title)) {
        window->title = SDL_strdup(syswin->Title);
    }

    if (OS4_SetupWindowData(_this, window, syswin) < 0) {
        return -1;
    }

    // TODO: OpenGL, (fullscreen may not be applicable here?)

    return 0;
}

void
OS4_SetWindowTitle(_THIS, SDL_Window * window)
{
    SDL_WindowData *data = window->driverdata;

    //dprintf("Called\n");

    if (data && data->syswin) {
        STRPTR title = window->title ? window->title : "";

        IIntuition->SetWindowTitles(data->syswin, title, title);
    }
}

void
OS4_SetWindowBoxInternal(_THIS, SDL_Window * window)
{
    SDL_WindowData *data = window->driverdata;

    if (data && data->syswin) {
        LONG ret;

        if (SDL_IsShapedWindow(window)) {
            OS4_ResizeWindowShape(window);
        }

        ret = IIntuition->SetWindowAttrs(data->syswin,
            WA_Left, window->x,
            WA_Top, window->y,
            WA_InnerWidth, window->w,
            WA_InnerHeight, window->h,
            TAG_DONE);

        if (ret) {
            dprintf("SetWindowAttrs() returned %d\n", ret);
        }

        if (data->glContext) {
            OS4_ResizeGlContext(_this, window);
        }
    }
}

void
OS4_SetWindowPosition(_THIS, SDL_Window * window)
{
    SDL_WindowData *data = window->driverdata;

    dprintf("New window position %d, %d\n", window->x, window->y);

    if (data && data->syswin) {

        LONG ret = IIntuition->SetWindowAttrs(data->syswin,
            WA_Left, window->x,
            WA_Top, window->y,
            TAG_DONE);

        if (ret) {
            dprintf("SetWindowAttrs() returned %d\n", ret);
        }
    }
}

void
OS4_SetWindowSize(_THIS, SDL_Window * window)
{
    SDL_WindowData *data = window->driverdata;

    if (data && data->syswin) {

        int width = 0;
        int height = 0;

        LONG ret = IIntuition->GetWindowAttrs(
                        data->syswin,
                        WA_InnerWidth, &width,
                        WA_InnerHeight, &height,
                        TAG_DONE);

        if (ret) {
            dprintf("GetWindowAttrs() returned %d\n", ret);
        }

        if (width != window->w || height != window->h) {

            dprintf("New window size %d*%d\n", window->w, window->h);

            if (SDL_IsShapedWindow(window)) {
                OS4_ResizeWindowShape(window);
            }

            ret = IIntuition->SetWindowAttrs(data->syswin,
                WA_InnerWidth, window->w,
                WA_InnerHeight, window->h,
                TAG_DONE);

            if (ret) {
                dprintf("SetWindowAttrs() returned %d\n", ret);
            }

            if (data->glContext /*window->flags & SDL_WINDOW_OPENGL*/) {
                OS4_ResizeGlContext(_this, window);
            }
        } else {
            dprintf("Ignored size request %d*%d\n", width, height);
        }
    }
}

void
OS4_ShowWindow(_THIS, SDL_Window * window)
{
    SDL_WindowData *data = window->driverdata;

    dprintf("Showing window '%s'\n", window->title);

    if (data && data->syswin) {

        // TODO: could use ShowWindow but what we pass for the Other?
        LONG ret = IIntuition->SetWindowAttrs(data->syswin,
            WA_Hidden, FALSE,
            TAG_DONE);

        if (ret) {
            dprintf("SetWindowAttrs() returned %d\n", ret);
        }

        if (OS4_IsFullscreen(window)) {
            IIntuition->ScreenToFront(data->syswin->WScreen);
        }

        IIntuition->ActivateWindow(data->syswin);

        window->flags |= SDL_WINDOW_INPUT_FOCUS;

        SDL_SetKeyboardFocus(window);
    }
}

void
OS4_HideWindow(_THIS, SDL_Window * window)
{
    SDL_WindowData *data = window->driverdata;

    dprintf("Hiding window '%s'\n", window->title);

    if (data && data->syswin) {

        /* TODO: how to hide a fullscreen window? Close the screen? */
        BOOL result = IIntuition->HideWindow(data->syswin);

        if (!result) {
            dprintf("HideWindow() failed\n");
        }
    }
}

void
OS4_RaiseWindow(_THIS, SDL_Window * window)
{
    SDL_WindowData *data = window->driverdata;

    dprintf("Raising window '%s'\n", window->title);

    if (data && data->syswin) {
        IIntuition->WindowToFront(data->syswin);
        IIntuition->ActivateWindow(data->syswin);
    }
}

static void
OS4_CloseWindowInternal(_THIS, struct Window * window)
{
    if (window) {
        dprintf("Closing window '%s' (address %p)\n", window->Title, window);
        struct Screen *screen = window->WScreen;

        IIntuition->CloseWindow(window);

        OS4_CloseScreenInternal(_this, screen);

    } else {
        dprintf("NULL pointer\n");
    }
}

void
OS4_SetWindowFullscreen(_THIS, SDL_Window * window, SDL_VideoDisplay * display, SDL_bool fullscreen)
{
    dprintf("Trying to set '%s' into %s mode\n", window->title, fullscreen ? "fullscreen" : "window");

    if (window->is_destroying) {
        // This function gets also called during window closing
        dprintf("Window '%s' is being destroyed, mode change ignored\n", window->title);
    } else {
        SDL_WindowData *data = window->driverdata;

        if (window->flags & SDL_WINDOW_FOREIGN) {
            dprintf("Native window '%s' (%p), mode change ignored\n", window->title, data->syswin);
        } else {
            if (fullscreen) {
                // Detect dummy transition and keep calm
                SDL_DisplayData *displayData = display->driverdata;

                if (displayData->screen && data->syswin) {
                    if (data->syswin->WScreen == displayData->screen) {
                        dprintf("Same screen, useless mode change ignored\n");
                        return;
                    }
                }
            }

            OS4_RemoveAppWindow(_this, data);

            if (data->syswin) {
                dprintf("Reopening window '%s' (%p) due to mode change\n",
                    window->title, data->syswin);

                OS4_CloseWindowInternal(_this, data->syswin);

            } else {
                dprintf("System window doesn't exist yet, let's open it\n");
            }

            data->syswin = OS4_CreateWindowInternal(_this, window, fullscreen ? display : NULL);

            if (fullscreen) {
                // Workaround: make the new fullscreen window active
                OS4_ShowWindow(_this, window);
            }
        }
    }
}

// This may be called from os4events.c
void
OS4_SetWindowGrabInternal(_THIS, struct Window * w, SDL_bool activate)
{
    if (w) {
        struct IBox grabBox = {
            w->BorderLeft,
            w->BorderTop,
            w->Width  - w->BorderLeft - w->BorderRight,
            w->Height - w->BorderTop  - w->BorderBottom
        };

        LONG ret;

        if (activate) {
            // It seems to be that grabbed window should be active, otherwise some other
            // window (like shell) may be grabbed?
            IIntuition->ActivateWindow(w);

            ret = IIntuition->SetWindowAttrs(w,
                WA_MouseLimits, &grabBox,
                WA_GrabFocus, POINTER_GRAB_TIMEOUT,
                TAG_DONE);
        } else {
            ret = IIntuition->SetWindowAttrs(w,
                WA_MouseLimits, NULL,
                WA_GrabFocus, 0,
                TAG_DONE);
        }

        if (ret) {
            dprintf("SetWindowAttrs() returned %d\n", ret);
        } else {
            dprintf("Window %p ('%s') input was %s\n",
                w, w->Title, activate ? "grabbed" : "released");
        }
    }
}

void
OS4_SetWindowGrab(_THIS, SDL_Window * window, SDL_bool grabbed)
{
    SDL_WindowData *data = window->driverdata;

    if (data) {
        OS4_SetWindowGrabInternal(_this, data->syswin, grabbed);
        data->pointerGrabTicks = 0;
    }
}

void
OS4_DestroyWindow(_THIS, SDL_Window * window)
{
    SDL_WindowData *data = window->driverdata;

    dprintf("Called for '%s' (flags 0x%X)\n", window->title, window->flags);

    if (data) {

        OS4_RemoveAppWindow(_this, data);

        if (data->syswin) {

            if (!(window->flags & SDL_WINDOW_FOREIGN)) {

                if (SDL_IsShapedWindow(window)) {
                    OS4_DestroyShape(_this, window);
                }

                OS4_CloseWindowInternal(_this, data->syswin);
                data->syswin = NULL;
            } else {
                dprintf("Ignored for native window\n");
            }
        }

        if (window->flags & SDL_WINDOW_OPENGL) {
            OS4_GL_FreeBuffers(_this, data);
        }

        SDL_free(data);
        window->driverdata = NULL;
    }
}

SDL_bool
OS4_GetWindowWMInfo(_THIS, SDL_Window * window, struct SDL_SysWMinfo * info)
{
    struct Window *syswin = ((SDL_WindowData *) window->driverdata)->syswin;

    dprintf("Called\n");

    if (info->version.major <= SDL_MAJOR_VERSION) {
        info->subsystem = SDL_SYSWM_OS4;
        info->info.os4.window = syswin;
        return SDL_TRUE;
    } else {
        SDL_SetError("Application not compiled with SDL %d.%d\n",
                    SDL_MAJOR_VERSION, SDL_MINOR_VERSION);
        return SDL_FALSE;
    }
}

int
OS4_SetWindowHitTest(SDL_Window * window, SDL_bool enabled)
{
    return 0; // just succeed, the real work is done elsewhere
}

int
OS4_SetWindowOpacity(_THIS, SDL_Window * window, float opacity)
{
    struct Window *syswin = ((SDL_WindowData *) window->driverdata)->syswin;
    LONG ret;

    UBYTE value = opacity * 255;

    dprintf("Setting window '%s' opaqueness to %d\n", window->title, value);

    ret = IIntuition->SetWindowAttrs(
        syswin,
        WA_Opaqueness, value,
        TAG_DONE);

    if (ret) {
        dprintf("Failed to set window opaqueness to %d\n", value);
        return -1;
    }

    return 0;
}

int
OS4_GetWindowBordersSize(_THIS, SDL_Window * window, int * top, int * left, int * bottom, int * right)
{
    struct Window *syswin = ((SDL_WindowData *) window->driverdata)->syswin;

    if (top) {
        *top = syswin->BorderTop;
    }

    if (left) {
        *left = syswin->BorderLeft;
    }

    if (bottom) {
        *bottom = syswin->BorderBottom;
    }

    if (right) {
        *right = syswin->BorderRight;
    }

    return 0;
}

#endif /* SDL_VIDEO_DRIVER_AMIGAOS4 */

/* vi: set ts=4 sw=4 expandtab: */
