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
// gl_vidnt.c -- NT GL vid component

#include "quakedef.h"
#include "winquake.h"
#include "resource.h"
#include <commctrl.h>

#define MAX_MODE_LIST	30
#define VID_ROW_SIZE	3
#define WARP_WIDTH		320
#define WARP_HEIGHT		200
#define MAXWIDTH		10000
#define MAXHEIGHT		10000
#define BASEWIDTH		320
#define BASEHEIGHT		200

#define MODE_WINDOWED			0
#define NO_MODE					(MODE_WINDOWED - 1)
#define MODE_FULLSCREEN_DEFAULT	(MODE_WINDOWED + 1)

typedef struct {
	modestate_t	type;
	int			width;
	int			height;
	int			modenum;
	int			dib;
	int			fullscreen;
	int			bpp;
	int			halfscreen;
	char		modedesc[17];
} vmode_t;

typedef struct {
	int			width;
	int			height;
} lmode_t;

lmode_t	lowresmodes[] = {
	{320, 200},
	{320, 240},
	{400, 300},
	{512, 384},
};

const char* gl_vendor;
const char* gl_renderer;
const char* gl_version;
const char* gl_extensions;

qboolean		DDActive;
qboolean		scr_skipupdate;

static vmode_t	modelist[MAX_MODE_LIST];
static int		nummodes;
static vmode_t* pcurrentmode;
static vmode_t	badmode;

static DEVMODE	gdevmode;
static qboolean	vid_initialized = false;
static qboolean	windowed, leavecurrentmode;
static qboolean vid_canalttab = false;
static qboolean vid_wassuspended = false;
static int		windowed_mouse;
extern qboolean	mouseactive;  // from in_win.c
static HICON	hIcon;

int			DIBWidth, DIBHeight;
RECT		WindowRect;
DWORD		WindowStyle, ExWindowStyle;

HWND	mainwindow, dibwindow;

int			vid_modenum = NO_MODE;
int			vid_realmode;
int			vid_default = MODE_WINDOWED;
static int	windowed_default;
unsigned char	vid_curpal[256 * 3];
static qboolean fullsbardraw = false;

static float vid_gamma = 1.0;

HGLRC	baseRC;
HDC		maindc;

glvert_t glv;

cvar_t	gl_ztrick = { "gl_ztrick","1" };

viddef_t	vid;				// global video state

unsigned short	d_8to16table[256];
unsigned	d_8to24table[256];
unsigned char d_15to8table[65536];

float		gldepthmin, gldepthmax;

modestate_t	modestate = MS_UNINIT;

void VID_MenuDraw(void);
void VID_MenuKey(int key);

void HandleEvents();
void AppActivate(const SDL_Event* event);
char* VID_GetModeDescription(int mode);
void ClearAllStates(void);
void VID_UpdateWindowStatus(void);
void GL_Init(void);
void Sys_RestartWithMode(int modnum);

PROC glArrayElementEXT;
PROC glColorPointerEXT;
PROC glTexCoordPointerEXT;
PROC glVertexPointerEXT;

typedef void (APIENTRY* lp3DFXFUNC) (int, int, int, int, int, const void*);
lp3DFXFUNC glColorTableEXT;
qboolean is8bit = false;
qboolean isPermedia = false;
qboolean gl_mtexable = false;

//====================================

cvar_t		vid_mode = { "vid_mode","0", false };
// Note that 0 is MODE_WINDOWED
cvar_t		_vid_default_mode = { "_vid_default_mode","0", true };
// Note that 3 is MODE_FULLSCREEN_DEFAULT
cvar_t		_vid_default_mode_win = { "_vid_default_mode_win","3", true };
cvar_t		vid_wait = { "vid_wait","0" };
cvar_t		vid_nopageflip = { "vid_nopageflip","0", true };
cvar_t		_vid_wait_override = { "_vid_wait_override", "0", true };
cvar_t		vid_config_x = { "vid_config_x","800", true };
cvar_t		vid_config_y = { "vid_config_y","600", true };
cvar_t		vid_stretch_by_2 = { "vid_stretch_by_2","1", true };
cvar_t		_windowed_mouse = { "_windowed_mouse","1", true };

int			window_center_x, window_center_y, window_x, window_y, window_width, window_height;
RECT		window_rect;

// SDL globals
SDL_Window* window = NULL;
SDL_GLContext glContext;

// direct draw software compatability stuff

void VID_HandlePause(qboolean pause)
{
}

void VID_ForceLockState(int lk)
{
}

void VID_LockBuffer(void)
{
}

void VID_UnlockBuffer(void)
{
}

int VID_ForceUnlockedAndReturnState(void)
{
	return 0;
}

void D_BeginDirectRect(int x, int y, byte* pbitmap, int width, int height)
{
}

void D_EndDirectRect(int x, int y, int width, int height)
{
}

qboolean VID_SetWindowedMode(int modenum)
{
	int width  = modelist[modenum].width;
	int height = modelist[modenum].height;

	// create or resize window
	if (!window)
	{
		window = SDL_CreateWindow(
			"GLQuake",
			SDL_WINDOWPOS_CENTERED,
			SDL_WINDOWPOS_CENTERED,
			width,
			height,
			SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL
		);

		if (!window)
			Sys_Error(va("VID_SetWindowedMode: SDL_CreateWindow failed: %s", SDL_GetError()));
	}
	else {
		SDL_SetWindowSize(window, width, height);
		SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	}

	modestate = MS_WINDOWED;

	DIBWidth = width;
	DIBHeight = height;

	WindowRect.left   = 0;
	WindowRect.top	  = 0;
	WindowRect.right  = width;
	WindowRect.bottom = height;

	if (vid.conheight > height) vid.conheight = height;
	if (vid.conwidth > width) vid.conwidth = width;

	vid.width = vid.conwidth;
	vid.height = vid.conheight;
	vid.numpages = 2;

	return true;
}


qboolean VID_SetFullDIBMode(int modenum)
{
	int width  = modelist[modenum].width;
	int height = modelist[modenum].height;
	int bpp = modelist[modenum].bpp ? modelist[modenum].bpp : 32;

	if (!window)
	{
		window = SDL_CreateWindow(
			"GLQuake",
			SDL_WINDOWPOS_CENTERED,
			SDL_WINDOWPOS_CENTERED,
			width,
			height,
			SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL
		);

		if (!window)
			Sys_Error(va("VID_SetFullDIBMode: SDL_CreateWindow failed: %s", SDL_GetError()));
	}
	else {
		SDL_SetWindowSize(window, width, height);
	}

	SDL_DisplayMode mode;
	mode.w = width;
	mode.h = height;
	mode.refresh_rate = 0; // keep default if unknown
	mode.format = SDL_PIXELFORMAT_RGB888;

	if (SDL_SetWindowDisplayMode(window, &mode) != 0)
	{
		Sys_Error(va("VID_SetFullDIBMode: SDL_SetWindowDisplayMode failed: %s", SDL_GetError()));
	}
	if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN) != 0)
	{
		Sys_Error(va("VID_SetFullDIBMode: SDL_SetWindowFullScreen failed: %s", SDL_GetError()));
	}

	// update video mode
	modestate = MS_FULLDIB;

	DIBWidth = width;
	DIBHeight = height;

	WindowRect.left = 0;
	WindowRect.top = 0;
	WindowRect.right = width;
	WindowRect.bottom = height;

	vid.conwidth = width;
	vid.conheight = height;
	vid.width = width;
	vid.height = height;
	vid.rowbytes = width;
	vid.numpages = 2;

	window_x = 0;
	window_y = 0;

	return true;
}


int VID_SetMode(int modenum, unsigned char* palette)
{
	if (modenum < 0 || modenum >= nummodes)
		Sys_Error("Vid_SetMode: Invalid mode index");

	scr_disabled_for_loading = true;
	CDAudio_Pause();

	vmode_t mode = modelist[modenum];
	vid_modenum = modenum;

	int width = mode.width;
	int height = mode.height;

	Uint32 flags = SDL_GetWindowFlags(window);

	// check existing GL context
	if (glContext) {
		SDL_GL_DeleteContext(glContext);
		glContext = NULL;
	}

	if (mode.type == MS_WINDOWED)
	{
		if (!window) {
			window = SDL_CreateWindow(
				"GLQuake",
				SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
				width, height,
				SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
			);
		}
		else {
			SDL_SetWindowFullscreen(window, 0);
			SDL_SetWindowSize(window, width, height);
			SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
		}
		// mouse behavior
		if (_windowed_mouse.value && key_dest == key_game)
		{
			IN_ActivateMouse();
		}
		else {
			IN_DeactivateMouse();
		}
	}
	else if (mode.type == MS_FULLDIB)
	{
		if (!window) {
			window = SDL_CreateWindow(
				"GLQuake",
				SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
				width, height,
				SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN
			);
		}
		else {
			SDL_SetWindowSize(window, width, height);
		}

		SDL_DisplayMode sdl_dm;
		sdl_dm.w = width;
		sdl_dm.h = height;
		sdl_dm.refresh_rate = 0;
		sdl_dm.format = SDL_PIXELFORMAT_RGB888;

		if (SDL_SetWindowDisplayMode(window, &sdl_dm) != 0)
		{
			Con_SafePrintf("Warning: SDL_SetWindowDisplayMode failed: %s\n", SDL_GetError());
		}
		if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN) != 0)
		{
			Con_SafePrintf("Warning: SDL_SetWindowFullscreen failed: %s\n", SDL_GetError());
		}
		IN_ActivateMouse();
	}
	else
	{
		Sys_Error("VID_SetMode: Unsupported mode type");
	}

	if (!window)
		Sys_Error(va("VID_SetMode: SDL_CreateWindow failed: %s", SDL_GetError()));

	// create GL context
	glContext = SDL_GL_CreateContext(window);
	if (!glContext)
	{
		Sys_Error(va("Could not initialize GL context: %s", SDL_GetError()));
	}

	if (SDL_GL_MakeCurrent(window, glContext) != 0) {
		Sys_Error(va("SDL_GL_MakeCurrent failed: %s", SDL_GetError()));
	}

	modestate = mode.type;

	DIBWidth = width;
	DIBHeight = height;

	WindowRect.left = 0;
	WindowRect.top = 0;
	WindowRect.right = width;
	WindowRect.bottom = height;

	// internal window reference sizes
	int drawable_w, drawable_h;
	SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);

	vid.width = drawable_w;
	vid.height = drawable_h;
	vid.rowbytes = vid.width * 4; // 4bpp

	vid.conwidth = min(vid.conwidth, vid.width);
	vid.conheight = min(vid.conheight, vid.height);

	VID_SetPalette(palette);

	// PeekMessage equivalent
	SDL_PumpEvents();

	CDAudio_Resume();
	scr_disabled_for_loading = false;

	Con_Printf("Video mode: %dx%d%s\n", width, height, (mode.type == MS_FULLDIB ? " fullscreen" : " windowed"));

	vid.recalc_refdef = 1;

	return true;
}


/*
================
VID_UpdateWindowStatus
================
*/
void VID_UpdateWindowStatus(void)
{

	window_rect.left = window_x;
	window_rect.top = window_y;
	window_rect.right = window_x + window_width;
	window_rect.bottom = window_y + window_height;
	window_center_x = (window_rect.left + window_rect.right) / 2;
	window_center_y = (window_rect.top + window_rect.bottom) / 2;

	IN_UpdateClipCursor();
}


//====================================

BINDTEXFUNCPTR bindTexFunc;

#define TEXTURE_EXT_STRING "GL_EXT_texture_object"


void CheckTextureExtensions(void)
{
	if (!strstr(gl_extensions, "GL_EXT_texture_object"))
	{
		Con_SafePrintf("No texture object extension, using fallback.\n");
		return;
	}

	// load glBinTextture
	bindTexFunc = SDL_GL_GetProcAddress("glBindTextureEXT");
	if (!bindTexFunc)
		bindTexFunc = SDL_GL_GetProcAddress("glBindTexture");
	if (!bindTexFunc)
		Sys_Error("GL: No glBindTexture available!");
}

void CheckArrayExtensions(void)
{
	char* tmp;

	/* check for texture extension */
	tmp = (unsigned char*)glGetString(GL_EXTENSIONS);
	while (*tmp)
	{
		if (strncmp((const char*)tmp, "GL_EXT_vertex_array", strlen("GL_EXT_vertex_array")) == 0)
		{
			if (
				((glArrayElementEXT = SDL_GL_GetProcAddress("glArrayElementEXT")) == NULL) ||
				((glColorPointerEXT = SDL_GL_GetProcAddress("glColorPointerEXT")) == NULL) ||
				((glTexCoordPointerEXT = SDL_GL_GetProcAddress("glTexCoordPointerEXT")) == NULL) ||
				((glVertexPointerEXT = SDL_GL_GetProcAddress("glVertexPointerEXT")) == NULL))
			{
				Sys_Error("GetProcAddress for vertex extension failed");
				return;
			}
			return;
		}
		tmp++;
	}

	Sys_Error("Vertex array extension not present");
}

//int		texture_mode = GL_NEAREST;
//int		texture_mode = GL_NEAREST_MIPMAP_NEAREST;
//int		texture_mode = GL_NEAREST_MIPMAP_LINEAR;
int		texture_mode = GL_LINEAR;
//int		texture_mode = GL_LINEAR_MIPMAP_NEAREST;
//int		texture_mode = GL_LINEAR_MIPMAP_LINEAR;

int		texture_extension_number = 1;

#ifdef _WIN32
void CheckMultiTextureExtensions(void)
{
	if (strstr(gl_extensions, "GL_SGIS_multitexture ") && !COM_CheckParm("-nomtex")) {
		Con_Printf("Multitexture extensions found.\n");
		qglMTexCoord2fSGIS = SDL_GL_GetProcAddress("glMTexCoord2fSGIS");
		qglSelectTextureSGIS = SDL_GL_GetProcAddress("glSelectTextureSGIS");
		gl_mtexable = true;
	}
}
#else
void CheckMultiTextureExtensions(void)
{
	gl_mtexable = true;
}
#endif

/*
===============
GL_Init
===============
*/
void GL_Init(void)
{
	gl_vendor = glGetString(GL_VENDOR);
	Con_Printf("GL_VENDOR: %s\n", gl_vendor);
	gl_renderer = glGetString(GL_RENDERER);
	Con_Printf("GL_RENDERER: %s\n", gl_renderer);

	gl_version = glGetString(GL_VERSION);
	Con_Printf("GL_VERSION: %s\n", gl_version);
	gl_extensions = glGetString(GL_EXTENSIONS);
	Con_Printf("GL_EXTENSIONS: %s\n", gl_extensions);

	//	Con_Printf ("%s %s\n", gl_renderer, gl_version);

	if (strnicmp(gl_renderer, "PowerVR", 7) == 0)
		fullsbardraw = true;

	if (strnicmp(gl_renderer, "Permedia", 8) == 0)
		isPermedia = true;

	CheckTextureExtensions();
	CheckMultiTextureExtensions();

	glClearColor(1, 0, 0, 0);
	glCullFace(GL_FRONT);
	glEnable(GL_TEXTURE_2D);

	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.666);

	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glShadeModel(GL_FLAT);

	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	//	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

#if 0
	CheckArrayExtensions();

	glEnable(GL_VERTEX_ARRAY_EXT);
	glEnable(GL_TEXTURE_COORD_ARRAY_EXT);
	glVertexPointerEXT(3, GL_FLOAT, 0, 0, &glv.x);
	glTexCoordPointerEXT(2, GL_FLOAT, 0, 0, &glv.s);
	glColorPointerEXT(3, GL_FLOAT, 0, 0, &glv.r);
#endif
}

/*
=================
GL_BeginRendering

=================
*/
void GL_BeginRendering(int* x, int* y, int* width, int* height)
{
	extern cvar_t gl_clear;

	*x = *y = 0;
	*width = WindowRect.right - WindowRect.left;
	*height = WindowRect.bottom - WindowRect.top;

	//    if (!wglMakeCurrent( maindc, baseRC ))
	//		Sys_Error ("wglMakeCurrent failed");

	//	glViewport (*x, *y, *width, *height);
}


void GL_EndRendering(void)
{
	if (!scr_skipupdate || block_drawing)
		SDL_GL_SwapWindow(window);

	// handle the mouse state when windowed if that's changed
	if (modestate == MS_WINDOWED)
	{
		if (!_windowed_mouse.value) {
			if (windowed_mouse) {
				IN_DeactivateMouse();
				IN_ShowMouse();
				windowed_mouse = false;
			}
		}
		else {
			windowed_mouse = true;
			if (key_dest == key_game && !mouseactive && ActiveApp) {
				IN_ActivateMouse();
				IN_HideMouse();
			}
			else if (mouseactive && key_dest != key_game) {
				IN_DeactivateMouse();
				IN_ShowMouse();
			}
		}
	}
	if (fullsbardraw)
		Sbar_Changed();
}

void	VID_SetPalette(unsigned char* palette)
{
	unsigned char* pal;
	unsigned r, g, b;
	unsigned v;
	int     r1, g1, b1;
	int		j, k, l, m;
	unsigned short i;
	unsigned* table;
	char s[255];

	//
	// 8 8 8 encoding
	//
	pal = palette;
	table = d_8to24table;
	for (i = 0; i < 256; i++)
	{
		r = pal[0];
		g = pal[1];
		b = pal[2];
		pal += 3;

		// storea s RGBA
		v = (255 << 24) | (b << 16) | (g << 8) | (r);
		*table++ = v;
	}
	d_8to24table[255] &= 0xffffff;	// 255 is transparent

	// JACK: 3D distance calcs - k is last closest, l is the distance.
	// FIXME: Precalculate this and cache to disk.
	for (i = 0; i < (1 << 15); i++) {
		/* Maps
			000000000000000
			000000000011111 = Red  = 0x1F
			000001111100000 = Blue = 0x03E0
			111110000000000 = Grn  = 0x7C00
		*/
		r = ((i & 0x1F) << 3) + 4;
		g = ((i & 0x03E0) >> 2) + 4;
		b = ((i & 0x7C00) >> 7) + 4;
		unsigned char* pal24 = (unsigned char*)d_8to24table;
		for (v = 0, k = 0, l = INT_MAX; v < 256; v++, pal24 += 4) {
			r1 = r - pal24[0];
			g1 = g - pal24[1];
			b1 = b - pal24[2];
			j = (r1 * r1) + (g1 * g1) + (b1 * b1);
			if (j < l) {
				k = v;
				l = j;
			}
		}
		d_15to8table[i] = k;
	}
}

BOOL	gammaworks;

void	VID_ShiftPalette(unsigned char* palette)
{
	extern	byte ramps[3][256];

	//	VID_SetPalette (palette);

	//	gammaworks = SetDeviceGammaRamp (maindc, ramps);
}


void VID_SetDefaultMode(void)
{
	IN_DeactivateMouse();
}


void	VID_Shutdown(void)
{
	if (!vid_initialized)
		return;

	vid_initialized = false;

	if (modestate == MS_FULLDIB)
		SDL_SetWindowFullscreen(window, 0);

	IN_DeactivateMouse();

	if (glContext)
	{
		SDL_GL_DeleteContext(glContext);
		glContext = NULL;
	}

	if (window)
	{
		SDL_DestroyWindow(window);
		window = NULL;
	}
	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}


//==========================================================================


byte        scantokey[128] =
{
	//  0           1       2       3       4       5       6       7 
	//  8           9       A       B       C       D       E       F 
		0  ,    27,     '1',    '2',    '3',    '4',    '5',    '6',
		'7',    '8',    '9',    '0',    '-',    '=',    K_BACKSPACE, 9, // 0 
		'q',    'w',    'e',    'r',    't',    'y',    'u',    'i',
		'o',    'p',    '[',    ']',    13 ,    K_CTRL,'a',  's',      // 1 
		'd',    'f',    'g',    'h',    'j',    'k',    'l',    ';',
		'\'' ,    '`',    K_SHIFT,'\\',  'z',    'x',    'c',    'v',      // 2 
		'b',    'n',    'm',    ',',    '.',    '/',    K_SHIFT,'*',
		K_ALT,' ',   0  ,    K_F1, K_F2, K_F3, K_F4, K_F5,   // 3 
		K_F6, K_F7, K_F8, K_F9, K_F10, K_PAUSE  ,    0  , K_HOME,
		K_UPARROW,K_PGUP,'-',K_LEFTARROW,'5',K_RIGHTARROW,'+',K_END, //4 
		K_DOWNARROW,K_PGDN,K_INS,K_DEL,0,0,             0,              K_F11,
		K_F12,0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,        // 5 
		0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,
		0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,        // 6 
		0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,
		0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0         // 7 
};

byte        shiftscantokey[128] =
{
	//  0           1       2       3       4       5       6       7 
	//  8           9       A       B       C       D       E       F 
		0  ,    27,     '!',    '@',    '#',    '$',    '%',    '^',
		'&',    '*',    '(',    ')',    '_',    '+',    K_BACKSPACE, 9, // 0 
		'Q',    'W',    'E',    'R',    'T',    'Y',    'U',    'I',
		'O',    'P',    '{',    '}',    13 ,    K_CTRL,'A',  'S',      // 1 
		'D',    'F',    'G',    'H',    'J',    'K',    'L',    ':',
		'"' ,    '~',    K_SHIFT,'|',  'Z',    'X',    'C',    'V',      // 2 
		'B',    'N',    'M',    '<',    '>',    '?',    K_SHIFT,'*',
		K_ALT,' ',   0  ,    K_F1, K_F2, K_F3, K_F4, K_F5,   // 3 
		K_F6, K_F7, K_F8, K_F9, K_F10, K_PAUSE  ,    0  , K_HOME,
		K_UPARROW,K_PGUP,'_',K_LEFTARROW,'%',K_RIGHTARROW,'+',K_END, //4 
		K_DOWNARROW,K_PGDN,K_INS,K_DEL,0,0,             0,              K_F11,
		K_F12,0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,        // 5 
		0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,
		0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,        // 6 
		0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0,
		0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0  ,    0         // 7 
};


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

/*
===================================================================

MAIN WINDOW

===================================================================
*/

/*
================
ClearAllStates
================
*/
void ClearAllStates(void)
{
	int		i;

	// send an up event for each key, to make sure the server clears them all
	for (i = 0; i < 256; i++)
	{
		Key_Event(i, false);
	}

	Key_ClearStates();
	IN_ClearStates();
}

void AppActivate(const SDL_Event* event)
{
	static qboolean sound_active = true;

	qboolean active = false;
	qboolean inactive = false;

	if (event->type == SDL_WINDOWEVENT)
	{
		switch (event->window.event)
		{
		case SDL_WINDOWEVENT_FOCUS_GAINED:
		case SDL_WINDOWEVENT_RESTORED:
			active = true;
			break;

		case SDL_WINDOWEVENT_FOCUS_LOST:
		case SDL_WINDOWEVENT_MINIMIZED:
			inactive = true;
			break;
		}
	}

	ActiveApp = active;
	Minimized = inactive;

	// Sound handling
	if (!ActiveApp && sound_active)
	{
		S_BlockSound();
		sound_active = false;
	}
	else if (ActiveApp && !sound_active)
	{
		S_UnblockSound();
		sound_active = true;
	}

	// Mouse behavior
	if (active)
	{
		if (modestate == MS_FULLDIB ||
			(modestate == MS_WINDOWED && _windowed_mouse.value && key_dest == key_game))
		{
			IN_ActivateMouse();
		}

		ClearAllStates();
	}

	if (inactive)
	{
		IN_DeactivateMouse();
		ClearAllStates();
	}
}


/* main window procedure */
void HandleEvents()
{
	SDL_Event event;

	while (SDL_PollEvent(&event))
	{
		switch (event.type) {
		case SDL_QUIT:
			// clode the window
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
				ActiveApp = true;
				IN_ActivateMouse();
				break;
			case SDL_WINDOWEVENT_FOCUS_LOST:
			case SDL_WINDOWEVENT_MINIMIZED:
				ActiveApp = false;
				IN_DeactivateMouse();
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
			IN_MouseMove(event.motion.xrel, event.motion.yrel);
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
VID_GetModeDescription
=================
*/
char* VID_GetModeDescription(int mode)
{
	char* pinfo;
	vmode_t* pv;
	static char	temp[100];

	if ((mode < 0) || (mode >= nummodes))
		return NULL;

	if (!leavecurrentmode)
	{
		pv = VID_GetModePtr(mode);
		pinfo = pv->modedesc;
	}
	else
	{
		sprintf(temp, "Desktop resolution (%dx%d)",
			modelist[MODE_FULLSCREEN_DEFAULT].width,
			modelist[MODE_FULLSCREEN_DEFAULT].height);
		pinfo = temp;
	}

	return pinfo;
}


// KJB: Added this to return the mode driver name in description for console

char* VID_GetExtModeDescription(int mode)
{
	static char	pinfo[40];
	vmode_t* pv;

	if ((mode < 0) || (mode >= nummodes))
		return NULL;

	pv = VID_GetModePtr(mode);
	if (modelist[mode].type == MS_FULLDIB)
	{
		if (!leavecurrentmode)
		{
			sprintf(pinfo, "%s fullscreen", pv->modedesc);
		}
		else
		{
			sprintf(pinfo, "Desktop resolution (%dx%d)",
				modelist[MODE_FULLSCREEN_DEFAULT].width,
				modelist[MODE_FULLSCREEN_DEFAULT].height);
		}
	}
	else
	{
		if (modestate == MS_WINDOWED)
			sprintf(pinfo, "%s windowed", pv->modedesc);
		else
			sprintf(pinfo, "windowed");
	}

	return pinfo;
}


/*
=================
VID_DescribeCurrentMode_f
=================
*/
void VID_DescribeCurrentMode_f(void)
{
	Con_Printf("%s\n", VID_GetExtModeDescription(vid_modenum));
}


/*
=================
VID_NumModes_f
=================
*/
void VID_NumModes_f(void)
{

	if (nummodes == 1)
		Con_Printf("%d video mode is available\n", nummodes);
	else
		Con_Printf("%d video modes are available\n", nummodes);
}


/*
=================
VID_DescribeMode_f
=================
*/
void VID_DescribeMode_f(void)
{
	int		t, modenum;

	modenum = Q_atoi(Cmd_Argv(1));

	t = leavecurrentmode;
	leavecurrentmode = 0;

	Con_Printf("%s\n", VID_GetExtModeDescription(modenum));

	leavecurrentmode = t;
}


/*
=================
VID_DescribeModes_f
=================
*/
void VID_DescribeModes_f(void)
{
	int			i, lnummodes, t;
	char* pinfo;
	vmode_t* pv;

	lnummodes = VID_NumModes();

	t = leavecurrentmode;
	leavecurrentmode = 0;

	for (i = 1; i < lnummodes; i++)
	{
		pv = VID_GetModePtr(i);
		pinfo = VID_GetExtModeDescription(i);
		Con_Printf("%2d: %s\n", i, pinfo);
	}

	leavecurrentmode = t;
}


void VID_InitDIB(void)
{
	int width = 640;
	int height;

	// argument passing
	if (COM_CheckParm("-width"))
		width = Q_atoi(com_argv[COM_CheckParm("-width") + 1]);

	if (COM_CheckParm("-height"))
		height = Q_atoi(com_argv[COM_CheckParm("-height") + 1]);
	else
		height = width * 240 / 320;

	if (width < 320)	width  = 320;
	if (height < 240)	height = 240;

	modelist[0].type = MS_WINDOWED;
	modelist[0].width = width;
	modelist[0].height = height;
	modelist[0].modenum = MODE_WINDOWED;
	modelist[0].dib = 1;
	modelist[0].fullscreen = 0;
	modelist[0].halfscreen = 0;
	modelist[0].bpp = 0;

	nummodes = 1;
}


/*
=================
VID_InitFullDIB
=================
*/
void VID_InitFullDIB()
{
	int display_idx = 0;
	int mode_count = SDL_GetNumDisplayModes(display_idx);

	if (mode_count < 1)
	{
		Con_SafePrintf("No fullscreen display modes found\n");
		return;
	}

	SDL_DisplayMode mode;
	for (int i = 0; i < mode_count; i++)
	{
		if (SDL_GetDisplayMode(display_idx, i, &mode) != 0)
			continue;

			if (mode.format != SDL_PIXELFORMAT_RGBA8888 &&
				mode.format != SDL_PIXELFORMAT_ARGB8888 &&
				mode.format != SDL_PIXELFORMAT_RGB888
				)
				continue;

			if (mode.w > MAXWIDTH || mode.h > MAXHEIGHT)
				continue;

			// check duplicates before adding
			int duplicate = 0;
			for (int j = 1; j < nummodes; j++)
			{
				if (modelist[j].width == mode.w && modelist[j].height == mode.h)
				{
					duplicate = 1;
					break;
				}
			}

			// add to existing modelist format
			if (!duplicate && nummodes < MAX_MODE_LIST)
			{
				modelist[nummodes].type = MS_FULLDIB;
				modelist[nummodes].width = mode.w;
				modelist[nummodes].height = mode.h;
				int bpp = 32;
				switch (mode.format)
				{
				case SDL_PIXELFORMAT_RGB888:
					bpp = 24;
					break;
				default:
					bpp = 32;
					break;
				}
				modelist[nummodes].bpp = bpp;
				modelist[nummodes].fullscreen = 1;
				modelist[nummodes].dib = 1;
				modelist[nummodes].modenum = 0;
				modelist[nummodes].halfscreen = 0;

				sprintf(modelist[nummodes].modedesc, "%dx%d", mode.w, mode.h);
				nummodes++;
			}
	}

	if (nummodes == 0)
		Con_SafePrintf("No fullscreen display modes found\n");
}

qboolean VID_Is8bit() {
	return is8bit;
}

#define GL_SHARED_TEXTURE_PALETTE_EXT 0x81FB

void VID_Init8bitPalette()
{
	// Check for 8bit Extensions and initialize them.
	int i;
	char thePalette[256 * 3];
	char* oldPalette, * newPalette;

	glColorTableEXT = (void*)SDL_GL_GetProcAddress("glColorTableEXT");
/*
	if (!glColorTableEXT || strstr(gl_extensions, "GL_EXT_shared_texture_palette") ||
		COM_CheckParm("-no8bit"))
		return;

	Con_SafePrintf("8-bit GL extensions enabled.\n");
	glEnable(GL_SHARED_TEXTURE_PALETTE_EXT);
	oldPalette = (char*)d_8to24table; //d_8to24table3dfx;
	newPalette = thePalette;
	for (i = 0;i < 256;i++) {
		*newPalette++ = *oldPalette++;
		*newPalette++ = *oldPalette++;
		*newPalette++ = *oldPalette++;
		oldPalette++;
	}
	glColorTableEXT(GL_SHARED_TEXTURE_PALETTE_EXT, GL_RGB, 256, GL_RGB, GL_UNSIGNED_BYTE,
		(void*)thePalette);
	is8bit = TRUE;
	*/
}

static void Check_Gamma(unsigned char* pal)
{
	float	f, inf;
	unsigned char	palette[768];
	int		i;

	if ((i = COM_CheckParm("-gamma")) == 0) {
		if ((gl_renderer && strstr(gl_renderer, "Voodoo")) ||
			(gl_vendor && strstr(gl_vendor, "3Dfx")))
			vid_gamma = 1;
		else
			vid_gamma = 0.7; // default to 0.7 on non-3dfx hardware
	}
	else
		vid_gamma = Q_atof(com_argv[i + 1]);

	for (i = 0; i < 768; i++)
	{
		f = pow((pal[i] + 1) / 256.0, vid_gamma);
		inf = f * 255 + 0.5;
		if (inf < 0)
			inf = 0;
		if (inf > 255)
			inf = 255;
		palette[i] = inf;
	}

	memcpy(pal, palette, sizeof(palette));
}

/*
===================
VID_Init
===================
*/
void	VID_Init(unsigned char* palette)
{
	int		i, existingmode;
	int		basenummodes, width, height, bpp, findbpp, done;
	byte* ptmp;
	char	gldir[MAX_OSPATH];
	DEVMODE	devmode;

	memset(&devmode, 0, sizeof(devmode));

	Cvar_RegisterVariable(&vid_mode);
	Cvar_RegisterVariable(&vid_wait);
	Cvar_RegisterVariable(&vid_nopageflip);
	Cvar_RegisterVariable(&_vid_wait_override);
	Cvar_RegisterVariable(&_vid_default_mode);
	Cvar_RegisterVariable(&_vid_default_mode_win);
	Cvar_RegisterVariable(&vid_config_x);
	Cvar_RegisterVariable(&vid_config_y);
	Cvar_RegisterVariable(&vid_stretch_by_2);
	Cvar_RegisterVariable(&_windowed_mouse);
	Cvar_RegisterVariable(&gl_ztrick);

	Cmd_AddCommand("vid_nummodes", VID_NumModes_f);
	Cmd_AddCommand("vid_describecurrentmode", VID_DescribeCurrentMode_f);
	Cmd_AddCommand("vid_describemode", VID_DescribeMode_f);
	Cmd_AddCommand("vid_describemodes", VID_DescribeModes_f);

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
		Sys_Error(va("VID_Init: Couldn't load SDL video subsystem: %s", SDL_GetError()));

	VID_InitDIB();
	basenummodes = nummodes = 1;

	VID_InitFullDIB();

	if (COM_CheckParm("-window"))
	{
		windowed = true;
		vid_default = MODE_WINDOWED;
	}
	else
	{
		if (nummodes == 1)
			Sys_Error("No RGB fullscreen modes available");

		windowed = false;
		vid_default = 1;

		if (COM_CheckParm("-mode"))
		{
			vid_default = Q_atoi(com_argv[COM_CheckParm("-mode") + 1]);
		}
		else
		{
			if (COM_CheckParm("-current"))
			{
				modelist[MODE_FULLSCREEN_DEFAULT].width =
					GetSystemMetrics(SM_CXSCREEN);
				modelist[MODE_FULLSCREEN_DEFAULT].height =
					GetSystemMetrics(SM_CYSCREEN);
				vid_default = MODE_FULLSCREEN_DEFAULT;
				leavecurrentmode = 1;
			}
			else
			{
				if (COM_CheckParm("-width"))
				{
					width = Q_atoi(com_argv[COM_CheckParm("-width") + 1]);
				}
				else
				{
					width = GetSystemMetrics(SM_CXSCREEN);
				}

				if (COM_CheckParm("-bpp"))
				{
					bpp = Q_atoi(com_argv[COM_CheckParm("-bpp") + 1]);
					findbpp = 0;
				}
				else
				{
					bpp = 15;
					findbpp = 1;
				}

				if (COM_CheckParm("-height"))
					height = Q_atoi(com_argv[COM_CheckParm("-height") + 1]);

				// if they want to force it, add the specified mode to the list
				if (COM_CheckParm("-force") && (nummodes < MAX_MODE_LIST))
				{
					modelist[nummodes].type = MS_FULLDIB;
					modelist[nummodes].width = width;
					modelist[nummodes].height = height;
					modelist[nummodes].modenum = 0;
					modelist[nummodes].halfscreen = 0;
					modelist[nummodes].dib = 1;
					modelist[nummodes].fullscreen = 1;
					modelist[nummodes].bpp = bpp;
					sprintf(modelist[nummodes].modedesc, "%dx%dx%d",
						devmode.dmPelsWidth, devmode.dmPelsHeight,
						devmode.dmBitsPerPel);

					for (i = nummodes, existingmode = 0; i < nummodes; i++)
					{
						if ((modelist[nummodes].width == modelist[i].width) &&
							(modelist[nummodes].height == modelist[i].height) &&
							(modelist[nummodes].bpp == modelist[i].bpp))
						{
							existingmode = 1;
							break;
						}
					}

					if (!existingmode)
					{
						nummodes++;
					}
				}

				done = 0;

				do
				{
					if (COM_CheckParm("-height"))
					{
						height = Q_atoi(com_argv[COM_CheckParm("-height") + 1]);

						for (i = 1, vid_default = 0; i < nummodes; i++)
						{
							if ((modelist[i].width == width) &&
								(modelist[i].height == height) &&
								(modelist[i].bpp == bpp))
							{
								vid_default = i;
								done = 1;
								break;
							}
						}
					}
					else
					{
						for (i = 1, vid_default = 0; i < nummodes; i++)
						{
							if ((modelist[i].width == width) && (modelist[i].bpp == bpp))
							{
								vid_default = i;
								done = 1;
								break;
							}
						}
					}

					if (!done)
					{
						if (findbpp)
						{
							switch (bpp)
							{
							case 15:
								bpp = 16;
								break;
							case 16:
								bpp = 32;
								break;
							case 32:
								bpp = 24;
								break;
							case 24:
								done = 1;
								break;
							}
						}
						else
						{
							done = 1;
						}
					}
				} while (!done);

				if (!vid_default)
				{
					Sys_Error("Specified video mode not available");
				}
			}
		}
	}

	vid_initialized = true;

	if ((i = COM_CheckParm("-conwidth")) != 0)
		vid.conwidth = Q_atoi(com_argv[i + 1]);
	else
		vid.conwidth = 640;

	vid.conwidth &= 0xfff8; // make it a multiple of eight

	if (vid.conwidth < 320)
		vid.conwidth = 320;

	// pick a conheight that matches with correct aspect
	vid.conheight = vid.conwidth * 3 / 4;

	if ((i = COM_CheckParm("-conheight")) != 0)
		vid.conheight = Q_atoi(com_argv[i + 1]);
	if (vid.conheight < 200)
		vid.conheight = 200;

	vid.maxwarpwidth = WARP_WIDTH;
	vid.maxwarpheight = WARP_HEIGHT;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong(*((int*)vid.colormap + 2048));

	Check_Gamma(palette);
	VID_SetPalette(palette);

	VID_SetMode(vid_default, palette);

	GL_Init();

	sprintf(gldir, "%s/glquake", com_gamedir);
	Sys_mkdir(gldir);

	vid_realmode = vid_modenum;

	// Check for 3DFX Extensions and initialize them.
	VID_Init8bitPalette();

	vid_menudrawfn = VID_MenuDraw;
	vid_menukeyfn = VID_MenuKey;

	strcpy(badmode.modedesc, "Bad mode");
	vid_canalttab = true;

	if (COM_CheckParm("-fullsbar"))
		fullsbardraw = true;
}


//========================================================
// Video menu stuff
//========================================================

extern void M_Menu_Options_f(void);
extern void M_Print(int cx, int cy, char* str);
extern void M_PrintWhite(int cx, int cy, char* str);
extern void M_DrawCharacter(int cx, int line, int num);
extern void M_DrawTransPic(int x, int y, qpic_t* pic);
extern void M_DrawPic(int x, int y, qpic_t* pic);

static int	vid_line, vid_wmodes;

typedef struct
{
	int		modenum;
	char* desc;
	int		iscur;
} modedesc_t;

#define MAX_COLUMN_SIZE		9
#define MODE_AREA_HEIGHT	(MAX_COLUMN_SIZE + 2)
#define MAX_MODEDESCS		(MAX_COLUMN_SIZE*3)

static modedesc_t	modedescs[MAX_MODEDESCS];

/*
================
VID_MenuDraw
================
*/
void VID_MenuDraw(void)
{
	qpic_t* p;
	char* ptr;
	int			lnummodes, i, j, k, column, row, dup, dupmode;
	char		temp[100];
	vmode_t* pv;

	p = Draw_CachePic("gfx/vidmodes.lmp");
	M_DrawPic((320 - p->width) / 2, 4, p);

	vid_wmodes = 0;
	lnummodes = VID_NumModes();

	for (i = 1; (i < lnummodes) && (vid_wmodes < MAX_MODEDESCS); i++)
	{
		ptr = VID_GetModeDescription(i);
		pv = VID_GetModePtr(i);

		k = vid_wmodes;

		modedescs[k].modenum = i;
		modedescs[k].desc = ptr;
		modedescs[k].iscur = 0;

		if (i == vid_modenum)
			modedescs[k].iscur = 1;

		vid_wmodes++;

	}

	if (vid_wmodes > 0)
	{
		M_Print(2 * 8, 36 + 0 * 8, "Fullscreen Modes (WIDTHxHEIGHTxBPP)");

		column = 8;
		row = 36 + 2 * 8;

		for (i = 0; i < vid_wmodes; i++)
		{
			if (modedescs[i].iscur)
				M_PrintWhite(column, row, modedescs[i].desc);
			else
				M_Print(column, row, modedescs[i].desc);\
				if (i == vid_line)
					M_DrawCharacter(column - 8, row, 12 + ((int)(realtime * 4) & 1));

			column += 13 * 8;

			if ((i % VID_ROW_SIZE) == (VID_ROW_SIZE - 1))
			{
				column = 8;
				row += 8;
			}
		}
	}

	M_Print(3 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 2,
		"Video modes can be set in here.");
	M_Print(3 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 3,
		"Make sure you save the game");
	M_Print(3 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 4,
		"before changing the game resolution.");
	M_Print(3 * 8, 36 + MODE_AREA_HEIGHT * 8 + 8 * 6,
		"The game restarts itself every mode change.");
}


/*
================
VID_MenuKey
================
*/
void VID_MenuKey(int key)
{
	int ln = vid_wmodes;
	switch (key)
	{
	case K_ESCAPE:
		S_LocalSound("misc/menu1.wav");
		M_Menu_Options_f();
		break;
	case K_LEFTARROW:
		S_LocalSound("misc/menu1.wav");
		vid_line--;
		if (vid_line < 0) vid_line = ln - 1;
		break;
	case K_RIGHTARROW:
		S_LocalSound("misc/menu1.wav");
		vid_line++;
		if (vid_line >= ln) vid_line = 0;
		break;
	case K_UPARROW:
		S_LocalSound("misc/menu1.wav");
		vid_line -= VID_ROW_SIZE;
		if (vid_line < 0)
			vid_line = (((ln - 1) / VID_ROW_SIZE) * VID_ROW_SIZE) + (vid_line + VID_ROW_SIZE);
		if (vid_line >= ln) vid_line = ln - 1;
		break;
	case K_DOWNARROW:
		S_LocalSound("misc/menu1.wav");
		vid_line += VID_ROW_SIZE;
		if (vid_line >= ln) vid_line = vid_line % VID_ROW_SIZE;
		if (vid_line >= ln) vid_line = ln - 1;
		break;
	case K_ENTER:
	{
		// apply selected mode and restart with -mode <modenum>
		int sel = modedescs[vid_line].modenum;
		if (sel <= 0) break;

		S_LocalSound("misc/menu2.wav");

		// set cvar so config reflects selection
		Cvar_SetValue("vid_mode", (float)sel);

		// ask command buffer to set vid_mode (keep behaviour consistent)
		Cbuf_AddText(va("vid_mode %d\n", sel));

		// restart the game to apply settings
		Con_SafePrintf("Restarting to apply video mode %s ...\n", VID_GetModeDescription(sel));
		Sys_RestartWithMode(sel);
		break;
	}
	default:
		break;
	}
}