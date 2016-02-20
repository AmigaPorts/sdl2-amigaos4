/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2014 Sam Lantinga <slouken@libsdl.org>

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

#include "SDL_os4video.h"

#define DEBUG
#include "../../main/amigaos4/SDL_os4debug.h"

static PIX_FMT OS4_DepthToPixf(int depth)
{
	switch (depth) {
		case 32: return PIXF_A8R8G8B8;
		case 16: return PIXF_R5G6B5;
		case 8: return PIXF_CLUT;
		default: return PIXF_NONE;
	}
}

static Uint32 OS4_PixfToSdlPixelFormat(PIX_FMT from)
{
	switch (from)
	{
		case PIXF_A8R8G8B8: return SDL_PIXELFORMAT_ARGB8888;
		case PIXF_R5G6B5: return SDL_PIXELFORMAT_RGB565;
		case PIXF_CLUT: return SDL_PIXELFORMAT_INDEX8;
		default: return SDL_PIXELFORMAT_UNKNOWN;
	}
}

int
OS4_CreateWindowFramebuffer(_THIS, SDL_Window * window, Uint32 * format, void ** pixels, int *pitch)
{
	SDL_WindowData * data = window->driverdata;

	if (data) {
		APTR lock;
		APTR base_address;
		uint32 bytes_per_row;
		uint32 depth;
		PIX_FMT pixf;

		if (data->bitmap) {

			dprintf("Freeing old bitmap %p\n", data->bitmap);

			IGraphics->FreeBitMap(data->bitmap);
		}

		depth = IGraphics->GetBitMapAttr(data->syswin->RPort->BitMap, BMA_BITSPERPIXEL);
		pixf = OS4_DepthToPixf(depth);

		dprintf("Allocating %d*%d*%d bitmap\n", window->w, window->h, depth);

		data->bitmap = IGraphics->AllocBitMapTags(
			window->w,
			window->h,
			depth,
			BMATags_Clear, TRUE,
			BMATags_UserPrivate, TRUE,
			//BMATags_Friend, data->syswin->RPort->BitMap,
			BMATags_PixelFormat, pixf,
			TAG_DONE);

		if (!data->bitmap) {
			dprintf("Failed to allocate bitmap\n");
			SDL_SetError("Failed to allocate bitmap for framebuffer");

			return -1;
		}

		*format = OS4_PixfToSdlPixelFormat(pixf);

		dprintf("Native format %d, SDL format %d\n", pixf, *format);

		/* Lock the bitmap to get details. Since it's user private,
		it should be safe to cache address and pitch. */
		lock = IGraphics->LockBitMapTags(
		    data->bitmap,
			LBM_BaseAddress, &base_address,
			LBM_BytesPerRow, &bytes_per_row,
			TAG_DONE);

		if (lock) {
			*pixels = base_address;
			*pitch = bytes_per_row;

			IGraphics->UnlockBitMap(lock);
		} else {
			dprintf("Failed to lock bitmap\n");
			SDL_SetError("Failed to lock framebuffer bitmap");

			IGraphics->FreeBitMap(data->bitmap);
			data->bitmap = NULL;
			
			return -1;
		}
	}
	
	return 0;
}

#ifndef MIN
#   define MIN(x,y) ((x)<(y)?(x):(y))
#endif

int
OS4_UpdateWindowFramebuffer(_THIS, SDL_Window * window, const SDL_Rect * rects, int numrects)
{
	SDL_WindowData * data = window->driverdata;

	dprintf("Called\n");

	if (data && data->bitmap) {
		if (data->syswin) {

			int i;

			struct Window * syswin = data->syswin;

			const struct IBox windowBox = {
				syswin->BorderLeft,
				syswin->BorderTop,
				syswin->Width - syswin->BorderLeft - syswin->BorderRight,
				syswin->Height - syswin->BorderTop - syswin->BorderBottom };

			//dprintf("blit box %d*%d\n", windowBox.Width, windowBox.Height);

			ILayers->LockLayer(0, syswin->WLayer);

			for (i = 0; i < numrects; ++i) {
				const SDL_Rect * r = &rects[i];

				int32 ret = IGraphics->BltBitMapTags(
					BLITA_Source, data->bitmap,
					//BLITA_SrcType, BLITT_BITMAP,
					BLITA_Dest, syswin->RPort,
					BLITA_DestType, BLITT_RASTPORT,
					BLITA_SrcX, r->x,
					BLITA_SrcY, r->y,
					BLITA_DestX, r->x + windowBox.Left,
					BLITA_DestY, r->y + windowBox.Top,
					BLITA_Width, MIN(r->w, windowBox.Width),
					BLITA_Height, MIN(r->h, windowBox.Height),
					TAG_DONE);
			
				if (ret != -1) {
					dprintf("BltBitMapTags() returned %d\n", ret);
				}
			}

			ILayers->UnlockLayer(syswin->WLayer);
		}
	}

	return 0;
}

void
OS4_DestroyWindowFramebuffer(_THIS, SDL_Window * window)
{
	SDL_WindowData * data = window->driverdata;

	if (data && data->bitmap) {

		dprintf("Freeing bitmap %p\n", data->bitmap);

		IGraphics->FreeBitMap(data->bitmap);
		data->bitmap = NULL;
	}
}

#endif /* SDL_VIDEO_DRIVER_AMIGAOS4 */

/* vi: set ts=4 sw=4 expandtab: */