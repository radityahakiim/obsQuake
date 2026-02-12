/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// vid_win.c -- Win32 video driver

#include "quakedef.h"
#include "winquake.h"
#include "d_local.h"
#include "resource.h"

#define MAX_MODE_LIST	100
#define VID_ROW_SIZE	4

qboolean	dibonly;

extern int		Minimized;
 // HWND		mainwindow;

// HWND WINAPI InitializeWindow (HINSTANCE hInstance, int nCmdShow);

// int			DIBWidth, DIBHeight;
qboolean	DDActive;
// RECT		WindowRect;
// DWORD		WindowStyle, ExWindowStyle;

int			window_center_x, window_center_y, window_x, window_y, window_width, window_height;
RECT		window_rect;

static DEVMODE	gdevmode;
static qboolean	startwindowed = 0, windowed_mode_set;
static int		firstupdate = 1;
static qboolean	vid_initialized = false, vid_palettized;
static int		lockcount;
static int		vid_fulldib_on_focus_mode;
static qboolean	force_minimized, in_mode_set, is_mode0x13, force_mode_set;
static int		vid_stretched, windowed_mouse;
static qboolean	palette_changed, syscolchg, vid_mode_set, hide_window, pal_is_nostatic;
static HICON	hIcon;
static qboolean paused_for_focus = false;

extern int mx_accum;
extern int my_accum;
extern qboolean	mouseactive;  // from in_win.c

viddef_t	vid;				// global video state

#define MODE_WINDOWED			0
#define MODE_SETTABLE_WINDOW	2
#define NO_MODE					(MODE_WINDOWED - 1)
#define MODE_FULLSCREEN_DEFAULT	(MODE_WINDOWED + 3)

// Note that 0 is MODE_WINDOWED
cvar_t		vid_mode = {"vid_mode","0", false};
// Note that 0 is MODE_WINDOWED
cvar_t		_vid_default_mode = {"_vid_default_mode","0", true};
// Note that 3 is MODE_FULLSCREEN_DEFAULT
cvar_t		_vid_default_mode_win = {"_vid_default_mode_win","3", true};
cvar_t		vid_wait = {"vid_wait","0"};
cvar_t		vid_nopageflip = {"vid_nopageflip","0", true};
cvar_t		_vid_wait_override = {"_vid_wait_override", "0", true};
cvar_t		vid_config_x = {"vid_config_x","800", true};
cvar_t		vid_config_y = {"vid_config_y","600", true};
cvar_t		vid_stretch_by_2 = {"vid_stretch_by_2","1", true};
cvar_t		_windowed_mouse = {"_windowed_mouse","0", true};
cvar_t		vid_fullscreen_mode = {"vid_fullscreen_mode","3", true};
cvar_t		vid_windowed_mode = {"vid_windowed_mode","0", true};
cvar_t		block_switch = {"block_switch","0", true};
cvar_t		vid_window_x = {"vid_window_x", "0", true};
cvar_t		vid_window_y = {"vid_window_y", "0", true};

typedef struct {
	int		width;
	int		height;
} lmode_t;

lmode_t	lowresmodes[] = {
	{320, 200},
	{320, 240},
	{400, 300},
	{512, 384},
};

int			vid_modenum = NO_MODE;
int			vid_testingmode, vid_realmode;
double		vid_testendtime;
int			vid_default = MODE_WINDOWED;
static int	windowed_default;

modestate_t	modestate = MS_UNINIT;

static byte		*vid_surfcache;
static int		vid_surfcachesize;
static int		VID_highhunkmark;

unsigned char	vid_curpal[256*3];

unsigned short	d_8to16table[256];
unsigned	d_8to24table[256];

int     driver = grDETECT,mode;
bool    useWinDirect = true, useDirectDraw = true;
// MGLDC	*mgldc = NULL,*memdc = NULL,*dibdc = NULL,*windc = NULL;

typedef struct {
	modestate_t	type;
	int			width;
	int			height;
	int			modenum;
	int			mode13;
	int			stretched;
	int			dib;
	int			fullscreen;
	int			bpp;
	int			halfscreen;
	char		modedesc[13];
} vmode_t;

static vmode_t	modelist[MAX_MODE_LIST];
static int		nummodes;
static vmode_t	*pcurrentmode;

int		aPage;					// Current active display page
int		vPage;					// Current visible display page
int		waitVRT = true;			// True to wait for retrace on flip

static vmode_t	badmode;

static byte	backingbuf[48*24];

void VID_MenuDraw (void);
void VID_MenuKey (int key);

LONG WINAPI MainWndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void AppActivate(const SDL_Event* event);
void VID_UpdateWindowStatus(void);
SDL_Window* window = NULL;
// SDL_Surface* screen_surface = NULL; // window surface
SDL_Surface* quake_surface = NULL; // 8-bit quake render surface

SDL_Renderer* renderer = NULL;
SDL_Texture* render_texture = NULL;

const float RATIO_STD = 4.0f / 3.0f;
const float RATIO_WS = 16.0f / 9.0f;
const float RATIO_WS_WXGA = 16.0f / 10.0f;
const float TOLERANCE = 0.02f;

static const int INTERNAL_WIDTH_STD = 1024;
static const int INTERNAL_HEIGHT_STD = 768;

static const int INTERNAL_WIDTH_WS = 1280;
static const int INTERNAL_HEIGHT_WS = 720;

static const int INTERNAL_WIDTH_WS_WXGA = 1280;
static const int INTERNAL_HEIGHT_WS_WXGA = 800;

void HandleEvents()
{
	SDL_Event event;

	while (SDL_PollEvent(&event))
	{
		switch (event.type) {
		case SDL_QUIT:
			// close the window
			Sys_Quit();
			break;
		case SDL_WINDOWEVENT:
			switch (event.window.event) {
			case SDL_WINDOWEVENT_MOVED:
				window_x = event.window.data1;
				window_y = event.window.data2;
				VID_UpdateWindowStatus();
				break;
			case SDL_WINDOWEVENT_FOCUS_GAINED:
			case SDL_WINDOWEVENT_RESTORED:
			case SDL_WINDOWEVENT_FOCUS_LOST:
			case SDL_WINDOWEVENT_MINIMIZED:
				AppActivate(&event);
				if (event.window.event == SDL_WINDOWEVENT_RESTORED)
					VID_UpdateWindowStatus();
				break;
			}
			break;
		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			int key = MapKey(event.key.keysym.scancode);
			if (key) {
				Key_Event(key, (event.type == SDL_KEYDOWN));
			}
		}
		break;
		case SDL_MOUSEMOTION:
			if (mouseactive) {
				mx_accum += event.motion.xrel;
				my_accum += event.motion.yrel;
			}
			break;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		{
			int buttonstate = -1;
			qboolean down = (event.type == SDL_MOUSEBUTTONDOWN);

			switch (event.button.button) {
			case SDL_BUTTON_LEFT:   buttonstate = K_MOUSE1; break;
			case SDL_BUTTON_RIGHT:  buttonstate = K_MOUSE2; break;
			case SDL_BUTTON_MIDDLE: buttonstate = K_MOUSE3; break;
			case SDL_BUTTON_X1:		buttonstate = K_MOUSE4; break;
			case SDL_BUTTON_X2:		buttonstate = K_MOUSE5; break;
			}
			if (buttonstate != -1) {
				Key_Event(buttonstate, down);
			}
			break;
		}
		case SDL_MOUSEWHEEL:
			if (event.wheel.y > 0) {
				Key_Event(K_MWHEELUP, true);
				Key_Event(K_MWHEELUP, false);
			}
			else if (event.wheel.y < 0) {
				Key_Event(K_MWHEELDOWN, true);
				Key_Event(K_MWHEELDOWN, false);

			}
			break;
		}
	}
}


/*
================
VID_RememberWindowPos
================
*/
void VID_RememberWindowPos (void)
{
	if (window)
	{
		SDL_Rect rect;
		SDL_GetWindowPosition(window, &rect.x, &rect.y);
		SDL_GetWindowSize(window, &rect.w, &rect.h);

		int display_idx = SDL_GetWindowDisplayIndex(window);
		SDL_Rect displayBounds;

		if (display_idx >= 0 &&
			SDL_GetDisplayBounds(display_idx, &displayBounds) == 0) {
			if ((rect.x < displayBounds.w) &&
				(rect.y < displayBounds.h) &&
				(rect.x + rect.w > 0) &&
				(rect.y + rect.h > 0))
			{
				Cvar_SetValue("vid_window_x", (float)rect.x);
				Cvar_SetValue("vid_window_y", (float)rect.y);
			}
		}
	}
}


/*
================
VID_CheckWindowXY
================
*/
void VID_CheckWindowXY (void)
{
	int display_idx = SDL_GetWindowDisplayIndex(window);
	SDL_Rect displayBounds;
	SDL_GetDisplayBounds(display_idx, &displayBounds);

	if (((int)vid_window_x.value > (displayBounds.w)) ||
		((int)vid_window_y.value > (displayBounds.h)) ||
		((int)vid_window_x.value < 0)				  ||
		((int)vid_window_y.value < 0))
	{
		Cvar_SetValue ("vid_window_x", 0.0);
		Cvar_SetValue ("vid_window_y", 0.0 );
	}
}


/*
================
VID_UpdateWindowStatus
================
*/
void VID_UpdateWindowStatus (void)
{
	if (window) {
		SDL_GetWindowPosition(window, &window_x, &window_y);
		SDL_GetWindowSize(window, &window_width, &window_height);
	}
	window_rect.left = window_x;
	window_rect.top = window_y;
	window_rect.right = window_x + window_width;
	window_rect.bottom = window_y + window_height;
	window_center_x = (window_rect.left + window_rect.right) / 2;
	window_center_y = (window_rect.top + window_rect.bottom) / 2;

	IN_UpdateClipCursor ();
}


/*
================
ClearAllStates
================
*/
void ClearAllStates (void)
{
	int		i;
	
// send an up event for each key, to make sure the server clears them all
	for (i=0 ; i<256 ; i++)
	{
		Key_Event (i, false);
	}

	Key_ClearStates ();
	IN_ClearStates ();
}


/*
================
VID_CheckAdequateMem
================
*/
qboolean VID_CheckAdequateMem (int width, int height)
{
	int		tbuffersize;

	tbuffersize = width * height * sizeof (*d_pzbuffer);

	tbuffersize += D_SurfaceCacheForRes (width, height);

// see if there's enough memory, allowing for the normal mode 0x13 pixel,
// z, and surface buffers
	if ((host_parms.memsize - tbuffersize + SURFCACHE_SIZE_AT_320X200 +
		 0x10000 * 3) < minimum_memory)
	{
		return false;		// not enough memory for mode
	}

	return true;
}


/*
================
VID_AllocBuffers
================
*/
qboolean VID_AllocBuffers (int width, int height)
{
	int		tsize, tbuffersize;

	tbuffersize = width * height * sizeof (*d_pzbuffer);

	tsize = D_SurfaceCacheForRes (width, height);

	tbuffersize += tsize;

// see if there's enough memory, allowing for the normal mode 0x13 pixel,
// z, and surface buffers
	if ((host_parms.memsize - tbuffersize + SURFCACHE_SIZE_AT_320X200 +
		 0x10000 * 3) < minimum_memory)
	{
		Con_SafePrintf ("Not enough memory for video mode\n");
		return false;		// not enough memory for mode
	}

	vid_surfcachesize = tsize;

	if (d_pzbuffer)
	{
		D_FlushCaches ();
		Hunk_FreeToHighMark (VID_highhunkmark);
		d_pzbuffer = NULL;
	}

	VID_highhunkmark = Hunk_HighMark ();

	d_pzbuffer = Hunk_HighAllocName (tbuffersize, "video");

	vid_surfcache = (byte *)d_pzbuffer +
			width * height * sizeof (*d_pzbuffer);
	
	return true;
}

void VID_InitModes(void) {
	nummodes = 0;
	SDL_DisplayMode mode;
	int display = 0; // primary display

	// get desktop mode for reference
	if (SDL_GetDesktopDisplayMode(display, &mode) == 0) {
		int win_width = mode.w * 0.8;
		int win_height = mode.h * 0.8;
		if (win_width < 640) win_width = 640;
		if (win_height < 480) win_height = 480;

		modelist[nummodes].type = MS_WINDOWED;
		modelist[nummodes].width = win_width;
		modelist[nummodes].height = win_height;
		sprintf(modelist[nummodes].modedesc, "%dx%d", win_width, win_height);
		modelist[nummodes].modenum = MODE_WINDOWED;
		modelist[nummodes].stretched = 0;
		modelist[nummodes].dib = 1;
		modelist[nummodes].fullscreen = 0;
		modelist[nummodes].halfscreen = 0;
		modelist[nummodes].bpp = SDL_BITSPERPIXEL(mode.format);
		nummodes++;
	}

	modelist[nummodes].type = MS_WINDOWED;
	modelist[nummodes].width = 320;
	modelist[nummodes].height = 240;
	sprintf(modelist[nummodes].modedesc, "320x240");
	modelist[nummodes].modenum = MODE_WINDOWED + 1;
	modelist[nummodes].stretched = 0;
	modelist[nummodes].dib = 1;
	modelist[nummodes].fullscreen = 0;
	modelist[nummodes].halfscreen = 0;
	modelist[nummodes].bpp = 8;
	nummodes++;

	modelist[nummodes].type = MS_WINDOWED;
	modelist[nummodes].width = 640;
	modelist[nummodes].height = 480;
	sprintf(modelist[nummodes].modedesc, "640x480");
	modelist[nummodes].modenum = MODE_WINDOWED + 2;
	modelist[nummodes].stretched = 0;
	modelist[nummodes].dib = 1;
	modelist[nummodes].fullscreen = 0;
	modelist[nummodes].halfscreen = 0;
	modelist[nummodes].bpp = 8;
	nummodes++;

	// enumerate fullscreen modes
	int num_modes = SDL_GetNumDisplayModes(display);
	for (int i = 0; i < num_modes && nummodes < MAX_MODE_LIST; i++) {
		if (SDL_GetDisplayMode(display, i, &mode) == 0) {
			int bpp = SDL_BITSPERPIXEL(mode.format);
			if (bpp >= 8 && mode.w >= 320 && mode.h >= 200 && mode.w <= MAXWIDTH && mode.h <= MAXHEIGHT)
			{
				// check for duplicates
				qboolean duplicate = false;
				for (int j = 0; j < nummodes; j++) {
					if (modelist[j].width == mode.w && modelist[j].height == mode.h && modelist[j].bpp == bpp) {
						duplicate = true;
						break;
					}
				}
				if (!duplicate) {
					modelist[nummodes].type = MS_FULLSCREEN;
					modelist[nummodes].width = mode.w;
					modelist[nummodes].height = mode.h;
					sprintf(modelist[nummodes].modedesc, "%dx%d", mode.w, mode.h);
					modelist[nummodes].modenum = nummodes; // offset after windowed
					modelist[nummodes].stretched = 0;
					modelist[nummodes].dib = 0;
					modelist[nummodes].fullscreen = 1;
					modelist[nummodes].halfscreen = 0;
					modelist[nummodes].bpp = bpp;
					nummodes++;
				}
			}
		}
	}
	if (nummodes <= 3) {
		modelist[nummodes].type = MS_FULLSCREEN;
		modelist[nummodes].width = 640;
		modelist[nummodes].height = 480;
		sprintf(modelist[nummodes].modedesc, "640x480");
		modelist[nummodes].modenum = MODE_FULLSCREEN_DEFAULT;
		modelist[nummodes].stretched = 0;
		modelist[nummodes].dib = 0;
		modelist[nummodes].fullscreen = 1;
		modelist[nummodes].halfscreen = 0;
		modelist[nummodes].bpp = 8;
		nummodes++;
	}

	vid_default = MODE_WINDOWED;
	windowed_default = vid_default;
}

qboolean VID_SetWindowedMode(int modenum)
{
	int targ_winternal;
	int targ_hinternal;
	float ratio;

	if (render_texture) { SDL_DestroyTexture(render_texture); render_texture = NULL; }
	if (renderer) { SDL_DestroyRenderer(renderer); renderer = NULL; }
	if (quake_surface) { SDL_FreeSurface(quake_surface); quake_surface = NULL; }
	if (window) { SDL_DestroyWindow(window); window = NULL; }

	Uint32 flags = SDL_WINDOW_SHOWN;

	// center automatically
	int posx = SDL_WINDOWPOS_CENTERED;
	int posy = SDL_WINDOWPOS_CENTERED;

	// determine aspect ratio for widescreen
	ratio = (float)modelist[modenum].width / modelist[modenum].height;

	qboolean is_std = (fabs(ratio - RATIO_STD) < TOLERANCE);
	qboolean is_ws = (fabs(ratio - RATIO_WS) < TOLERANCE);
	qboolean is_ws_wxga = (fabs(ratio - RATIO_WS_WXGA) < TOLERANCE);
	if (is_ws)
	{
		if ((modelist[modenum].width >= INTERNAL_WIDTH_WS) && (modelist[modenum].height >= INTERNAL_HEIGHT_WS)) {
			targ_winternal = INTERNAL_WIDTH_WS;
			targ_hinternal = INTERNAL_HEIGHT_WS;
		}
		else {
			targ_winternal = modelist[modenum].width;
			targ_hinternal = modelist[modenum].height;
		}
	}
	else if (is_std) {
		if ((modelist[modenum].width >= INTERNAL_WIDTH_STD) && (modelist[modenum].height >= INTERNAL_HEIGHT_STD)) {
			targ_winternal = INTERNAL_WIDTH_STD;
			targ_hinternal = INTERNAL_HEIGHT_STD;
		}
		else {
			targ_winternal = modelist[modenum].width;
			targ_hinternal = modelist[modenum].height;
		}
	}
	else if (is_ws_wxga) {
		if ((modelist[modenum].width >= INTERNAL_WIDTH_WS_WXGA) && (modelist[modenum].height >= INTERNAL_HEIGHT_WS_WXGA)) {
			targ_winternal = INTERNAL_WIDTH_WS_WXGA;
			targ_hinternal = INTERNAL_HEIGHT_WS_WXGA;
		}
		else {
			targ_winternal = modelist[modenum].width;
			targ_hinternal = modelist[modenum].height;
		}
	}
	else {
		targ_winternal = modelist[modenum].width;
		targ_hinternal = modelist[modenum].height;
	}


	window = SDL_CreateWindow("WinQuake",
		posx,
		posy,
		modelist[modenum].width,
		modelist[modenum].height,
		flags);
	
	if (!window) {
		Sys_Error("SDL_CreateWindow failed: %s", SDL_GetError());
	}

	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
	//SDL_RenderSetLogicalSize(renderer, targ_winternal, targ_hinternal);

	render_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
		SDL_TEXTUREACCESS_STREAMING, targ_winternal, targ_hinternal);
	 if (!render_texture) Sys_Error("SDL_CreateTexture failed: %s", SDL_GetError());

	quake_surface = SDL_CreateRGBSurface(0, targ_winternal, targ_hinternal, 8, 0, 0, 0, 0);
	if (!quake_surface) {
		Sys_Error("SDL_CreateRGBSurface failed: %s", SDL_GetError());
	}

	vid.buffer = vid.conbuffer = vid.direct = (byte*)quake_surface->pixels;
	vid.rowbytes = vid.conrowbytes = quake_surface->pitch;
	vid.numpages = 1;
	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.height = vid.conheight = targ_hinternal;
	vid.width = vid.conwidth = targ_winternal;
	vid.aspect = ((float)targ_hinternal / (float)targ_winternal) * (320.0 / 240.0);

	vid_stretched = modelist[modenum].stretched;
	modestate = MS_WINDOWED;
	vid_fulldib_on_focus_mode = 0;

	VID_UpdateWindowStatus();

	if (_windowed_mouse.value) {
		IN_ActivateMouse();
		IN_HideMouse();
	}
	else {
		IN_DeactivateMouse();
		IN_ShowMouse();
	}
	return true;
}


qboolean VID_SetFullscreenMode (int modenum)
{
	int targ_winternal;
	int targ_hinternal;
	float ratio;

	if (render_texture) { SDL_DestroyTexture(render_texture); render_texture = NULL; }
	if (renderer) { SDL_DestroyRenderer(renderer); renderer = NULL; }
	if (quake_surface) { SDL_FreeSurface(quake_surface); quake_surface = NULL; }
	if (window) { SDL_DestroyWindow(window); window = NULL; }

	Uint32 flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN;

	// determine aspect ratio for widescreen
	ratio = (float)modelist[modenum].width / modelist[modenum].height;

	qboolean is_std = (fabs(ratio - RATIO_STD) < TOLERANCE);
	qboolean is_ws = (fabs(ratio - RATIO_WS) < TOLERANCE);
	qboolean is_ws_wxga = (fabs(ratio - RATIO_WS_WXGA) < TOLERANCE);
	if (is_ws)
	{
		if ((modelist[modenum].width >= INTERNAL_WIDTH_WS) && (modelist[modenum].height >= INTERNAL_HEIGHT_WS)) {
			targ_winternal = INTERNAL_WIDTH_WS;
			targ_hinternal = INTERNAL_HEIGHT_WS;
		}
		else {
			targ_winternal = modelist[modenum].width;
			targ_hinternal = modelist[modenum].height;
		}
	}
	else if (is_std) {
		if ((modelist[modenum].width >= INTERNAL_WIDTH_STD) && (modelist[modenum].height >= INTERNAL_HEIGHT_STD)) {
			targ_winternal = INTERNAL_WIDTH_STD;
			targ_hinternal = INTERNAL_HEIGHT_STD;
		}
		else {
			targ_winternal = modelist[modenum].width;
			targ_hinternal = modelist[modenum].height;
		}
	}
	else if (is_ws_wxga) {
		if ((modelist[modenum].width >= INTERNAL_WIDTH_WS_WXGA) && (modelist[modenum].height >= INTERNAL_HEIGHT_WS_WXGA)) {
			targ_winternal = INTERNAL_WIDTH_WS_WXGA;
			targ_hinternal = INTERNAL_HEIGHT_WS_WXGA;
		}
		else {
			targ_winternal = modelist[modenum].width;
			targ_hinternal = modelist[modenum].height;
		}
	}
	else {
		targ_winternal = modelist[modenum].width;
		targ_hinternal = modelist[modenum].height;
	}

	window = SDL_CreateWindow("WinQuake",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		modelist[modenum].width,
		modelist[modenum].height,
		flags);

	if (!window) {
		Sys_Error("SDL_CreateWindow failed: %s", SDL_GetError());
	}

	SDL_DisplayMode dm;
	SDL_GetWindowDisplayMode(window, &dm);
	dm.w = modelist[modenum].width;
	dm.h = modelist[modenum].height;
	SDL_SetWindowDisplayMode(window, &dm);

	renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);

	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
	//SDL_RenderSetLogicalSize(renderer, targ_winternal, targ_hinternal);

	render_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
		SDL_TEXTUREACCESS_STREAMING, targ_winternal, targ_hinternal);
	if (!render_texture) Sys_Error("SDL_CreateTexture failed: %s", SDL_GetError());

	quake_surface = SDL_CreateRGBSurface(0, targ_winternal, targ_hinternal, 8, 0, 0, 0, 0);
	if (!quake_surface) {
		Sys_Error("SDL_CreateRGBSurface failed: %s", SDL_GetError());
	}

	vid.buffer = vid.conbuffer = vid.direct = (byte*)quake_surface->pixels;
	vid.rowbytes = vid.conrowbytes = quake_surface->pitch;
	vid.numpages = 1;
	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.height = vid.conheight = targ_hinternal;
	vid.width = vid.conwidth = targ_winternal;
	vid.aspect = ((float)targ_hinternal / (float)targ_winternal) * (320.0 / 240.0);

	vid_stretched = modelist[modenum].stretched;

	modestate = MS_FULLSCREEN;

	VID_UpdateWindowStatus();

	IN_ActivateMouse();
	IN_HideMouse(); 

	return true;
}

/*
=================
VID_NumModes
=================
*/
int VID_NumModes(void)
{
	return nummodes;
}

/*
=================
VID_GetModePtr
=================
*/
vmode_t* VID_GetModePtr(int modenum)
{

	if ((modenum >= 0) && (modenum < nummodes))
		return &modelist[modenum];
	else
		return &badmode;
}

/*
=================
VID_CheckModedescFixup
=================
*/
void VID_CheckModedescFixup(int mode)
{
	int		x, y, stretch;

	if (mode == MODE_SETTABLE_WINDOW)
	{
		modelist[mode].stretched = (int)vid_stretch_by_2.value;
		stretch = modelist[mode].stretched;

		if (vid_config_x.value < (320 << stretch))
			vid_config_x.value = 320 << stretch;

		if (vid_config_y.value < (200 << stretch))
			vid_config_y.value = 200 << stretch;

		x = (int)vid_config_x.value;
		y = (int)vid_config_y.value;
		sprintf(modelist[mode].modedesc, "%dx%d", x, y);
		modelist[mode].width = x;
		modelist[mode].height = y;
	}
}

/*
=================
VID_GetModeDescriptionMemCheck
=================
*/
char* VID_GetModeDescriptionMemCheck(int mode)
{
	char* pinfo;
	vmode_t* pv;

	if ((mode < 0) || (mode >= nummodes))
		return NULL;

	VID_CheckModedescFixup(mode);

	pv = VID_GetModePtr(mode);
	pinfo = pv->modedesc;

	if (VID_CheckAdequateMem(pv->width, pv->height))
	{
		return pinfo;
	}
	else
	{
		return NULL;
	}
}


/*
=================
VID_GetModeDescription
=================
*/
char* VID_GetModeDescription(int mode)
{
	char* pinfo;
	vmode_t* pv;

	if ((mode < 0) || (mode >= nummodes))
		return NULL;

	VID_CheckModedescFixup(mode);

	pv = VID_GetModePtr(mode);
	pinfo = pv->modedesc;
	return pinfo;
}


/*
=================
VID_GetModeDescription2

Tacks on "windowed" or "fullscreen"
=================
*/
char* VID_GetModeDescription2(int mode)
{
	static char	pinfo[40];
	vmode_t* pv;

	if ((mode < 0) || (mode >= nummodes))
		return NULL;

	VID_CheckModedescFixup(mode);

	pv = VID_GetModePtr(mode);

	if (modelist[mode].type == MS_FULLSCREEN)
	{
		sprintf(pinfo, "%s fullscreen", pv->modedesc);
	}
	else if (modelist[mode].type == MS_FULLDIB)
	{
		sprintf(pinfo, "%s fullscreen", pv->modedesc);
	}
	else
	{
		sprintf(pinfo, "%s windowed", pv->modedesc);
	}

	return pinfo;
}

char* VID_GetExtModeDescription(int mode)
{
	static char	pinfo[40];
	vmode_t* pv;

	if ((mode < 0) || (mode >= nummodes))
		return NULL;

	VID_CheckModedescFixup(mode);

	pv = VID_GetModePtr(mode);
	if (modelist[mode].type == MS_FULLSCREEN)
	{
		sprintf(pinfo, "%s fullscreen %s", pv->modedesc,
			MGL_modeDriverName(pv->modenum));
	}
	else if (modelist[mode].type == MS_FULLDIB)
	{
		sprintf(pinfo, "%s fullscreen DIB", pv->modedesc);
	}
	else
	{
		sprintf(pinfo, "%s windowed", pv->modedesc);
	}

	return pinfo;
}

void VID_SetDefaultMode(void)
{

	if (vid_initialized)
		VID_SetMode(0, vid_curpal);

	IN_DeactivateMouse();
}

void D_BeginDirectRect(int x, int y, byte* pbitmap, int width, int height)
{
}

void D_EndDirectRect(int x, int y, int width, int height)
{
}

int VID_SetMode (int modenum, unsigned char *palette)
{
	int				original_mode, temp, dummy;
	qboolean		stat;
    // MSG				msg;
	// HDC				hdc;

	while ((modenum >= nummodes) || (modenum < 0))
	{
		if (vid_modenum == NO_MODE)
		{
			modenum = vid_default;
			Cvar_SetValue("vid_mode", (float)modenum);
		}
		else
		{
			Cvar_SetValue("vid_mode", (float)vid_modenum);
			return 0;
		}
	}

	if (!force_mode_set && (modenum == vid_modenum))
		return true;

// so Con_Printfs don't mess us up by forcing vid and snd updates
	temp = scr_disabled_for_loading;
	scr_disabled_for_loading = true;
	in_mode_set = true;

	CDAudio_Pause ();
	S_ClearBuffer ();

	if (vid_modenum == NO_MODE)
		original_mode = windowed_default;
	else
		original_mode = vid_modenum;

	// set the mode
	if (modelist[modenum].type == MS_WINDOWED)
	{
		stat = VID_SetWindowedMode(modenum);
	}
	else
	{
		stat = VID_SetFullscreenMode(modenum);
	}

	window_width = vid.width << vid_stretched;
	window_height = vid.height << vid_stretched;
	VID_UpdateWindowStatus();

	// PeekMessage equivalent
	SDL_PumpEvents();

	CDAudio_Resume();
	scr_disabled_for_loading = temp;

	if (!stat)
	{
		Sys_Error("Couldn't set video mode");
	}

	vid_palettized = true;

	VID_SetPalette(palette);
	vid_modenum = modenum;
	Cvar_SetValue("vid_mode", (float)vid_modenum);

	if (!VID_AllocBuffers(vid.width, vid.height))
		Sys_Error("Couldn't allocate video buffers");

	D_InitCaches(vid_surfcache, vid_surfcachesize);

	ClearAllStates();

	in_mode_set = false;
	vid.recalc_refdef = 1;

	return true;
}

void VID_LockBuffer (void)
{
	lockcount++;

	if (lockcount > 1)
		return;

	SDL_LockSurface(quake_surface);
	vid.buffer = vid.conbuffer = vid.direct = d_viewbuffer = (byte*)quake_surface->pixels;
	if (r_dowarp)
		d_viewbuffer = r_warpbuffer;
	else
		d_viewbuffer = (void *)(byte *)vid.buffer;

	if (r_dowarp)
		screenwidth = WARP_WIDTH;
	else
		screenwidth = vid.rowbytes;

	if (lcd_x.value)
		screenwidth <<= 1;
}
		
		
void VID_UnlockBuffer (void)
{
	lockcount--;

	if (lockcount > 0)
		return;

	if (lockcount < 0)
		Sys_Error ("Unbalanced unlock");

	SDL_UnlockSurface(quake_surface);

// to turn up any unlocked accesses
//	vid.buffer = vid.conbuffer = vid.direct = d_viewbuffer = NULL;

}


int VID_ForceUnlockedAndReturnState (void)
{
	int	lk = lockcount;

	if (lockcount > 0)
	{
		lockcount = 1;
		VID_UnlockBuffer ();
	}

	return lk;
}


void VID_ForceLockState (int lk)
{

	if (lk)
	{
		VID_LockBuffer ();
	}

	lockcount = lk;
}


void	VID_SetPalette (unsigned char *palette)
{
	SDL_Color colors[256];

	if (!Minimized)
	{
		palette_changed = true;

		for (int i = 0; i < 256; i++) {
			colors[i].r = palette[i * 3];
			colors[i].g = palette[i * 3 + 1];
			colors[i].b = palette[i * 3 + 2];
		}

		SDL_SetPaletteColors(quake_surface->format->palette, colors, 0, 256);
	}

	memcpy (vid_curpal, palette, sizeof(vid_curpal));
}


void	VID_ShiftPalette (unsigned char *palette)
{
	VID_SetPalette (palette);
}


/*
=================
VID_DescribeCurrentMode_f
=================
*/
void VID_DescribeCurrentMode_f (void)
{
	Con_Printf ("%s\n", VID_GetExtModeDescription (vid_modenum));
}


/*
=================
VID_NumModes_f
=================
*/
void VID_NumModes_f (void)
{

	if (nummodes == 1)
		Con_Printf ("%d video mode is available\n", nummodes);
	else
		Con_Printf ("%d video modes are available\n", nummodes);
}


/*
=================
VID_DescribeMode_f
=================
*/
void VID_DescribeMode_f (void)
{
	int		modenum;
	
	modenum = Q_atoi (Cmd_Argv(1));

	Con_Printf ("%s\n", VID_GetExtModeDescription (modenum));
}


/*
=================
VID_DescribeModes_f
=================
*/
void VID_DescribeModes_f (void)
{
	int			i, lnummodes;
	char		*pinfo;
	qboolean	na;
	vmode_t		*pv;

	na = false;

	lnummodes = VID_NumModes ();

	for (i=0 ; i<lnummodes ; i++)
	{
		pv = VID_GetModePtr (i);
		pinfo = VID_GetExtModeDescription (i);

		if (VID_CheckAdequateMem (pv->width, pv->height))
		{
			Con_Printf ("%2d: %s\n", i, pinfo);
		}
		else
		{
			Con_Printf ("**: %s\n", pinfo);
			na = true;
		}
	}

	if (na)
	{
		Con_Printf ("\n[**: not enough system RAM for mode]\n");
	}
}


/*
=================
VID_TestMode_f
=================
*/
void VID_TestMode_f (void)
{
	int		modenum;
	double	testduration;

	if (!vid_testingmode)
	{
		modenum = Q_atoi (Cmd_Argv(1));

		if (VID_SetMode (modenum, vid_curpal))
		{
			vid_testingmode = 1;
			testduration = Q_atof (Cmd_Argv(2));
			if (testduration == 0)
				testduration = 5.0;
			vid_testendtime = realtime + testduration;
		}
	}
}


/*
=================
VID_Windowed_f
=================
*/
void VID_Windowed_f (void)
{

	VID_SetMode ((int)vid_windowed_mode.value, vid_curpal);
}


/*
=================
VID_Fullscreen_f
=================
*/
void VID_Fullscreen_f (void)
{

	VID_SetMode ((int)vid_fullscreen_mode.value, vid_curpal);
}


/*
=================
VID_Minimize_f
=================
*/
void VID_Minimize_f (void)
{

// we only support minimizing windows; if you're fullscreen,
// switch to windowed first
	if (modestate == MS_WINDOWED && window)
		SDL_MinimizeWindow (window);
}



/*
=================
VID_ForceMode_f
=================
*/
void VID_ForceMode_f (void)
{
	int		modenum;
	double	testduration;

	if (!vid_testingmode)
	{
		modenum = Q_atoi (Cmd_Argv(1));

		force_mode_set = 1;
		VID_SetMode (modenum, vid_curpal);
		force_mode_set = 0;
	}
}


void	VID_Init (unsigned char *palette)
{
	int		i, bestmatch, bestmatchmetric, t, dr, dg, db;
	int		basenummodes;
	byte	*ptmp;

	Cvar_RegisterVariable (&vid_mode);
	Cvar_RegisterVariable (&vid_wait);
	Cvar_RegisterVariable (&vid_nopageflip);
	Cvar_RegisterVariable (&_vid_wait_override);
	Cvar_RegisterVariable (&_vid_default_mode);
	Cvar_RegisterVariable (&_vid_default_mode_win);
	Cvar_RegisterVariable (&vid_config_x);
	Cvar_RegisterVariable (&vid_config_y);
	Cvar_RegisterVariable (&vid_stretch_by_2);
	Cvar_RegisterVariable (&_windowed_mouse);
	Cvar_RegisterVariable (&vid_fullscreen_mode);
	Cvar_RegisterVariable (&vid_windowed_mode);
	Cvar_RegisterVariable (&block_switch);
	Cvar_RegisterVariable (&vid_window_x);
	Cvar_RegisterVariable (&vid_window_y);

	Cmd_AddCommand ("vid_testmode", VID_TestMode_f);
	Cmd_AddCommand ("vid_nummodes", VID_NumModes_f);
	Cmd_AddCommand ("vid_describecurrentmode", VID_DescribeCurrentMode_f);
	Cmd_AddCommand ("vid_describemode", VID_DescribeMode_f);
	Cmd_AddCommand ("vid_describemodes", VID_DescribeModes_f);
	Cmd_AddCommand ("vid_forcemode", VID_ForceMode_f);
	Cmd_AddCommand ("vid_windowed", VID_Windowed_f);
	Cmd_AddCommand ("vid_fullscreen", VID_Fullscreen_f);
	Cmd_AddCommand ("vid_minimize", VID_Minimize_f);

	if (COM_CheckParm ("-dibonly"))
		dibonly = true;

	if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
		Sys_Error("SDL_InitSubSystem failed: %s", SDL_GetError());
	}

	VID_InitModes();

	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong(*((int*)vid.colormap + 2048));
	vid_testingmode = 0;

	if (COM_CheckParm("-startwindowed"))
	{
		startwindowed = 1;
		vid_default = windowed_default;
	}
	else {
		vid_default = (int)_vid_default_mode_win.value;
	}

	vid_initialized = true;

	force_mode_set = true;
	VID_SetMode(vid_default, palette);
	S_Init();
	force_mode_set = false;

	vid_realmode = vid_modenum;

	VID_SetPalette(palette);

	vid_menudrawfn = VID_MenuDraw;
	vid_menukeyfn = VID_MenuKey;
}


void	VID_Shutdown (void)
{
	if (vid_initialized)
	{
		if (quake_surface) SDL_FreeSurface(quake_surface);
		// if (screen_surface) SDL_FreeSurface(screen_surface);
		if (window) SDL_DestroyWindow(window);

		SDL_QuitSubSystem(SDL_INIT_VIDEO);

		vid_testingmode = 0;
		vid_initialized = 0;
	}
}

void	VID_Update(vrect_t* rects)
{
	if (!renderer || !render_texture || !quake_surface || !window)
		return;

	int targ_winternal;
	int targ_hinternal;
	float ratio;

	// determine aspect ratio for widescreen
	ratio = (float)modelist[vid_modenum].width / modelist[vid_modenum].height;

	qboolean is_std = (fabs(ratio - RATIO_STD) < TOLERANCE);
	qboolean is_ws = (fabs(ratio - RATIO_WS) < TOLERANCE);
	qboolean is_ws_wxga = (fabs(ratio - RATIO_WS_WXGA) < TOLERANCE);
	if (is_ws)
	{
		if ((modelist[vid_modenum].width >= INTERNAL_WIDTH_WS) && (modelist[vid_modenum].height >= INTERNAL_HEIGHT_WS)) {
			targ_winternal = INTERNAL_WIDTH_WS;
			targ_hinternal = INTERNAL_HEIGHT_WS;
		}
		else {
			targ_winternal = modelist[vid_modenum].width;
			targ_hinternal = modelist[vid_modenum].height;
		}
	}
	else if (is_std) {
		if ((modelist[vid_modenum].width >= INTERNAL_WIDTH_STD) && (modelist[vid_modenum].height >= INTERNAL_HEIGHT_STD)) {
			targ_winternal = INTERNAL_WIDTH_STD;
			targ_hinternal = INTERNAL_HEIGHT_STD;
		}
		else {
			targ_winternal = modelist[vid_modenum].width;
			targ_hinternal = modelist[vid_modenum].height;
		}
	}
	else if (is_ws_wxga) {
		if ((modelist[vid_modenum].width >= INTERNAL_WIDTH_WS_WXGA) && (modelist[vid_modenum].height >= INTERNAL_HEIGHT_WS_WXGA)) {
			targ_winternal = INTERNAL_WIDTH_WS_WXGA;
			targ_hinternal = INTERNAL_HEIGHT_WS_WXGA;
		}
		else {
			targ_winternal = modelist[vid_modenum].width;
			targ_hinternal = modelist[vid_modenum].height;
		}
	}
	else {
		targ_winternal = modelist[vid_modenum].width;
		targ_hinternal = modelist[vid_modenum].height;
	}

	// convert 8-bit palette surface to RGBA8888 texture
	void* pixels;
	int pitch;
	if (SDL_LockTexture(render_texture, NULL, &pixels, &pitch) == 0) {
		SDL_Palette* pal = quake_surface->format->palette;
		byte* src = (byte*)quake_surface->pixels;
		uint32_t* dst = (uint32_t*)pixels;
		int src_pitch = quake_surface->pitch;
		int dst_pitch = pitch / 4;

		for (int y = 0; y < targ_hinternal; y++) {
			for (int x = 0; x < targ_winternal; x++) {
				byte idx = src[x];
				SDL_Color c = pal->colors[idx];
				dst[x] = (c.r << 24) | (c.g << 16) | (c.b << 8) | 0xFF;
			}
			src += src_pitch;
			dst += dst_pitch;
		}
		SDL_UnlockTexture(render_texture);
	}

	// stretch to fill the entire window
	SDL_Rect dst_rect = { 0,0,window_width, window_height };
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, render_texture, NULL, &dst_rect);
	SDL_RenderPresent(renderer);
	if (firstupdate)
	{
		firstupdate = 0;
		if ((int)_vid_default_mode_win.value != vid_default)
		{
			Cvar_SetValue("vid_mode", _vid_default_mode_win.value);
		}
	}

	if (vid_testingmode)
	{
		if (realtime >= vid_testendtime)
		{
			VID_SetMode(vid_realmode, vid_curpal);
			vid_testingmode = 0;
		}
	}
	else
	{
		if ((int)vid_mode.value != vid_realmode)
		{
			VID_SetMode((int)vid_mode.value, vid_curpal);
			Cvar_SetValue("vid_mode", (float)vid_modenum);
			vid_realmode = vid_modenum;
		}
	}

	if (modestate == MS_WINDOWED)
	{
		if ((int)_windowed_mouse.value != windowed_mouse)
		{
			if (_windowed_mouse.value)
			{
				IN_ActivateMouse();
				IN_HideMouse();
			}
			else
			{
				IN_DeactivateMouse();
				IN_ShowMouse();
			}

			windowed_mouse = (int)_windowed_mouse.value;
		}
	}
	// show cursor on pause
	if (key_dest != key_game || cl.paused)
	{
		if (mouseactive)
			IN_DeactivateMouse();
		IN_ShowMouse();
	}
	else
	{
		// game input active: restore mouse behavior based on _windowed_mouse
		if (modestate == MS_FULLSCREEN ||
			(modestate == MS_WINDOWED && _windowed_mouse.value && key_dest == key_game))
		{
			IN_ActivateMouse();
			IN_HideMouse();
		}
		else
		{
			IN_DeactivateMouse();
			IN_ShowMouse();
		}
	}
}

//==========================================================================

/*
=======
MapKey

Map from windows to quake keynums
=======
*/
int MapKey(int scancode)
{
	switch (scancode)
	{
	case SDL_SCANCODE_ESCAPE:      return K_ESCAPE;
	case SDL_SCANCODE_TAB:         return K_TAB;
	case SDL_SCANCODE_KP_ENTER:
	case SDL_SCANCODE_RETURN:      return K_ENTER;
	case SDL_SCANCODE_SPACE:       return K_SPACE;
	case SDL_SCANCODE_BACKSPACE:   return K_BACKSPACE;

	case SDL_SCANCODE_UP:          return K_UPARROW;
	case SDL_SCANCODE_DOWN:        return K_DOWNARROW;
	case SDL_SCANCODE_LEFT:        return K_LEFTARROW;
	case SDL_SCANCODE_RIGHT:       return K_RIGHTARROW;

	case SDL_SCANCODE_LCTRL:
	case SDL_SCANCODE_RCTRL:       return K_CTRL;

	case SDL_SCANCODE_LSHIFT:
	case SDL_SCANCODE_RSHIFT:      return K_SHIFT;

	case SDL_SCANCODE_LALT:
	case SDL_SCANCODE_RALT:        return K_ALT;

	case SDL_SCANCODE_F1:  return K_F1;
	case SDL_SCANCODE_F2:  return K_F2;
	case SDL_SCANCODE_F3:  return K_F3;
	case SDL_SCANCODE_F4:  return K_F4;
	case SDL_SCANCODE_F5:  return K_F5;
	case SDL_SCANCODE_F6:  return K_F6;
	case SDL_SCANCODE_F7:  return K_F7;
	case SDL_SCANCODE_F8:  return K_F8;
	case SDL_SCANCODE_F9:  return K_F9;
	case SDL_SCANCODE_F10: return K_F10;
	case SDL_SCANCODE_F11: return K_F11;
	case SDL_SCANCODE_F12: return K_F12;

	case SDL_SCANCODE_DELETE:      return K_DEL;
	case SDL_SCANCODE_HOME:        return K_HOME;
	case SDL_SCANCODE_END:         return K_END;
	case SDL_SCANCODE_PAGEUP:      return K_PGUP;
	case SDL_SCANCODE_PAGEDOWN:    return K_PGDN;
	case SDL_SCANCODE_INSERT:      return K_INS;

		// ------- Numeric keypad mapped to AUX keys --------
	case SDL_SCANCODE_KP_0:        return K_AUX1;
	case SDL_SCANCODE_KP_1:        return K_AUX2;
	case SDL_SCANCODE_KP_2:        return K_AUX3;
	case SDL_SCANCODE_KP_3:        return K_AUX4;
	case SDL_SCANCODE_KP_4:        return K_AUX5;
	case SDL_SCANCODE_KP_5:        return K_AUX6;
	case SDL_SCANCODE_KP_6:        return K_AUX7;
	case SDL_SCANCODE_KP_7:        return K_AUX8;
	case SDL_SCANCODE_KP_8:        return K_AUX9;
	case SDL_SCANCODE_KP_9:        return K_AUX10;
	case SDL_SCANCODE_KP_PERIOD:   return K_AUX11;

	case SDL_SCANCODE_PAUSE:       return K_PAUSE;

	case SDL_SCANCODE_MINUS:       return '-';
	case SDL_SCANCODE_EQUALS:      return '=';
	case SDL_SCANCODE_LEFTBRACKET: return '[';
	case SDL_SCANCODE_RIGHTBRACKET:return ']';
	case SDL_SCANCODE_SEMICOLON:   return ';';
	case SDL_SCANCODE_APOSTROPHE:  return '\'';
	case SDL_SCANCODE_BACKSLASH:   return '\\';
	case SDL_SCANCODE_SLASH:       return '/';
	case SDL_SCANCODE_GRAVE:       return '`';
	case SDL_SCANCODE_COMMA:	   return ',';
	case SDL_SCANCODE_PERIOD:	   return '.';

	default:
		break;
	}

	// return ASCII if the key is printable
	if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
		return 'a' + (scancode - SDL_SCANCODE_A);

	if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
		return '1' + (scancode - SDL_SCANCODE_1);

	if (scancode == SDL_SCANCODE_0)
		return '0';

	return 0;
}

void AppActivate(const SDL_Event* event)
{
	Uint32 flags = 0;
	qboolean focused = false;
	qboolean minimized = false;

	if (window) {
		flags = SDL_GetWindowFlags(window);
		focused = (flags & SDL_WINDOW_INPUT_FOCUS) ? true : false;
		minimized = (flags & SDL_WINDOW_MINIMIZED) ? true : false;
	}

	ActiveApp = focused;
	Minimized = minimized;

	if (!ActiveApp)
	{
		scr_skipupdate = true;

		if (!cl.paused)
		{
			paused_for_focus = true;
			cl.paused = true;
		}

		S_BlockSound();
		CDAudio_Pause();

		IN_DeactivateMouse();
		IN_ShowMouse();
		ClearAllStates();
		VID_HandlePause(true);
	}
	else
	{
		scr_skipupdate = false;

		if (window) {
			SDL_Surface* new_screen = SDL_GetWindowSurface(window);
			/*
			if (new_screen) {
				screen_surface = new_screen;
			}
			*/
		}

		S_UnblockSound();
		CDAudio_Resume();

		if (paused_for_focus)
		{
			cl.paused = false;
			paused_for_focus = false;
		}

		if (modestate == MS_FULLSCREEN ||
			(modestate == MS_WINDOWED && _windowed_mouse.value && key_dest == key_game))
		{
			IN_ActivateMouse();
			IN_HideMouse();
		}
		else {
			IN_DeactivateMouse();
			IN_ShowMouse();
		}
		ClearAllStates();
		VID_HandlePause(false);

		// reapply palette and force surface update to avoid permanent freeze
		if (quake_surface /* && screen_surface*/) {
			VID_SetPalette(vid_curpal);
			VID_UpdateWindowStatus();
			//SDL_BlitSurface(quake_surface, NULL, screen_surface, NULL);
			SDL_UpdateWindowSurface(window);
			SDL_PumpEvents();
		}
	}
}



/*
================
VID_HandlePause
================
*/
void VID_HandlePause (qboolean pause)
{

	if ((modestate == MS_WINDOWED) && _windowed_mouse.value)
	{
		if (pause)
		{
			IN_DeactivateMouse ();
			IN_ShowMouse ();
		}
		else
		{
			IN_ActivateMouse ();
			IN_HideMouse ();
		}
	}
}


/*
===================================================================

MAIN WINDOW

===================================================================
*/

// LONG CDAudio_MessageHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

extern void M_Menu_Options_f (void);
extern void M_Print (int cx, int cy, char *str);
extern void M_PrintWhite (int cx, int cy, char *str);
extern void M_DrawCharacter (int cx, int line, int num);
extern void M_DrawTransPic (int x, int y, qpic_t *pic);
extern void M_DrawPic (int x, int y, qpic_t *pic);

static int	vid_line, vid_wmodes;

typedef struct
{
	int		modenum;
	char	*desc;
	int		iscur;
	int		ismode13;
	int		width;
} modedesc_t;

#define MAX_COLUMN_SIZE		9
#define MODE_AREA_HEIGHT	(MAX_COLUMN_SIZE + 6)
#define MAX_MODEDESCS		(MAX_COLUMN_SIZE*VID_ROW_SIZE)

static modedesc_t	modedescs[MAX_MODEDESCS];

/*
================
VID_MenuDraw
================
*/
void VID_MenuDraw(void)
{
	qpic_t*		p;
	char*		ptr;
	int			lnummodes, i, j, k, column, row, dup, dupmode;
	char		temp[100];
	vmode_t*	pv;
	modedesc_t	tmodedesc;

	p = Draw_CachePic("gfx/vidmodes.lmp");
	M_DrawPic((320 - p->width) / 2, 4, p);

	// clear any stale entries so leftover data doesn't mis-map selections
	for (i = 0; i < MAX_MODEDESCS; i++) {
		modedescs[i].modenum = -1;
		modedescs[i].desc = NULL;
		modedescs[i].ismode13 = 0;
		modedescs[i].iscur = 0;
		modedescs[i].width = 0;
	}

	for (i = 0; i < 3; i++)
	{
		ptr = VID_GetModeDescription(i);
		modedescs[i].modenum = i;
		modedescs[i].desc = ptr;
		modedescs[i].ismode13 = modelist[i].mode13;
		modedescs[i].iscur = (vid_modenum == i);
		modedescs[i].width = modelist[i].width;
	}

	vid_wmodes = 3;
	lnummodes = VID_NumModes();

	for (i = 3; i < lnummodes; i++)
	{
		ptr = VID_GetModeDescription(i);
		pv = VID_GetModePtr(i);

		// we only have room for 15 fullscreen modes, so don't allow
		// 360-wide modes, because if there are 5 320-wide modes and
		// 5 360-wide modes, we'll run out of space
		if (ptr && ((pv->width != 360) || COM_CheckParm("-allow360")))
		{
			dup = 0;

			for (j = 3; j < vid_wmodes; j++)
			{
				if (!strcmp(modedescs[j].desc, ptr))
				{
					dup = 1;
					dupmode = j;
					break;
				}
			}

			if (dup || (vid_wmodes < MAX_MODEDESCS))
			{
				if (!dup || !modedescs[dupmode].ismode13 || COM_CheckParm("-noforcevga"))
				{
					if (dup)
					{
						k = dupmode;
					}
					else
					{
						k = vid_wmodes;
					}

					modedescs[k].modenum = i;
					modedescs[k].desc = ptr;
					modedescs[k].ismode13 = pv->mode13;
					modedescs[k].iscur = 0;
					modedescs[k].width = pv->width;

					if (i == vid_modenum)
						modedescs[k].iscur = 1;

					if (!dup)
						vid_wmodes++;
				}
			}
		}
	}

	// sort the modes on width (to handle picking up oddball dibonly modes
	// after all the others)
	for (i = 3; i < (vid_wmodes - 1); i++)
	{
		for (j = (i + 1); j < vid_wmodes; j++)
		{
			if (modedescs[i].width > modedescs[j].width)
			{
				tmodedesc = modedescs[i];
				modedescs[i] = modedescs[j];
				modedescs[j] = tmodedesc;
			}
		}
	}


	M_Print(13 * 8, 36, "Windowed Modes");

	column = 16;
	row = 36 + 2 * 8;

	for (i = 0; i < 3; i++)
	{
		if (modedescs[i].iscur)
			M_PrintWhite(column, row, modedescs[i].desc);
		else
			M_Print(column, row, modedescs[i].desc);

		column += 13 * 8;
	}

	if (vid_wmodes > 3)
	{
		M_Print(12 * 8, 36 + 4 * 8, "Fullscreen Modes");

		column = 16;
		row = 36 + 6 * 8;

		for (i = 3; i < vid_wmodes; i++)
		{
			if (modedescs[i].iscur)
				M_PrintWhite(column, row, modedescs[i].desc);
			else
				M_Print(column, row, modedescs[i].desc);

			column += 13 * 8;

			if (((i - 3) % VID_ROW_SIZE) == (VID_ROW_SIZE - 1))
			{
				column = 16;
				row += 8;
			}
		}
	}

	// line cursor
	if (vid_testingmode)
	{
		sprintf(temp, "TESTING %s",
			modedescs[vid_line].desc);
		M_Print(13 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 4, temp);
		M_Print(9 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 6,
			"Please wait 5 seconds...");
	}
	else
	{
		M_Print(9 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8,
			"Press Enter to set mode");
		M_Print(6 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 3,
			"T to test mode for 5 seconds");
		ptr = VID_GetModeDescription2(vid_modenum);

		if (ptr)
		{
			sprintf(temp, "D to set default: %s", ptr);
			M_Print(2 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 5, temp);
		}

		ptr = VID_GetModeDescription2((int)_vid_default_mode_win.value);

		if (ptr)
		{
			sprintf(temp, "Current default: %s", ptr);
			M_Print(3 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 6, temp);
		}

		M_Print(15 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 8,
			"Esc to exit");

		if (vid_line < 3) {
			row = 36 + 2 * 8;
			column = 16 + (vid_line % VID_ROW_SIZE) * 13 * 8;
		}
		else {
			row = 36 + 6 * 8 + ((vid_line - 3) / VID_ROW_SIZE) * 8;
			column = 16 + ((vid_line - 3) % VID_ROW_SIZE) * 13 * 8;
		}

		M_DrawCharacter(column - 8, row, 12 + ((int)(realtime * 4) & 1));
	}
}


/*
================
VID_MenuKey
================
*/
void VID_MenuKey(int key)
{
	if (vid_testingmode)
		return;

	switch (key)
	{
	case K_ESCAPE:
		S_LocalSound("misc/menu1.wav");
		M_Menu_Options_f();
		break;

	case K_LEFTARROW:
		S_LocalSound("misc/menu1.wav");
		vid_line--;
		if (vid_line < 0)
			vid_line = 0;
		if (vid_line >= vid_wmodes)
			vid_line = vid_wmodes - 1;
		break;

	case K_RIGHTARROW:
		S_LocalSound("misc/menu1.wav");
		vid_line++;
		if (vid_line < 0)
			vid_line = 0;
		if (vid_line >= vid_wmodes)
			vid_line = vid_wmodes - 1;
		break;

	case K_UPARROW:
		S_LocalSound("misc/menu1.wav");
		vid_line -= VID_ROW_SIZE;
		if (vid_line < 0)
			vid_line = 0;
		if (vid_line >= vid_wmodes)
			vid_line = vid_wmodes - 1;
		break;

	case K_DOWNARROW:
		S_LocalSound("misc/menu1.wav");
		vid_line += VID_ROW_SIZE;
		if (vid_line < 0)
			vid_line = 0;
		if (vid_line >= vid_wmodes)
			vid_line = vid_wmodes - 1;
		break;

	case K_ENTER:
	{
		S_LocalSound("misc/menu1.wav");
		int sel = modedescs[vid_line].modenum;
		if (sel >= 0 && sel < nummodes) {
			VID_SetMode(sel, vid_curpal);
		}
		break;
	}

	case 'T':
	case 't':
		S_LocalSound("misc/menu1.wav");
		// have to set this before setting the mode because WM_PAINT
		// happens during the mode set and does a VID_Update, which
		// checks vid_testingmode
		vid_testingmode = 1;
		vid_testendtime = realtime + 5.0;
		{
			int sel = modedescs[vid_line].modenum;
			if (sel >= 0 && sel < nummodes)
			{
				if (!VID_SetMode(sel, vid_curpal))
				{
					vid_testingmode = 0;
				}
			}
			else
			{
				vid_testingmode = 0;
			}
		}
		break;

	case 'D':
	case 'd':
		S_LocalSound("misc/menu1.wav");
		firstupdate = 0;
		Cvar_SetValue("_vid_default_mode_win", vid_modenum);
		break;

	default:
		break;
	}
}