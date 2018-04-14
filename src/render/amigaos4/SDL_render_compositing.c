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

#if SDL_VIDEO_RENDER_AMIGAOS4 && !SDL_RENDER_DISABLED

#include "SDL_render_compositing.h"
#include "SDL_rc_texture.h"
#include "SDL_rc_draw.h"

#include "../SDL_sysrender.h"
#include "SDL_hints.h"

#include "../../video/SDL_sysvideo.h"
#include "../../video/amigaos4/SDL_os4window.h"
#include "../../video/amigaos4/SDL_os4video.h"

#include <proto/graphics.h>
#include <proto/layers.h>
#include <intuition/intuition.h>

#define DEBUG
#include "../../main/amigaos4/SDL_os4debug.h"

/* AmigaOS4 (compositing) renderer implementation

TODO:

- SDL_BlendMode_Mod: is it impossible to accelerate?
- Blended line drawing could probably be optimized

NOTE:

- compositing is used for blended rectangles and texture blitting
- blended lines and points are drawn with the CPU as compositing doesn't support these primitives
    (could try small triangles to plot a point?)
- texture color modulation is implemented by CPU

*/

static SDL_Renderer *OS4_CreateRenderer(SDL_Window * window, Uint32 flags);

static void OS4_WindowEvent(SDL_Renderer * renderer,
                           const SDL_WindowEvent *event);

static int OS4_GetOutputSize(SDL_Renderer * renderer, int *w, int *h);

static int OS4_UpdateViewport(SDL_Renderer * renderer);
static int OS4_UpdateClipRect(SDL_Renderer * renderer);

static int OS4_RenderClear(SDL_Renderer * renderer);

static int OS4_RenderFillRects(SDL_Renderer * renderer,
                              const SDL_FRect * rects, int count);
static int OS4_RenderCopy(SDL_Renderer * renderer, SDL_Texture * texture,
                         const SDL_Rect * srcrect, const SDL_FRect * dstrect);

static int OS4_RenderCopyEx(SDL_Renderer * renderer, SDL_Texture * texture,
                          const SDL_Rect * srcrect, const SDL_FRect * dstrect,
                          const double angle, const SDL_FPoint * center, const SDL_RendererFlip flip);

static int OS4_RenderReadPixels(SDL_Renderer * renderer, const SDL_Rect * rect,
                               Uint32 format, void * pixels, int pitch);

static void OS4_RenderPresent(SDL_Renderer * renderer);

static void OS4_DestroyRenderer(SDL_Renderer * renderer);

SDL_RenderDriver OS4_RenderDriver = {
    OS4_CreateRenderer,
    {
     "compositing",
     SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE | SDL_RENDERER_PRESENTVSYNC,
     1,
     {
      SDL_PIXELFORMAT_ARGB8888,
     },
     0,
     0}
};

typedef struct {
    float x, y;
    float s, t, w;
} OS4_Vertex;

static const uint16 OS4_QuadIndices[] = {
    0, 1, 2, 2, 3, 0
};

static SDL_bool
OS4_IsVsyncEnabled()
{
    const char *hint = SDL_GetHint(SDL_HINT_RENDER_VSYNC);

    if (hint && *hint == '1') {
        return SDL_TRUE;
    }

    return SDL_FALSE;
}

SDL_bool
OS4_IsColorModEnabled(SDL_Texture * texture)
{
    if ((texture->r & texture->g & texture->b) != 255) {
        //dprintf("Color mod enabled (%d, %d, %d)\n", r, g, b);
        return SDL_TRUE;
    }

    return SDL_FALSE;
}

struct BitMap *
OS4_AllocBitMap(SDL_Renderer * renderer, int width, int height, int depth) {
    OS4_RenderData *data = (OS4_RenderData *) renderer->driverdata;

    struct BitMap *bitmap = data->iGraphics->AllocBitMapTags(
        width,
        height,
        depth,
        BMATags_Displayable, TRUE,
        BMATags_PixelFormat, PIXF_A8R8G8B8,
        TAG_DONE);

    return bitmap;
}

struct BitMap *
OS4_ActivateRenderer(SDL_Renderer * renderer)
{
    OS4_RenderData *data = (OS4_RenderData *) renderer->driverdata;

    if (!data->target) {
        data->target = data->bitmap;
    }

    if (!data->target && renderer->window) {

        int width = renderer->window->w;
        int height = renderer->window->h;
        int depth = 32;

        dprintf("Allocating VRAM bitmap %d*%d*%d for renderer\n", width, height, depth);

        data->target = data->bitmap = OS4_AllocBitMap(renderer, width, height, depth);

        if (data->bitmap) {
            OS4_UpdateViewport(renderer);
            OS4_UpdateClipRect(renderer);
        } else {
            dprintf("Allocation failed\n");
        }
    }

    if (!data->solidcolor) {
        int width = 1;
        int height = 1;
        int depth = 32;

        data->solidcolor = OS4_AllocBitMap(renderer, width, height, depth);

        if (!data->solidcolor) {
            dprintf("Failed to allocate solid color bitmap\n");
        }
    }

    data->rastport.BitMap = data->target;

    return data->target;
}

SDL_Renderer *
OS4_CreateRenderer(SDL_Window * window, Uint32 flags)
{
    SDL_VideoData *videodata = (SDL_VideoData *)SDL_GetVideoDevice()->driverdata;
    SDL_Renderer *renderer;
    OS4_RenderData *data;

    dprintf("Creating renderer for '%s' (flags 0x%x)\n", window->title, flags);

    renderer = (SDL_Renderer *) SDL_calloc(1, sizeof(*renderer));
    if (!renderer) {
        SDL_OutOfMemory();
        return NULL;
    }

    data = (OS4_RenderData *) SDL_calloc(1, sizeof(*data));
    if (!data) {
        OS4_DestroyRenderer(renderer);
        SDL_OutOfMemory();
        return NULL;
    }

    renderer->WindowEvent = OS4_WindowEvent;
    renderer->GetOutputSize = OS4_GetOutputSize;
    renderer->CreateTexture = OS4_CreateTexture;
    renderer->SetTextureColorMod = OS4_SetTextureColorMod;
    renderer->SetTextureAlphaMod = OS4_SetTextureAlphaMod;
    renderer->SetTextureBlendMode = OS4_SetTextureBlendMode;
    renderer->UpdateTexture = OS4_UpdateTexture;
    renderer->LockTexture = OS4_LockTexture;
    renderer->UnlockTexture = OS4_UnlockTexture;
    renderer->SetRenderTarget = OS4_SetRenderTarget;
    renderer->UpdateViewport = OS4_UpdateViewport;
    renderer->UpdateClipRect = OS4_UpdateClipRect;
    renderer->RenderClear = OS4_RenderClear;
    renderer->RenderDrawPoints = OS4_RenderDrawPoints;
    renderer->RenderDrawLines = OS4_RenderDrawLines;
    renderer->RenderFillRects = OS4_RenderFillRects;
    renderer->RenderCopy = OS4_RenderCopy;
    renderer->RenderCopyEx = OS4_RenderCopyEx;
    renderer->RenderReadPixels = OS4_RenderReadPixels;
    renderer->RenderPresent = OS4_RenderPresent;
    renderer->DestroyTexture = OS4_DestroyTexture;
    renderer->DestroyRenderer = OS4_DestroyRenderer;
    renderer->info = OS4_RenderDriver.info;

    renderer->driverdata = data;

    data->iGraphics = videodata->iGraphics;
    data->iLayers = videodata->iLayers;

    data->iGraphics->InitRastPort(&data->rastport);

    dprintf("VSYNC: %s\n", OS4_IsVsyncEnabled() ? "on" : "off");

    return renderer;
}

static void
OS4_WindowEvent(SDL_Renderer * renderer, const SDL_WindowEvent *event)
{
    OS4_RenderData *data = (OS4_RenderData *) renderer->driverdata;

    dprintf("Called with event %d\n", event->event);

    if (event->event == SDL_WINDOWEVENT_SIZE_CHANGED) {

        /* Next time ActivateRenderer() is called, new bitmap will be created */
        if (data->bitmap) {

            dprintf("Freeing renderer bitmap %p\n", data->bitmap);

            data->iGraphics->FreeBitMap(data->bitmap);
            data->bitmap = NULL;
            data->target = NULL;
        }
    }
}

static int
OS4_GetOutputSize(SDL_Renderer * renderer, int *w, int *h)
{
    OS4_RenderData *data = (OS4_RenderData *) renderer->driverdata;

    struct BitMap * bitmap = OS4_ActivateRenderer(renderer);

    if (bitmap) {
        if (w) {
            *w = data->iGraphics->GetBitMapAttr(bitmap, BMA_WIDTH);
	        dprintf("w=%d\n", *w);
        }
        if (h) {
            *h = data->iGraphics->GetBitMapAttr(bitmap, BMA_HEIGHT);
			dprintf("h=%d\n", *h);
        }

        return 0;
    } else {
        SDL_SetError("OS4 renderer doesn't have an output bitmap");
        return -1;
    }
}

/* Special function to set our 1*1*32 bitmap */
static SDL_bool
OS4_SetSolidColor(SDL_Renderer * renderer, Uint32 color)
{
    OS4_RenderData *data = (OS4_RenderData *) renderer->driverdata;

    if (data->solidcolor) {
        APTR baseaddress;

        APTR lock = data->iGraphics->LockBitMapTags(
            data->solidcolor,
            LBM_BaseAddress, &baseaddress,
            TAG_DONE);

        if (lock) {
            *(Uint32 *)baseaddress = color;

            data->iGraphics->UnlockBitMap(data->solidcolor);

            return SDL_TRUE;
        } else {
            dprintf("Lock failed\n");
        }
    }

    return SDL_FALSE;
}

static SDL_bool
OS4_RectChanged(const SDL_Rect * first, const SDL_Rect * second)
{
    if (first->x != second->x ||
        first->y != second->y ||
        first->w != second->w ||
        first->h != second->h) {

        return SDL_TRUE;
    }

    return SDL_FALSE;
}

static int
OS4_UpdateViewport(SDL_Renderer * renderer)
{
    OS4_RenderData *data = (OS4_RenderData *) renderer->driverdata;
    SDL_Rect old;

    if (!data->bitmap) {
        /* We'll update the viewport after we recreate the surface */
        return 0;
    }

    old = data->cliprect;

    if (&renderer->viewport) {
        data->cliprect = renderer->viewport;
    } else {
        data->cliprect.x = 0;
        data->cliprect.y = 0;
        data->cliprect.w = renderer->window->w;
        data->cliprect.h = renderer->window->h;
    }

    if (OS4_RectChanged(&old, &data->cliprect)) {
        dprintf("Cliprect: (%d,%d) - %d*%d\n",
            data->cliprect.x, data->cliprect.y, data->cliprect.w, data->cliprect.h);
    }

    return 0;
}

static int
OS4_UpdateClipRect(SDL_Renderer * renderer)
{
    OS4_RenderData *data = (OS4_RenderData *) renderer->driverdata;

    if (data->bitmap) {
        const SDL_Rect *rect = &renderer->clip_rect;

        SDL_Rect old = data->cliprect;

        if (rect && !SDL_RectEmpty(rect)) {
            data->cliprect = *rect;
        } else {
            data->cliprect.x = 0;
            data->cliprect.y = 0;
            data->cliprect.w = renderer->window->w;
            data->cliprect.h = renderer->window->h;
        }

        if (OS4_RectChanged(&old, &data->cliprect)) {
            dprintf("Cliprect: (%d,%d) - %d*%d\n",
                data->cliprect.x, data->cliprect.y, data->cliprect.w, data->cliprect.h);
        }
    }

    return 0;
}

static int
OS4_RenderClear(SDL_Renderer * renderer)
{
    Uint32 color;

    struct BitMap *bitmap = OS4_ActivateRenderer(renderer);

    OS4_RenderData *data = (OS4_RenderData *) renderer->driverdata;
    //Sint32 s = SDL_GetTicks();
    //dprintf("Called\n");

    if (!bitmap) {
        return -1;
    }

    color = renderer->a << 24 |
            renderer->r << 16 |
            renderer->g << 8 |
            renderer->b;

    data->iGraphics->RectFillColor(
        &data->rastport,
        0,
        0,
        renderer->window->w - 1,
        renderer->window->h - 1,
        color); // graphics.lib v54!

    //dprintf("Took %d\n", SDL_GetTicks() - s);

    return 0;
}

static int
OS4_GetScaleQuality(void)
{
    const char *hint = SDL_GetHint(SDL_HINT_RENDER_SCALE_QUALITY);

    if (!hint || *hint == '0' || SDL_strcasecmp(hint, "nearest") == 0) {
        return 0;
    } else {
        return 1;
    }
}

static uint32
OS4_ConvertBlendMode(SDL_BlendMode mode)
{
    switch (mode) {
        case SDL_BLENDMODE_NONE:
            return COMPOSITE_Src;
        case SDL_BLENDMODE_BLEND:
            return COMPOSITE_Src_Over_Dest;
        case SDL_BLENDMODE_ADD:
            return COMPOSITE_Plus;
        case SDL_BLENDMODE_MOD:
            // This is not correct, but we can't do modulation at the moment
            return COMPOSITE_Src_Over_Dest;
        default:
            dprintf("Unknown blend mode %d\n", mode);
            return COMPOSITE_Src_Over_Dest;
    }
}

static uint32
OS4_GetCompositeFlags(SDL_BlendMode mode)
{
    uint32 flags = COMPFLAG_IgnoreDestAlpha | COMPFLAG_HardwareOnly;

    if (OS4_GetScaleQuality()) {
        flags |= COMPFLAG_SrcFilter;
    }

    if (mode == SDL_BLENDMODE_NONE) {
        flags |= COMPFLAG_SrcAlphaOverride;
    }

    return flags;
}

static float
OS4_GetCompositeAlpha(SDL_Texture * texture)
{
    float alpha;

    if (texture->blendMode == SDL_BLENDMODE_NONE) {
        alpha = 1.0f;
    } else {
        alpha = (float)texture->a / 255.0f;
    }

    return alpha;
}

static void
OS4_RotateVertices(OS4_Vertex vertices[4], const double angle, const SDL_FPoint * center)
{
    int i;

    float rads = angle * M_PI / 180.0f;

    float sina = SDL_sinf(rads);
    float cosa = SDL_cosf(rads);

    for (i = 0; i < 4; ++i) {
        float x = vertices[i].x - center->x;
        float y = vertices[i].y - center->y;

        vertices[i].x = x * cosa - y * sina + center->x;
        vertices[i].y = x * sina + y * cosa + center->y;
    }
}

static void
OS4_FillVertexData(OS4_Vertex vertices[4], const SDL_Rect * srcrect, const SDL_Rect * dstrect,
    const double angle, const SDL_FPoint * center, const SDL_RendererFlip flip)
{
    /* Flip texture coordinates if needed */

    Uint16 left, right, top, bottom, tmp;

    left = srcrect->x;
    right = left + srcrect->w - 1;
    top = srcrect->y;
    bottom = top + srcrect->h - 1;

    if (flip & SDL_FLIP_HORIZONTAL) {
        tmp = left;
        left = right;
        right = tmp;
    }

    if (flip & SDL_FLIP_VERTICAL) {
        tmp = bottom;
        bottom = top;
        top = tmp;
    }

    /*

    Plan is to draw quad with two triangles:

    v0-v3
    | \ |
    v1-v2

    */

    vertices[0].x = dstrect->x;
    vertices[0].y = dstrect->y;
    vertices[0].s = left;
    vertices[0].t = top;
    vertices[0].w = 1.0f;

    vertices[1].x = dstrect->x;
    vertices[1].y = dstrect->y + dstrect->h - 1;
    vertices[1].s = left;
    vertices[1].t = bottom;
    vertices[1].w = 1.0f;

    vertices[2].x = dstrect->x + dstrect->w - 1;
    vertices[2].y = dstrect->y + dstrect->h - 1;
    vertices[2].s = right;
    vertices[2].t = bottom;
    vertices[2].w = 1.0f;

    vertices[3].x = dstrect->x + dstrect->w - 1;
    vertices[3].y = dstrect->y;
    vertices[3].s = right;
    vertices[3].t = top;
    vertices[3].w = 1.0f;

    if (angle != 0.0) {
        OS4_RotateVertices(vertices, angle, center);
    }
}

static int
OS4_RenderFillRects(SDL_Renderer * renderer, const SDL_FRect * rects, int count)
{
    OS4_RenderData *data = (OS4_RenderData *) renderer->driverdata;
    struct BitMap *bitmap = OS4_ActivateRenderer(renderer);
    SDL_Rect *final_rects;
    int i, status;

    //dprintf("Called for %d rects\n", count);
    //Sint32 s = SDL_GetTicks();

    if (!bitmap) {
        return -1;
    }

    final_rects = SDL_stack_alloc(SDL_Rect, count);
    if (!final_rects) {
        return SDL_OutOfMemory();
    }
    if (renderer->viewport.x || renderer->viewport.y) {
        int x = renderer->viewport.x;
        int y = renderer->viewport.y;

        for (i = 0; i < count; ++i) {
            final_rects[i].x = (int)(x + rects[i].x);
            final_rects[i].y = (int)(y + rects[i].y);
            final_rects[i].w = SDL_max((int)rects[i].w, 1);
            final_rects[i].h = SDL_max((int)rects[i].h, 1);
        }
    } else {
        for (i = 0; i < count; ++i) {
            final_rects[i].x = (int)rects[i].x;
            final_rects[i].y = (int)rects[i].y;
            final_rects[i].w = SDL_max((int)rects[i].w, 1);
            final_rects[i].h = SDL_max((int)rects[i].h, 1);
        }
    }

    if (renderer->blendMode == SDL_BLENDMODE_NONE) {

        Uint32 color =
            renderer->a << 24 |
            renderer->r << 16 |
            renderer->g << 8 |
            renderer->b;

        for (i = 0; i < count; ++i) {

            SDL_Rect clipped;
            //dprintf("%d, %d - %d, %d\n", final_rects[i].x, final_rects[i].y, final_rects[i].w, final_rects[i].h);

            /* Perform clipping - is it possible to use RastPort? */
            if (!SDL_IntersectRect(&final_rects[i], &data->cliprect, &clipped)) {
                continue;
            }

            data->iGraphics->RectFillColor(
                &data->rastport,
                clipped.x,
                clipped.y,
                clipped.x + clipped.w - 1,
                clipped.y + clipped.h - 1,
                color); // graphics.lib v54!
        }

        status = 0;
    } else {

        Uint32 colormod;

        if (!data->solidcolor) {
            SDL_stack_free(final_rects);
            return -1;
        }

        colormod = renderer->a << 24 | renderer->r << 16 | renderer->g << 8 | renderer->b;

        // Color modulation is implemented through fill texture manipulation
        if (!OS4_SetSolidColor(renderer, colormod)) {
            SDL_stack_free(final_rects);
            return -1;
        }

        /* TODO: batch */
        for (i = 0; i < count; ++i) {

            SDL_Rect srcrect = { 0, 0, 1, 1 };

            OS4_Vertex vertices[4];

            uint32 ret_code;

            OS4_FillVertexData(vertices, &srcrect, &final_rects[i], 0.0, NULL, SDL_FLIP_NONE);

            ret_code = data->iGraphics->CompositeTags(
                OS4_ConvertBlendMode(renderer->blendMode),
                data->solidcolor,
                bitmap,
                COMPTAG_DestX,      data->cliprect.x,
                COMPTAG_DestY,      data->cliprect.y,
                COMPTAG_DestWidth,  data->cliprect.w,
                COMPTAG_DestHeight, data->cliprect.h,
                COMPTAG_Flags,      OS4_GetCompositeFlags(renderer->blendMode),
                COMPTAG_VertexArray, vertices,
                COMPTAG_VertexFormat, COMPVF_STW0_Present,
                COMPTAG_NumTriangles, 2,
                COMPTAG_IndexArray, OS4_QuadIndices,
                TAG_END);

            if (ret_code) {
                static Uint32 counter;

                if ((counter++ % 100) == 0) {
                    dprintf("CompositeTags: %d (fails: %u)\n", ret_code, counter);
                }
            }
        }

        status = 0;
    }
    SDL_stack_free(final_rects);
    //dprintf("Took %d\n", SDL_GetTicks() - s);

    return status;
}

static int
OS4_RenderCopy(SDL_Renderer * renderer, SDL_Texture * texture,
              const SDL_Rect * srcrect, const SDL_FRect * dstrect)
{
    OS4_RenderData *data = (OS4_RenderData *) renderer->driverdata;
    OS4_TextureData *texturedata = (OS4_TextureData *) texture->driverdata;

    struct BitMap *dst = OS4_ActivateRenderer(renderer);
    struct BitMap *src = OS4_IsColorModEnabled(texture) ?
        texturedata->finalbitmap : texturedata->bitmap;

    SDL_Rect final_rect;

    float scalex, scaley;
    uint32 ret_code;

    //dprintf("Called\n");
    //Sint32 s = SDL_GetTicks();
    if (!dst) {
        return -1;
    }

    if (renderer->viewport.x || renderer->viewport.y) {
        final_rect.x = (int)(renderer->viewport.x + dstrect->x);
        final_rect.y = (int)(renderer->viewport.y + dstrect->y);
    } else {
        final_rect.x = (int)dstrect->x;
        final_rect.y = (int)dstrect->y;
    }
    final_rect.w = (int)dstrect->w;
    final_rect.h = (int)dstrect->h;

    scalex = srcrect->w ? (float)final_rect.w / srcrect->w : 1.0f;
    scaley = srcrect->h ? (float)final_rect.h / srcrect->h : 1.0f;

    ret_code = data->iGraphics->CompositeTags(
        OS4_ConvertBlendMode(texture->blendMode),
        src,
        dst,
        COMPTAG_SrcAlpha,   COMP_FLOAT_TO_FIX(OS4_GetCompositeAlpha(texture)),
        COMPTAG_SrcX,       srcrect->x,
        COMPTAG_SrcY,       srcrect->y,
        COMPTAG_SrcWidth,   srcrect->w,
        COMPTAG_SrcHeight,  srcrect->h,
        COMPTAG_OffsetX,    final_rect.x,
        COMPTAG_OffsetY,    final_rect.y,
        COMPTAG_ScaleX,     COMP_FLOAT_TO_FIX(scalex),
        COMPTAG_ScaleY,     COMP_FLOAT_TO_FIX(scaley),
        COMPTAG_DestX,      data->cliprect.x,
        COMPTAG_DestY,      data->cliprect.y,
        COMPTAG_DestWidth,  data->cliprect.w,
        COMPTAG_DestHeight, data->cliprect.h,
        COMPTAG_Flags,      OS4_GetCompositeFlags(texture->blendMode),
        TAG_END);

    if (ret_code) {
        static Uint32 counter;

        if ((counter++ % 100) == 0) {
            dprintf("CompositeTags: %d (fails: %u)\n", ret_code, counter);
        }

        return SDL_SetError("CompositeTags failed");
    }

    //dprintf("Took %d\n", SDL_GetTicks() - s);

    return 0;
}

static int
OS4_RenderCopyEx(SDL_Renderer * renderer, SDL_Texture * texture,
                const SDL_Rect * srcrect, const SDL_FRect * dstrect,
                const double angle, const SDL_FPoint * center, const SDL_RendererFlip flip)
{
    OS4_RenderData *data = (OS4_RenderData *) renderer->driverdata;
    OS4_TextureData *texturedata = (OS4_TextureData *) texture->driverdata;

    struct BitMap *dst = OS4_ActivateRenderer(renderer);
    struct BitMap *src = OS4_IsColorModEnabled(texture) ?
        texturedata->finalbitmap : texturedata->bitmap;

    SDL_Rect final_rect;
    SDL_FPoint final_center;

    uint32 ret_code;

    OS4_Vertex vertices[4];

    if (!dst) {
        return -1;
    }

    if (renderer->viewport.x || renderer->viewport.y) {
        final_rect.x = (int)(renderer->viewport.x + dstrect->x);
        final_rect.y = (int)(renderer->viewport.y + dstrect->y);
    } else {
        final_rect.x = (int)dstrect->x;
        final_rect.y = (int)dstrect->y;
    }
    final_rect.w = (int)dstrect->w;
    final_rect.h = (int)dstrect->h;

    final_center.x = dstrect->x + center->x;
    final_center.y = dstrect->y + center->y;

    OS4_FillVertexData(vertices, srcrect, &final_rect, angle, &final_center, flip);

    ret_code = data->iGraphics->CompositeTags(
        OS4_ConvertBlendMode(texture->blendMode),
        src,
        dst,
        COMPTAG_SrcAlpha,   COMP_FLOAT_TO_FIX(OS4_GetCompositeAlpha(texture)),
        COMPTAG_DestX,      data->cliprect.x,
        COMPTAG_DestY,      data->cliprect.y,
        COMPTAG_DestWidth,  data->cliprect.w,
        COMPTAG_DestHeight, data->cliprect.h,
        COMPTAG_Flags,      OS4_GetCompositeFlags(texture->blendMode),
        COMPTAG_VertexArray, vertices,
        COMPTAG_VertexFormat, COMPVF_STW0_Present,
        COMPTAG_NumTriangles, 2,
        COMPTAG_IndexArray, OS4_QuadIndices,
        TAG_END);

    if (ret_code) {
        static Uint32 counter;

        if ((counter++ % 100) == 0) {
            dprintf("CompositeTags: %d (fails: %u)\n", ret_code, counter);
        }

        return SDL_SetError("CompositeTags failed");
    }

    return 0;
}

static int
OS4_RenderReadPixels(SDL_Renderer * renderer, const SDL_Rect * rect,
                    Uint32 format, void * pixels, int pitch)
{
    OS4_RenderData *data = (OS4_RenderData *) renderer->driverdata;
    SDL_Rect final_rect;

    struct BitMap *bitmap = OS4_ActivateRenderer(renderer);

    //dprintf("Called\n");

    if (!bitmap) {
        return -1;
    }

    if (renderer->viewport.x || renderer->viewport.y) {
        final_rect.x = renderer->viewport.x + rect->x;
        final_rect.y = renderer->viewport.y + rect->y;
        final_rect.w = rect->w;
        final_rect.h = rect->h;
        rect = &final_rect;
    }

    if (rect->x < 0 || rect->x+rect->w > renderer->window->w ||
        rect->y < 0 || rect->y+rect->h > renderer->window->h) {
        return SDL_SetError("Tried to read outside of surface bounds");
    }

    if (format != SDL_PIXELFORMAT_ARGB8888) {
        return SDL_SetError("Unsupported pixel format");
    }

    data->iGraphics->ReadPixelArray(
        &data->rastport,
        rect->x,
        rect->y,
        pixels,
        0,
        0,
        pitch,
        PIXF_A8R8G8B8,
        rect->w,
        rect->h);

    return 0;
}

static void
OS4_RenderPresent(SDL_Renderer * renderer)
{
    SDL_Window *window = renderer->window;
    struct BitMap *source = OS4_ActivateRenderer(renderer);

    //dprintf("Called\n");
    //Uint32 s = SDL_GetTicks();

    if (window && source) {
        OS4_RenderData *data = (OS4_RenderData *)renderer->driverdata;

        // TODO: should we take viewport into account?

        SDL_WindowData *windowdata = (SDL_WindowData *)window->driverdata;

        struct Window *syswin = windowdata->syswin;

        if (syswin) {

            int32 ret;
            //dprintf("target %p\n", data->target);

            if (OS4_IsVsyncEnabled()) {
                data->iGraphics->WaitTOF();
            }

            data->iLayers->LockLayer(0, syswin->WLayer);

            ret = data->iGraphics->BltBitMapTags(
                BLITA_Source, source,
                BLITA_DestType, BLITT_RASTPORT,
                BLITA_Dest, syswin->RPort,
                BLITA_DestX, syswin->BorderLeft,
                BLITA_DestY, syswin->BorderTop,
                BLITA_Width, window->w,
                BLITA_Height, window->h,
                TAG_DONE);

            data->iLayers->UnlockLayer(syswin->WLayer);

            if (ret != -1) {
                dprintf("BltBitMapTags(): %d\n", ret);
            }
        }
    }
    //dprintf("Took %d\n", SDL_GetTicks() - s);
}

static void
OS4_DestroyRenderer(SDL_Renderer * renderer)
{
    OS4_RenderData *data = (OS4_RenderData *) renderer->driverdata;

    if (data->bitmap) {
        dprintf("Freeing renderer bitmap %p\n", data->bitmap);

        data->iGraphics->FreeBitMap(data->bitmap);
        data->bitmap = NULL;
    }

    if (data->solidcolor) {
        data->iGraphics->FreeBitMap(data->solidcolor);
        data->solidcolor = NULL;
    }

    SDL_free(data);
    SDL_free(renderer);
}

#endif /* !SDL_RENDER_DISABLED */

/* vi: set ts=4 sw=4 expandtab: */

