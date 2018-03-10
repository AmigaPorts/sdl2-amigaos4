/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2018 Sam Lantinga <slouken@libsdl.org>

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

#include <proto/exec.h>

#include "SDL_video.h"
#include "SDL_hints.h"
#include "SDL_version.h"

#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"

#include "SDL_os4video.h"
#include "SDL_os4events.h"
#include "SDL_os4framebuffer.h"
#include "SDL_os4mouse.h"
#include "SDL_os4opengl.h"
#include "SDL_os4opengles.h"
#include "SDL_os4shape.h"
#include "SDL_os4messagebox.h"
#include "SDL_os4modes.h"
#include "SDL_os4keyboard.h"
#include "SDL_os4library.h"

#define DEBUG
#include "../../main/amigaos4/SDL_os4debug.h"

#define OS4VID_DRIVER_NAME "os4"

/* Initialization/Query functions */
static int OS4_VideoInit(_THIS);
static void OS4_VideoQuit(_THIS);

SDL_bool (*OS4_ResizeGlContext)(_THIS, SDL_Window * window) = NULL;

/* OS4 driver bootstrap functions */

static int
OS4_Available(void)
{
    return (1);
}

/*
 * Libraries required by OS4 video driver
 */

#define MIN_LIB_VERSION 51

static SDL_bool
OS4_OpenLibraries(_THIS)
{
    dprintf("Opening libraries\n");

    GfxBase       = OS4_OpenLibrary("graphics.library", 54);
    LayersBase    = OS4_OpenLibrary("layers.library", 53);
    IntuitionBase = OS4_OpenLibrary("intuition.library", MIN_LIB_VERSION);
    IconBase      = OS4_OpenLibrary("icon.library", MIN_LIB_VERSION);
    WorkbenchBase = OS4_OpenLibrary("workbench.library", MIN_LIB_VERSION);
    KeymapBase    = OS4_OpenLibrary("keymap.library", MIN_LIB_VERSION);
    TextClipBase  = OS4_OpenLibrary("textclip.library", MIN_LIB_VERSION);
    DOSBase       = OS4_OpenLibrary("dos.library", MIN_LIB_VERSION);

    if (GfxBase && LayersBase && IntuitionBase && IconBase &&
        WorkbenchBase && KeymapBase && TextClipBase && DOSBase) {

        IGraphics  = (struct GraphicsIFace *)  OS4_GetInterface(GfxBase);
        ILayers    = (struct LayersIFace *)    OS4_GetInterface(LayersBase);
        IIntuition = (struct IntuitionIFace *) OS4_GetInterface(IntuitionBase);
        IIcon      = (struct IconIFace *)      OS4_GetInterface(IconBase);
        IWorkbench = (struct WorkbenchIFace *) OS4_GetInterface(WorkbenchBase);
        IKeymap    = (struct KeymapIFace *)    OS4_GetInterface(KeymapBase);
        ITextClip  = (struct TextClipIFace *)  OS4_GetInterface(TextClipBase);
        IDOS       = (struct DOSIFace *)       OS4_GetInterface(DOSBase);

        if (IGraphics && ILayers && IIntuition && IIcon &&
            IWorkbench && IKeymap && ITextClip && IDOS) {

            dprintf("All library interfaces OK\n");

            return SDL_TRUE;

        } else {
            dprintf("Failed to get library interfaces\n");
        }
    } else {
        dprintf("Failed to open system libraries\n");
    }

    return SDL_FALSE;
}

static void
OS4_CloseLibraries(_THIS)
{
    dprintf("Closing libraries\n");

    OS4_DropInterface((void *)&IDOS);
    OS4_DropInterface((void *)&ITextClip);
    OS4_DropInterface((void *)&IKeymap);
    OS4_DropInterface((void *)&IWorkbench);
    OS4_DropInterface((void *)&IIcon);
    OS4_DropInterface((void *)&IIntuition);
    OS4_DropInterface((void *)&ILayers);
    OS4_DropInterface((void *)&IGraphics);

    OS4_CloseLibrary(&DOSBase);
    OS4_CloseLibrary(&TextClipBase);
    OS4_CloseLibrary(&KeymapBase);
    OS4_CloseLibrary(&WorkbenchBase);
    OS4_CloseLibrary(&IconBase);
    OS4_CloseLibrary(&IntuitionBase);
    OS4_CloseLibrary(&LayersBase);
    OS4_CloseLibrary(&GfxBase);
}

static void
OS4_HandleScreenNotify(_THIS, ULONG cl)
{
    switch (cl) {
        case SNOTIFY_BEFORE_CLOSEWB:
            dprintf("Before close WB\n");
            OS4_IconifyWindows(_this);
            break;

        case SNOTIFY_AFTER_OPENWB:
            dprintf("After open WB\n");
            OS4_UniconifyWindows(_this);
            break;

        default:
            dprintf("Unknown screen notify message %d\n", cl);
            break;
    }
}

static int
OS4_NotifyTask(uint32_t param)
{
    _THIS = (SDL_VideoDevice *)param;
    SDL_VideoData *data = (SDL_VideoData *)_this->driverdata;

    if ((data->screenNotifySignal = IExec->AllocSignal(-1)) == -1) {
        dprintf("Failed to allocate screen notify signal\n");
        goto done;
    }

    if (!(data->screenNotifyPort = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE))) {
        dprintf("Failed to create screen notify msg port\n");
        goto done;
    }

    if (!(data->screenNotifyRequest = IIntuition->StartScreenNotifyTags(
        //SNA_PubName, "Workbench",
        SNA_MsgPort, data->screenNotifyPort,
        SNA_Notify, SNOTIFY_BEFORE_CLOSEWB | SNOTIFY_AFTER_OPENWB /*| SNOTIFY_WAIT_REPLY*/,
        SNA_Priority, 0,
        TAG_DONE))) {

        dprintf("Failed to start screen notify\n");
        goto done;
    }

    dprintf("Signalling main task\n");
    IExec->Signal(data->mainTask, 1L << data->mainSignal);

    while (data->running) {
        ULONG notifySignal = 1L << data->screenNotifyPort->mp_SigBit;
        ULONG stopSignal = 1L << data->screenNotifySignal;

        ULONG sigs = IExec->Wait(notifySignal | stopSignal);

        if (sigs & notifySignal) {
           struct ScreenNotifyMessage *msg;

            while ((msg = (struct ScreenNotifyMessage *)IExec->GetMsg(data->screenNotifyPort))) {
                ULONG cl = msg->snm_Class;
                IExec->ReplyMsg((struct Message *) msg);

                dprintf("Received screen notify msg %d\n", cl);

                OS4_HandleScreenNotify(_this, cl);
            }
        }

        if (sigs & stopSignal) {
            dprintf("Received stop signal\n");
            break;
        }
    }

    dprintf("SN task Ending\n");

done:

    if (data->screenNotifyRequest) {
        dprintf("End screen notify\n");
        if (!IIntuition->EndScreenNotify(data->screenNotifyRequest)) {
            dprintf("...failed\n");
        }

        data->screenNotifyRequest = NULL;
    }

    if (data->screenNotifyPort) {

        struct Message *msg;

        while ((msg = IExec->GetMsg(data->screenNotifyPort))) {
            IExec->ReplyMsg((struct Message *) msg);
        }

        IExec->FreeSysObject(ASOT_PORT, data->screenNotifyPort);
        data->screenNotifyPort = NULL;
    }

    if (data->screenNotifySignal != -1) {
        dprintf("Signalling main\n");

        IExec->Signal(data->mainTask, 1L << data->mainSignal);

        IExec->FreeSignal(data->screenNotifySignal);
        data->screenNotifySignal = -1;
    }

    dprintf("Waiting for removal\n");

    IExec->Wait(0L);
    return 0;
}

static void
OS4_FindApplicationName(_THIS)
{
    SDL_VideoData *data = (SDL_VideoData *) _this->driverdata;

    size_t size;

    char pathBuffer[MAX_DOS_PATH];
    char nameBuffer[MAX_DOS_FILENAME];

    if (IDOS->GetCliProgramName(pathBuffer, MAX_DOS_PATH - 1)) {
        CONST_STRPTR filePart = IDOS->FilePart(pathBuffer);

        snprintf(nameBuffer, MAX_DOS_FILENAME, "%s", filePart);
    } else {
        dprintf("Failed to get CLI program name, checking task node\n");

        struct Task* me = IExec->FindTask(NULL);
        snprintf(nameBuffer, MAX_DOS_FILENAME, "%s", ((struct Node *)me)->ln_Name);
    }

    size = SDL_strlen(nameBuffer) + 1;

    data->appName = SDL_malloc(size);

    if (data->appName) {
        snprintf(data->appName, size, nameBuffer);
    }

    dprintf("Application name: '%s'\n", data->appName);
}

static SDL_bool
OS4_AllocSystemResources(_THIS)
{
    SDL_VideoData *data = (SDL_VideoData *) _this->driverdata;

    dprintf("Called\n");

    if (!OS4_OpenLibraries(_this)) {
        return SDL_FALSE;
    }

    OS4_FindApplicationName(_this);

    data->running = TRUE;
    data->mainTask = IExec->FindTask(NULL);

    if (!(data->userPort = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE))) {
        SDL_SetError("Couldn't allocate message port");
        return SDL_FALSE;
    }

    if (!(data->appMsgPort = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE))) {
        SDL_SetError("Couldn't allocate AppMsg port");
        return SDL_FALSE;
    }

    if ((data->mainSignal = IExec->AllocSignal(-1)) == -1) {
        SDL_SetError("Couldn't allocate main signal");
        return SDL_FALSE;
    }

    if (!(data->screenNotifyTask = IExec->CreateTaskTags("SDL2 Screen Notification",
        0, OS4_NotifyTask, 16384, AT_Param1, (uint32)_this, TAG_DONE))) {

        SDL_SetError("Couldn't create Screen Notification task");
        return SDL_FALSE;
    }

    dprintf("Waiting for sn task\n");
    IExec->Wait(1L << data->mainSignal);
    dprintf("sn reported\n");

    /* Create the pool we'll be using (Shared, might be used from threads) */
    if (!(data->pool = IExec->AllocSysObjectTags(ASOT_MEMPOOL,
        ASOPOOL_MFlags,    MEMF_SHARED,
        ASOPOOL_Threshold, 16384,
        ASOPOOL_Puddle,    16384,
        ASOPOOL_Protected, TRUE,
        TAG_DONE))) {

        SDL_SetError("Couldn't allocate pool");
        return SDL_FALSE;
    }

    /* inputPort, inputReq and and input.device are created for WarpMouse functionality. (In SDL1
    they were created in library constructor for an unknown reason) */
    if (!(data->inputPort = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE))) {

        SDL_SetError("Couldn't allocate input port");
        return SDL_FALSE;
    }

    if (!(data->inputReq = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
                                             ASOIOR_Size,       sizeof(struct IOStdReq),
                                             ASOIOR_ReplyPort,  data->inputPort,
                                             TAG_DONE))) {

        SDL_SetError("Couldn't allocate input request");
        return SDL_FALSE;
    }

    if (IExec->OpenDevice("input.device", 0, (struct IORequest *)data->inputReq, 0))
    {
        SDL_SetError("Couldn't open input.device");
        return SDL_FALSE;
    }

    IInput = (struct InputIFace *)OS4_GetInterface((struct Library *)data->inputReq->io_Device);
    if (!IInput) {
        SDL_SetError("Failed to get IInput interface");
        return SDL_FALSE;
    }

    return SDL_TRUE;
}

static void
OS4_FreeSystemResources(_THIS)
{
    SDL_VideoData *data = (SDL_VideoData *) _this->driverdata;

    dprintf("Called\n");

    data->running = FALSE;

    if (data->screenNotifyTask && data->screenNotifySignal) {
        dprintf("Signalling screen notify task\n");

        IExec->Signal(data->screenNotifyTask, 1L << data->screenNotifySignal);

        if (data->mainSignal != -1) {
            dprintf("Waiting for screen notify task\n");
            IExec->Wait(1L << data->mainSignal);
        }

        IExec->RemTask(data->screenNotifyTask);
    }

    if (data->mainSignal != -1) {
        IExec->FreeSignal(data->mainSignal);
    }

    OS4_DropInterface((void *)&IInput);

    if (data->inputReq) {
        dprintf("Deleting input.device\n");
        //IExec->AbortIO((struct IORequest *)data->inputReq);
        //IExec->WaitIO((struct IORequest *)data->inputReq);
        IExec->CloseDevice((struct IORequest *)data->inputReq);

        dprintf("Deleting IORequest\n");
        IExec->FreeSysObject(ASOT_IOREQUEST, (void *)data->inputReq);
    }

    if (data->inputPort) {
        dprintf("Deleting MsgPort\n");
        IExec->FreeSysObject(ASOT_PORT, (void *)data->inputPort);
    }

    if (data->pool) {
        IExec->FreeSysObject(ASOT_MEMPOOL, data->pool);
    }

    if (data->appMsgPort) {
        struct Message *msg;

        while ((msg = IExec->GetMsg(data->appMsgPort))) {
            IExec->ReplyMsg((struct Message *) msg);
        }

        IExec->FreeSysObject(ASOT_PORT, data->appMsgPort);
    }

    if (data->userPort) {
        IExec->FreeSysObject(ASOT_PORT, data->userPort);
    }

    if (data->appName) {
        SDL_free(data->appName);
    }

    OS4_CloseLibraries(_this);
}

static void
OS4_DeleteDevice(SDL_VideoDevice * device)
{
    dprintf("Called\n");

    OS4_FreeSystemResources(device);

    SDL_free(device->driverdata);
    SDL_free(device);
}

static void
OS4_SetMiniGLFunctions(SDL_VideoDevice * device)
{
    device->GL_GetProcAddress = OS4_GL_GetProcAddress;
    device->GL_UnloadLibrary = OS4_GL_UnloadLibrary;
    device->GL_MakeCurrent = OS4_GL_MakeCurrent;
    device->GL_GetDrawableSize = OS4_GL_GetDrawableSize;
    device->GL_SetSwapInterval = OS4_GL_SetSwapInterval;
    device->GL_GetSwapInterval = OS4_GL_GetSwapInterval;
    device->GL_SwapWindow = OS4_GL_SwapWindow;
    device->GL_CreateContext = OS4_GL_CreateContext;
    device->GL_DeleteContext = OS4_GL_DeleteContext;

    OS4_ResizeGlContext = OS4_GL_ResizeContext;
}

#if SDL_VIDEO_OPENGL_ES2
static void
OS4_SetGLESFunctions(SDL_VideoDevice * device)
{
    /* Some functions are recycled from SDL_os4opengl.c 100% ... */
    device->GL_GetProcAddress = OS4_GLES_GetProcAddress;
    device->GL_UnloadLibrary = OS4_GLES_UnloadLibrary;
    device->GL_MakeCurrent = OS4_GLES_MakeCurrent;
    device->GL_GetDrawableSize = OS4_GL_GetDrawableSize;
    device->GL_SetSwapInterval = OS4_GL_SetSwapInterval;
    device->GL_GetSwapInterval = OS4_GL_GetSwapInterval;
    device->GL_SwapWindow = OS4_GLES_SwapWindow;
    device->GL_CreateContext = OS4_GLES_CreateContext;
    device->GL_DeleteContext = OS4_GLES_DeleteContext;

    OS4_ResizeGlContext = OS4_GLES_ResizeContext;
}
#endif

#if SDL_VIDEO_OPENGL_ES2
static SDL_bool
OS4_IsOpenGLES2(_THIS)
{
    if ((_this->gl_config.profile_mask == SDL_GL_CONTEXT_PROFILE_ES) &&
        (_this->gl_config.major_version == 2) &&
        (_this->gl_config.minor_version == 0)) {
            dprintf("OpenGL ES 2.0 requested\n");
            return SDL_TRUE;
    }

    return SDL_FALSE;
}
#endif

static int
OS4_LoadGlLibrary(_THIS, const char * path)
{
    dprintf("Profile_mask %d, major ver %d, minor ver %d\n",
        _this->gl_config.profile_mask,
        _this->gl_config.major_version,
        _this->gl_config.minor_version);

#if SDL_VIDEO_OPENGL_ES2
    if (OS4_IsOpenGLES2(_this)) {
        OS4_SetGLESFunctions(_this);
        return OS4_GLES_LoadLibrary(_this, path);
    } else {
        OS4_SetMiniGLFunctions(_this);
    }
#endif

    return OS4_GL_LoadLibrary(_this, path);
}

static void
OS4_SetFunctionPointers(SDL_VideoDevice * device)
{
    device->VideoInit = OS4_VideoInit;
    device->VideoQuit = OS4_VideoQuit;

    device->GetDisplayBounds = OS4_GetDisplayBounds;
    device->GetDisplayModes = OS4_GetDisplayModes;
    device->SetDisplayMode = OS4_SetDisplayMode;

    device->CreateSDLWindow = OS4_CreateWindow;
    device->CreateSDLWindowFrom = OS4_CreateWindowFrom;
    device->SetWindowTitle = OS4_SetWindowTitle;
    //device->SetWindowIcon = OS4_SetWindowIcon;
    device->SetWindowPosition = OS4_SetWindowPosition;
    device->SetWindowSize = OS4_SetWindowSize;
    device->ShowWindow = OS4_ShowWindow;
    device->HideWindow = OS4_HideWindow;
    device->RaiseWindow = OS4_RaiseWindow;

    device->SetWindowMinimumSize = OS4_SetWindowMinMaxSize;
    device->SetWindowMaximumSize = OS4_SetWindowMinMaxSize;

    device->MaximizeWindow = OS4_MaximizeWindow;
    device->MinimizeWindow = OS4_MinimizeWindow;
    device->RestoreWindow = OS4_RestoreWindow;

    //device->SetWindowBordered = OS4_SetWindowBordered; // Not supported by SetWindowAttrs()?
    device->SetWindowFullscreen = OS4_SetWindowFullscreen;
    //device->SetWindowGammaRamp = OS4_SetWindowGammaRamp;
    //device->GetWindowGammaRamp = OS4_GetWindowGammaRamp;
    device->SetWindowGrab = OS4_SetWindowGrab;
    device->DestroyWindow = OS4_DestroyWindow;

    device->CreateWindowFramebuffer = OS4_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = OS4_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = OS4_DestroyWindowFramebuffer;

    //device->OnWindowEnter = OS4_OnWindowEnter;
    device->SetWindowHitTest = OS4_SetWindowHitTest;

    device->SetWindowOpacity = OS4_SetWindowOpacity;
    device->GetWindowBordersSize = OS4_GetWindowBordersSize;

    device->shape_driver.CreateShaper = OS4_CreateShaper;
    device->shape_driver.SetWindowShape = OS4_SetWindowShape;
    device->shape_driver.ResizeWindowShape = OS4_ResizeWindowShape;

    device->GetWindowWMInfo = OS4_GetWindowWMInfo;

    device->GL_LoadLibrary = OS4_LoadGlLibrary;
    OS4_SetMiniGLFunctions(device);

    device->PumpEvents = OS4_PumpEvents;
    //device->SuspendScreenSaver = OS4_SuspendScreenSaver;
    device->SetClipboardText = OS4_SetClipboardText;
    device->GetClipboardText = OS4_GetClipboardText;
    device->HasClipboardText = OS4_HasClipboardText;
    //device->ShowMessageBox = OS4_ShowMessageBox; Can be called without video initialization

    device->free = OS4_DeleteDevice;
}

static SDL_VideoDevice *
OS4_CreateDevice(int devindex)
{
    SDL_VideoDevice *device;
    SDL_VideoData *data;
    SDL_version version;

    SDL_GetVersion(&version);

    dprintf("*** SDL %d.%d.%d video initialization starts ***\n",
        version.major, version.minor, version.patch);

    /* Initialize all variables that we clean on shutdown */
    device = (SDL_VideoDevice *) SDL_calloc(1, sizeof(SDL_VideoDevice));

    if (device) {
        data = (SDL_VideoData *) SDL_calloc(1, sizeof(SDL_VideoData));
    } else {
        data = NULL;
    }

    if (!data) {
        SDL_free(device);
        SDL_OutOfMemory();
        return NULL;
    }

    device->driverdata = data;

    if (!OS4_AllocSystemResources(device)) {
        /* If we return with NULL, SDL_VideoQuit() can't clean up OS4 stuff. So let's do it now. */
        OS4_FreeSystemResources(device);

        SDL_free(device);
        SDL_free(data);

        SDL_Unsupported();

        return NULL;
    }

    OS4_SetFunctionPointers(device);

    return device;
}

VideoBootStrap OS4_bootstrap = {
    OS4VID_DRIVER_NAME, "SDL AmigaOS 4 video driver",
    OS4_Available, OS4_CreateDevice
};

int
OS4_VideoInit(_THIS)
{
    dprintf("Called\n");

    if (OS4_InitModes(_this) < 0) {
        return SDL_SetError("Failed to initialize modes");
    }

    OS4_InitKeyboard(_this);
    OS4_InitMouse(_this);

    // We don't want SDL to change  window setup in SDL_OnWindowFocusLost()
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    return 0;
}

void
OS4_VideoQuit(_THIS)
{
    dprintf("Called\n");

    OS4_QuitModes(_this);
    OS4_QuitKeyboard(_this);
    OS4_QuitMouse(_this);
}

void *
OS4_SaveAllocPooled(_THIS, uint32 size)
{
    SDL_VideoData *data = (SDL_VideoData *) _this->driverdata;

    return IExec->AllocPooled(data->pool, size);
}

void *
OS4_SaveAllocVecPooled(_THIS, uint32 size)
{
    SDL_VideoData *data = (SDL_VideoData *) _this->driverdata;

    return IExec->AllocVecPooled(data->pool, size);
}

void
OS4_SaveFreePooled(_THIS, void * mem, uint32 size)
{
    SDL_VideoData *data = (SDL_VideoData *) _this->driverdata;

    IExec->FreePooled(data->pool, mem, size);
}

void
OS4_SaveFreeVecPooled(_THIS, void * mem)
{
    SDL_VideoData *data = (SDL_VideoData *) _this->driverdata;

    IExec->FreeVecPooled(data->pool, mem);
}

/* Native window apps may be interested in calling this */
struct MsgPort *
OS4_GetSharedMessagePort()
{
    SDL_VideoDevice *vd = SDL_GetVideoDevice();

    if (vd) {
        SDL_VideoData *data = (SDL_VideoData *) vd->driverdata;
        if (data) {
            return data->userPort;
        }
    }

    return NULL;
}

#endif /* SDL_VIDEO_DRIVER_AMIGAOS4 */

/* vi: set ts=4 sw=4 expandtab: */
