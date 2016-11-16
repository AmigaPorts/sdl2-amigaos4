/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2016 Sam Lantinga <slouken@libsdl.org>

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
#include "SDL_os4opengl.h"
#include "SDL_os4shape.h"

#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_windowevents_c.h"
#include "../../events/scancodes_amiga.h"

//#define DEBUG
#include "../../main/amigaos4/SDL_os4debug.h"

struct MyIntuiMessage
{
	uint32 Class;
	uint16 Code;
	uint16 Qualifier;

	struct Gadget *Gadget;

	struct Window *IDCMPWindow;

	int16  PointerX;	/* Absolute pointer position, relative to */
	int16  PointerY;	/* top-left corner of inner window */

	int16  Width;		/* Inner window dimensions */
	int16  Height;
};

typedef struct HitTestInfo
{
	SDL_HitTestResult htr;
	SDL_Point point;
} HitTestInfo;

static HitTestInfo hti; // TODO: move to window data?

/* We could possibly use also Window.userdata field to contain SDL_Window,
and thus avoid searching */
static SDL_Window *
OS4_FindWindow(_THIS, struct Window * syswin)
{
	SDL_Window * sdlwin;

	for (sdlwin = _this->windows; sdlwin; sdlwin = sdlwin->next) {

		SDL_WindowData *data = sdlwin->driverdata;

		if (data->syswin == syswin) {

			//dprintf("Found window %p\n", syswin);

			return sdlwin;
		}
	}

	dprintf("No window found\n");

	return NULL;
}

static char
OS4_TranslateUnicode(_THIS, uint16 Code, uint32 Qualifier)
{
	struct InputEvent ie;
	uint16 res;
	char buffer[10];

	ie.ie_Class = IECLASS_RAWKEY;
	ie.ie_SubClass = 0;
	ie.ie_Code  = Code & ~(IECODE_UP_PREFIX);
	ie.ie_Qualifier = Qualifier;
	ie.ie_EventAddress = NULL;

	res = IKeymap->MapRawKey(&ie, buffer, sizeof(buffer), 0);

	if (res != 1)
	    return 0;
	else
	    return buffer[0];
}

static void
OS4_HandleKeyboard(_THIS, struct MyIntuiMessage *imsg)
{
	if ((imsg->Qualifier & IEQUALIFIER_REPEAT) == 0) {

		uint8 rawkey = imsg->Code & 0x7F;

		if (rawkey < sizeof(amiga_scancode_table) / sizeof(amiga_scancode_table[0])) {

			SDL_Scancode s = amiga_scancode_table[rawkey];

			if (imsg->Code <= 127) {

				char text[2];

				text[0]	= OS4_TranslateUnicode(_this, imsg->Code, imsg->Qualifier);
				text[1] = '\0';

				SDL_SendKeyboardKey(SDL_PRESSED, s);
				SDL_SendKeyboardText(text);
			} else {
				SDL_SendKeyboardKey(SDL_RELEASED, s);
			}
		}
	}
}

static void
OS4_HandleHitTestMotion(_THIS, SDL_Window * sdlwin, struct MyIntuiMessage *imsg)
{
	int16 newx = imsg->IDCMPWindow->WScreen->MouseX; // TODO: MyIntuiMsg?
	int16 newy = imsg->IDCMPWindow->WScreen->MouseY;

	int16 deltax = newx - hti.point.x;
	int16 deltay = newy - hti.point.y;

	int16 x, y, w, h;

	if (deltax == 0 && deltay == 0) {
		return;
	}

	x = sdlwin->x;
	y = sdlwin->y;
	w = sdlwin->w;
	h = sdlwin->h;

	hti.point.x = newx;
	hti.point.y = newy;

	switch (hti.htr) {
		case SDL_HITTEST_DRAGGABLE:
			x += deltax;
			y += deltay;
		    break;

		case SDL_HITTEST_RESIZE_TOPLEFT:
			x += deltax;
			y += deltay;
			w -= deltax;
			h -= deltay;
			break;

		case SDL_HITTEST_RESIZE_TOP:
			y += deltay;
			h -= deltay;
		    break;

		case SDL_HITTEST_RESIZE_TOPRIGHT:
			y += deltay;
			w += deltax;
			h -= deltay;
			break;

		case SDL_HITTEST_RESIZE_RIGHT:
			w += deltax;
		    break;

		case SDL_HITTEST_RESIZE_BOTTOMRIGHT:
			w += deltax;
			h += deltay;
			break;

		case SDL_HITTEST_RESIZE_BOTTOM:
			h += deltay;
		    break;

		case SDL_HITTEST_RESIZE_BOTTOMLEFT:
			x += deltax;

			w -= deltax;
			h += deltay;
			break;

		case SDL_HITTEST_RESIZE_LEFT:
			x += deltax;
			w -= deltax;
		    break;

		default:
		    break;
	}

	dprintf("newx %d, newy %d (dx %d, dy %d) w=%d h=%d\n", newx, newy, deltax, deltay, w, h);

	sdlwin->x = x;
	sdlwin->y = y;
	sdlwin->w = w;
	sdlwin->h = h;

	LONG ret = IIntuition->SetWindowAttrs(imsg->IDCMPWindow,
		WA_InnerWidth, w,
		WA_InnerHeight, h,
		WA_Left, x,
		WA_Top, y,
		TAG_DONE);

	if (ret) {
		dprintf("SetWindowAttrs() returned %d\n", ret);
	}
}

static void
OS4_HandleMouseMotion(_THIS, struct MyIntuiMessage *imsg)
{
	SDL_Window * sdlwin = OS4_FindWindow(_this, imsg->IDCMPWindow);

	if (sdlwin) {
		dprintf("X:%d Y:%d\n", imsg->PointerX, imsg->PointerY);

		SDL_SendMouseMotion(sdlwin, 0 /*mouse->mouseID*/, 0, imsg->PointerX, imsg->PointerY);

		if (hti.htr != SDL_HITTEST_NORMAL) {
			OS4_HandleHitTestMotion(_this, sdlwin, imsg);
		}
	}
}

static SDL_bool OS4_HandleHitTest(_THIS, SDL_Window * sdlwin, struct MyIntuiMessage *imsg)
{
	if (sdlwin->hit_test) {
		const SDL_Point point = { imsg->PointerX, imsg->PointerY };
		const SDL_HitTestResult rc = sdlwin->hit_test(sdlwin, &point, sdlwin->hit_test_data);

		switch (rc) {
			case SDL_HITTEST_DRAGGABLE:
			case SDL_HITTEST_RESIZE_TOPLEFT:
			case SDL_HITTEST_RESIZE_TOP:
			case SDL_HITTEST_RESIZE_TOPRIGHT:
			case SDL_HITTEST_RESIZE_RIGHT:
			case SDL_HITTEST_RESIZE_BOTTOMRIGHT:
			case SDL_HITTEST_RESIZE_BOTTOM:
			case SDL_HITTEST_RESIZE_BOTTOMLEFT:
			case SDL_HITTEST_RESIZE_LEFT:

				// Store the action and mouse coordinates for later use
				hti.htr = rc;
				hti.point.x = imsg->IDCMPWindow->WScreen->MouseX;
				hti.point.y = imsg->IDCMPWindow->WScreen->MouseY;

				return SDL_TRUE;

			default: return SDL_FALSE;
		}
	}

	return SDL_FALSE;
}

static void
OS4_HandleMouseButtons(_THIS, struct MyIntuiMessage *imsg)
{
	SDL_Window * sdlwin = OS4_FindWindow(_this, imsg->IDCMPWindow);

	if (sdlwin) {
		uint8 button = 0;
		uint8 state = (imsg->Code & IECODE_UP_PREFIX) ? SDL_RELEASED : SDL_PRESSED;

		if ((imsg->Code & ~IECODE_UP_PREFIX) == IECODE_LBUTTON) {
			if (state == SDL_PRESSED) {
				if (OS4_HandleHitTest(_this, sdlwin, imsg)) {
				    return;
				}
			} else {
				hti.htr = SDL_HITTEST_NORMAL;
				// TODO: send window resized event to SDL?
		    }
			button = SDL_BUTTON_LEFT;
		} else if ((imsg->Code & ~IECODE_UP_PREFIX) == IECODE_RBUTTON)
			button = SDL_BUTTON_RIGHT;
		else if ((imsg->Code & ~IECODE_UP_PREFIX) == IECODE_MBUTTON)
			button = SDL_BUTTON_MIDDLE;

		// TODO: can we support more buttons?

		dprintf("X:%d Y:%d button:%d state:%d\n",
			imsg->PointerX, imsg->PointerY, button, state);

		SDL_SendMouseButton(sdlwin, 0, state, button);
	}
}

static void
OS4_HandleMouseWheel(_THIS, struct MyIntuiMessage *imsg)
{
	SDL_Window * sdlwin = OS4_FindWindow(_this, imsg->IDCMPWindow);

	if (sdlwin) {
		struct IntuiWheelData *data = (struct IntuiWheelData *)imsg->Gadget;

		if (data->WheelY < 0) {
			SDL_SendMouseWheel(sdlwin, 0, 0, 1, SDL_MOUSEWHEEL_NORMAL);
		} else if (data->WheelY > 0) {
			SDL_SendMouseWheel(sdlwin, 0, 0, -1, SDL_MOUSEWHEEL_NORMAL);
		}

		if (data->WheelX < 0) {
			SDL_SendMouseWheel(sdlwin, 0, 1, 0, SDL_MOUSEWHEEL_NORMAL);
		} else if (data->WheelX > 0) {
			SDL_SendMouseWheel(sdlwin, 0, -1, 0, SDL_MOUSEWHEEL_NORMAL);
		}
	}
}


static void
OS4_HandleResize(_THIS, struct MyIntuiMessage *imsg)
{
	if (hti.htr == SDL_HITTEST_NORMAL) {
		SDL_Window * sdlwin = OS4_FindWindow(_this, imsg->IDCMPWindow);

		dprintf("Window resized to %d*%d\n", imsg->Width, imsg->Height);

		if (sdlwin) {
			if (imsg->Width != sdlwin->w || imsg->Height != sdlwin->h) {
				SDL_WindowData * data = (SDL_WindowData *)sdlwin->driverdata;

				SDL_SendWindowEvent(sdlwin, SDL_WINDOWEVENT_RESIZED,
					imsg->Width, imsg->Height);

				if (SDL_IsShapedWindow(sdlwin)) {
					OS4_ResizeWindowShape(sdlwin);
	        	}

				if (data->IGL /*sdlwin->flags & SDL_WINDOW_OPENGL*/ ) {
					OS4_GL_ResizeContext(_this, sdlwin);
				}
			}
		}
	} else {
		dprintf("Resize notification ignored because resize is still in progress\n");
	}
}

static void
OS4_HandleActivation(_THIS, struct MyIntuiMessage *imsg, SDL_bool activated)
{
	SDL_Window * sdlwin = OS4_FindWindow(_this, imsg->IDCMPWindow);

	if (sdlwin) {
		if (activated) {
			SDL_SendWindowEvent(sdlwin, SDL_WINDOWEVENT_SHOWN, 0, 0);
			if (SDL_GetKeyboardFocus() != sdlwin) {
				SDL_SetKeyboardFocus(sdlwin);
				// TODO: do we want to set mouse colors as in SDL1?
			}
		} else {
			if (SDL_GetKeyboardFocus() == sdlwin) {
				SDL_SetKeyboardFocus(NULL);
				// TODO: do we want to reset mouse colors as in SDL1?
			}
		}

		dprintf("Window %p activation %d\n", sdlwin, activated);
	}
}

static void
OS4_HandleClose(_THIS, struct MyIntuiMessage *imsg)
{
	SDL_Window * sdlwin = OS4_FindWindow(_this, imsg->IDCMPWindow);

	if (sdlwin) {
	   SDL_SendWindowEvent(sdlwin, SDL_WINDOWEVENT_CLOSE, 0, 0);
	}
}

static void
OS4_HandleTicks(_THIS, struct MyIntuiMessage *imsg)
{
	SDL_Window * sdlwin = OS4_FindWindow(_this, imsg->IDCMPWindow);

	if (sdlwin) {
		if ((sdlwin->flags & SDL_WINDOW_INPUT_GRABBED) && !(sdlwin->flags & SDL_WINDOW_FULLSCREEN) &&
			(SDL_GetKeyboardFocus() == sdlwin)) {

			SDL_WindowData *data = sdlwin->driverdata;

			dprintf("Window %p ticks %d\n", imsg->IDCMPWindow, data->pointerGrabTicks);

			// Re-grab the window after our ticks have passed
			if (++data->pointerGrabTicks >= POINTER_GRAB_TIMEOUT) {
				data->pointerGrabTicks = 0;

				OS4_SetWindowGrabInternal(_this, imsg->IDCMPWindow, TRUE);
			}
		}
	}
}

static void
OS4_CopyRelevantFields(struct IntuiMessage * src, struct MyIntuiMessage * dst)
{
	/* Copy relevant fields. This makes it safer if the window goes away during
	  this loop (re-open due to keystroke) */
	dst->Class 			 = src->Class;
	dst->Code 			 = src->Code;
	dst->Qualifier 		 = src->Qualifier;

	dst->Gadget			 = (struct Gadget *) src->IAddress;

	dst->IDCMPWindow     = src->IDCMPWindow;

	/* The window's MouseX/Y fields, however, always contain the absolute pointer
	* position (relative to the window's upper-left corner). */
	dst->PointerX		 = src->IDCMPWindow->MouseX - src->IDCMPWindow->BorderLeft;
	dst->PointerY		 = src->IDCMPWindow->MouseY - src->IDCMPWindow->BorderTop;

	dst->Width 			 = src->IDCMPWindow->Width  - src->IDCMPWindow->BorderLeft - src->IDCMPWindow->BorderRight;
	dst->Height 		 = src->IDCMPWindow->Height - src->IDCMPWindow->BorderTop  - src->IDCMPWindow->BorderBottom;
}

// TODO: we need to handle Intuition's window move (repositioning) event and update sdlwin's x&y
static void
OS4_EventHandler(_THIS)
{
	SDL_VideoData *data = (SDL_VideoData *) _this->driverdata;

	struct IntuiMessage *imsg;
	struct MyIntuiMessage msg;

	while ((imsg = (struct IntuiMessage *)IExec->GetMsg(data->userport))) {

		OS4_CopyRelevantFields(imsg, &msg);

		IExec->ReplyMsg((struct Message *) imsg);

		switch (msg.Class) {
			case IDCMP_MOUSEMOVE:
				OS4_HandleMouseMotion(_this, &msg);
				break;

			case IDCMP_RAWKEY:
				OS4_HandleKeyboard(_this, &msg);
				break;

			case IDCMP_MOUSEBUTTONS:
				OS4_HandleMouseButtons(_this, &msg);
				break;

			case IDCMP_EXTENDEDMOUSE:
				OS4_HandleMouseWheel(_this, &msg);
				break;

			case IDCMP_NEWSIZE:
				OS4_HandleResize(_this, &msg);
				break;

			case IDCMP_ACTIVEWINDOW:
				OS4_HandleActivation(_this, &msg, SDL_TRUE);
				break;

			case IDCMP_INACTIVEWINDOW:
				OS4_HandleActivation(_this, &msg, SDL_FALSE);
				break;

			case IDCMP_CLOSEWINDOW:
				OS4_HandleClose(_this, &msg);
				break;

			case IDCMP_INTUITICKS:
				OS4_HandleTicks(_this, &msg);
				break;

			default:
				dprintf("Unknown event received class %d, code %d\n", msg.Class, msg.Code);
				break;
		}

	}
}

void
OS4_PumpEvents(_THIS)
{
	//SDL_VideoData *data = (SDL_VideoData *) _this->driverdata;
	OS4_EventHandler(_this);
}

#endif /* SDL_VIDEO_DRIVER_AMIGAOS4 */

/* vi: set ts=4 sw=4 expandtab: */
