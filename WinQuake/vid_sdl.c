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
// vid_sdl.c -- SDL video driver

#include "quakedef.h"
#ifdef _WIN32
#include "winquake.h"
#include "resource.h"
#endif
#include "d_local.h"
#include <glad/glad.h>

#ifndef _WIN32
#include "linuxcrossplat.h"
#endif

#define MAX_MODE_LIST	100
#define VID_ROW_SIZE	4

qboolean	dibonly;

#ifdef _WIN32
extern int		Minimized;
#endif

qboolean	DDActive;

int			window_center_x, window_center_y, window_x, window_y, window_width, window_height;

static qboolean	startwindowed = 0, windowed_mode_set;
static int		startup_count = 0;
static qboolean	vid_initialized = false, vid_palettized;
static int		lockcount;
static int		vid_fulldib_on_focus_mode;
static qboolean	force_minimized, in_mode_set, is_mode0x13, force_mode_set;
static int		vid_stretched, windowed_mouse;
static qboolean	palette_changed, syscolchg, vid_mode_set, hide_window, pal_is_nostatic;
static qboolean paused_for_focus = false;

extern int mx_accum;
extern int my_accum;
extern qboolean	mouseactive;  // from in_win.c

extern viddef_t	vid;				// global video state

extern cvar_t v_gamma;

#define MODE_WINDOWED			0
#define MODE_SETTABLE_WINDOW	2
#define NO_MODE					(MODE_WINDOWED - 1)
#define MODE_FULLSCREEN_DEFAULT	(MODE_WINDOWED + 3)

// Note that 0 is MODE_WINDOWED
cvar_t		vid_mode = {"vid_mode","0", false};
// Note that 0 is MODE_WINDOWED
cvar_t		_vid_default_mode = {"_vid_default_mode","0", true};
// Note that 3 is MODE_FULLSCREEN_DEFAULT
cvar_t		_vid_default_mode_win = {"_vid_default_mode_win","0", true};
cvar_t		vid_wait = {"vid_wait","0"};
cvar_t		vid_nopageflip = {"vid_nopageflip","0", true};
cvar_t		_vid_wait_override = {"_vid_wait_override", "0", true};
cvar_t		vid_config_x = {"vid_config_x","800", true};
cvar_t		vid_config_y = {"vid_config_y","600", true};
cvar_t		vid_stretch_by_2 = {"vid_stretch_by_2","1", true};
cvar_t		_windowed_mouse = {"_windowed_mouse","0", true};
cvar_t		vid_fullscreen_mode = {"vid_fullscreen_mode","0", true};
cvar_t		vid_windowed_mode = {"vid_windowed_mode","0", true};
cvar_t		block_switch = {"block_switch","0", true};
cvar_t		vid_window_x = {"vid_window_x", "0", true};
cvar_t		vid_window_y = {"vid_window_y", "0", true};
cvar_t		vid_refreshrate = {"vid_refreshrate", "0", true};
cvar_t		vid_vsync = { "vid_vsync", "0", true };
cvar_t		vid_renderer = { "vid_renderer", "0", true }; // 0 = sdl, 1 = opengl
cvar_t		vid_render_scale = { "vid_render_scale", "100", true };

typedef struct {
	int		width;
	int		height;
} lmode_t;

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

void AppActivate(const SDL_Event* event);
int MapKey(int scancode);
void VID_UpdateWindowStatus(void);
SDL_Window* window = NULL;
SDL_Surface* quake_surface = NULL; // 8-bit quake render surface
SDL_Surface* quake_surface_world = NULL;
SDL_Surface* quake_surface32 = NULL;

SDL_GLContext gl_context = NULL;
static int gl_uniform_texture_world = -1;
static int gl_uniform_texture_ui = -1;
static int gl_uniform_palette = -1;
static int gl_uniform_time = -1;
static int gl_uniform_dowarp = -1;
static int gl_uniform_width = -1;
static int gl_uniform_height = -1;
unsigned int gl_palette_texture = 0;
unsigned int gl_texture = 0;
unsigned int gl_texture_world = 0;
unsigned int gl_vbo = 0;
unsigned int gl_shader_program = 0;

static unsigned char gamma_lut[256];
static float cached_gamma = -1.0f;

static int cached_draw_w = 0;
static int cached_draw_h = 0;

static const char* vertex_shader_src =
    "#version 110\n"
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

static const char* fragment_shader_src =
    "#version 110\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_texture_world;\n"
    "uniform sampler2D u_texture_ui;\n"
    "uniform sampler2D u_palette;\n"
    "uniform float u_time;\n"
    "uniform float u_dowarp;\n"
	"uniform float u_width;\n"
	"uniform float u_height;\n"
    "void main() {\n"
    "    vec2 uv = v_texcoord;\n"
    "    if (u_dowarp > 0.5) {\n"
    "        uv.x += sin((uv.y * u_height + u_time * 20.0) * 6.28318 / 128.0) * (3.0 / u_width);\n"
    "        uv.y += sin((uv.x * u_width + u_time * 20.0) * 6.28318 / 128.0) * (3.0 / u_height);\n"
    "    }\n"
    "    float world_val = texture2D(u_texture_world, uv).r;\n"
    "    float ui_val = texture2D(u_texture_ui, v_texcoord).r;\n"
    "    float final_val = world_val;\n"
    "    if (ui_val < 0.995) {\n"
    "        final_val = ui_val;\n"
    "    }\n"
    "    float index = final_val * (255.0 / 256.0) + (0.5 / 256.0);\n"
    "    vec4 color = texture2D(u_palette, vec2(index, 0.0));\n"
    "    gl_FragColor = color;\n"
    "}\n";

static void VID_InitOpenGL(int width, int height)
{
	unsigned int vs = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vs, 1, &vertex_shader_src, NULL);
	glCompileShader(vs);

	unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fs, 1, &fragment_shader_src, NULL);
	glCompileShader(fs);

	gl_shader_program = glCreateProgram();
	glAttachShader(gl_shader_program, vs);
	glAttachShader(gl_shader_program, fs);

	glBindAttribLocation(gl_shader_program, 0, "a_pos");
	glBindAttribLocation(gl_shader_program, 1, "a_texcoord");

	glLinkProgram(gl_shader_program);

	glDeleteShader(vs);
	glDeleteShader(fs);

	glGenTextures(1, &gl_texture);
	glBindTexture(GL_TEXTURE_2D, gl_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

	glGenTextures(1, &gl_texture_world);
	glBindTexture(GL_TEXTURE_2D, gl_texture_world);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

	float vertices[] = {
		// pos      // tex
		-1.0f,  1.0f, 0.0f, 0.0f,
		 1.0f,  1.0f, 1.0f, 0.0f,
		-1.0f, -1.0f, 0.0f, 1.0f,
		 1.0f, -1.0f, 1.0f, 1.0f
	};

	glGenBuffers(1, &gl_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	
	gl_uniform_texture_world = glGetUniformLocation(gl_shader_program, "u_texture_world");
	gl_uniform_texture_ui = glGetUniformLocation(gl_shader_program, "u_texture_ui");
	gl_uniform_palette = glGetUniformLocation(gl_shader_program, "u_palette");
	gl_uniform_time = glGetUniformLocation(gl_shader_program, "u_time");
	gl_uniform_dowarp = glGetUniformLocation(gl_shader_program, "u_dowarp");
	gl_uniform_width = glGetUniformLocation(gl_shader_program, "u_width");
	gl_uniform_height = glGetUniformLocation(gl_shader_program, "u_height");

	glGenTextures(1, &gl_palette_texture);
	glBindTexture(GL_TEXTURE_2D, gl_palette_texture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

static void VID_ShutdownOpenGL()
{
	if (gl_texture) { glDeleteTextures(1, &gl_texture); gl_texture = 0; }
	if (gl_texture_world) { glDeleteTextures(1, &gl_texture_world); gl_texture_world = 0; }
	if (gl_vbo) { glDeleteBuffers(1, &gl_vbo); gl_vbo = 0; }
	if (gl_shader_program) { glDeleteProgram(gl_shader_program); gl_shader_program = 0; }
	if (gl_context) { SDL_GL_DeleteContext(gl_context); gl_context = NULL; }
}

static qboolean VID_CheckGLSupport(void)
{
	// opengl 2.0 request
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);

	SDL_Window* temp_window = SDL_CreateWindow(
		"gl check",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		1, 1,
		SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL
	);

	if (!temp_window)
		return false;

	SDL_GLContext temp_ctx = SDL_GL_CreateContext(temp_window);
	if (!temp_ctx) {
		SDL_DestroyWindow(temp_window);
		return false;
	}

	if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
		SDL_GL_DeleteContext(temp_ctx);
		SDL_DestroyWindow(temp_window);
		return false;
	}

	SDL_GL_DeleteContext(temp_ctx);
	SDL_DestroyWindow(temp_window);
	return true;
}

// video menu state
static int vid_menuline = 0;
static int vid_nummodes = 0;
static int vid_refresh_rates[32];
static int vid_num_refresh = 0;
static int vid_refresh_index = 0;
static int vid_current_mode = 0; // current selection of mode
static int vid_menu_fullscreen = 2;
static int vid_menu_vsync = 0;
static qboolean vid_test_active = false;
static double vid_test_start = 0.0;
static double vid_test_duration = 15.0; // 15 seconds

static int prev_width = 0, prev_height = 0;
static int prev_refresh = 60;
static int prev_vsync = 0;
static int prev_fullscreen = 2; // 2 = exclusive fullscreen
static int prev_renderer = 1;
static int prev_render_scale = 100;
static int vid_menu_renderer = 0;
static int vid_menu_render_scale = 100;
static qboolean gl_supported = false;

extern cvar_t vid_refreshrate;
extern cvar_t vid_vsync;
extern cvar_t vid_fullscreen_mode;

/*
=================
VID_CollectRefreshRates
=================
*/
static void VID_CollectRefreshRates(int width, int height)
{
	int display_index = SDL_GetWindowDisplayIndex(window);
	if (display_index < 0) display_index = 0;

	vid_num_refresh = 0;
	int count = SDL_GetNumDisplayModes(display_index);
	SDL_DisplayMode mode;

	// always add 60Hz as a safe default
	vid_refresh_rates[vid_num_refresh++] = 60;

	for (int i = 0; i < count && vid_num_refresh < 32; i++) {
		if (SDL_GetDisplayMode(display_index, i, &mode) == 0) {
			if (mode.w == width && mode.h == height && mode.refresh_rate > 0) {
				// check duplicates
				qboolean found = false;
				for (int j = 0; j < vid_num_refresh; j++) {
					if (vid_refresh_rates[j] == mode.refresh_rate) {
						found = true;
						break;
					}
				}
				if (!found) {
					vid_refresh_rates[vid_num_refresh++] = mode.refresh_rate;
				}
			}
		}
	}

	// sort
	for (int i = 0; i < vid_num_refresh - 1; i++) {
		for (int j = i + 1; j < vid_num_refresh; j++) {
			if (vid_refresh_rates[i] > vid_refresh_rates[j]) {
				int temp = vid_refresh_rates[i];
				vid_refresh_rates[i] = vid_refresh_rates[j];
				vid_refresh_rates[j] = temp;
			}
		}
	}

	// find current refresh rate index
	vid_refresh_index = 0;
	int current_rate = (int)vid_refreshrate.value;
	if (current_rate == 0) current_rate = 60;

	for (int i = 0; i < vid_num_refresh; i++) {
		if (vid_refresh_rates[i] == current_rate) {
			vid_refresh_index = i;
			break;
		}
	}
}

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
	window_center_x = window_x + window_width / 2;
	window_center_y = window_y + window_height / 2;

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
	SDL_DisplayMode desktop_mode, mode;
	int display = 0; // primary display
	int lowres[][2] = { {320,200},{320,240},{400,300},{512,384} };
	vmode_t tmp;

	if (SDL_GetDesktopDisplayMode(display, &desktop_mode) != 0) {
		desktop_mode.w = 640; desktop_mode.h = 480;
	}

	// get desktop mode for reference
	int win_width = desktop_mode.w * 0.8;
	int win_height = desktop_mode.h * 0.8;
	if (win_width < 640) win_width = 640;
	if (win_height < 480) win_height = 480;
	if (win_width > desktop_mode.w) win_width = desktop_mode.w;
	if (win_height > desktop_mode.h) win_height = desktop_mode.h;

	modelist[nummodes].type = MS_WINDOWED;
	modelist[nummodes].width = win_width;
	modelist[nummodes].height = win_height;
	sprintf(modelist[nummodes].modedesc, "%dx%d", win_width, win_height);
	modelist[nummodes].modenum = nummodes;
	modelist[nummodes].stretched = 0;
	modelist[nummodes].dib = 1;
	modelist[nummodes].fullscreen = 0;
	modelist[nummodes].halfscreen = 0;
	modelist[nummodes].bpp = SDL_BITSPERPIXEL(desktop_mode.format);
	nummodes++;

	for (int k = 0; k < 4; k++) {
		int w = lowres[k][0], h = lowres[k][1];
		qboolean dup = false;
		for (int j = 0; j < nummodes; j++)
			if (modelist[j].width == w && modelist[j].height == h) { dup = true; break; }
		if (!dup && w <= desktop_mode.w && h <= desktop_mode.h) {
			modelist[nummodes].type = MS_WINDOWED;
			modelist[nummodes].width = w;
			modelist[nummodes].height = h;
			sprintf(modelist[nummodes].modedesc, "%dx%d", w, h);
			modelist[nummodes].modenum = nummodes;
			modelist[nummodes].stretched = 0;
			modelist[nummodes].dib = 1;
			modelist[nummodes].fullscreen = 0;
			modelist[nummodes].halfscreen = 0;
			modelist[nummodes].bpp = 8;
			nummodes++;
		}
	}

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
					if (modelist[j].width == mode.w && modelist[j].height == mode.h) {
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
	if (nummodes <= 1) {
		modelist[nummodes].type = MS_FULLSCREEN;
		modelist[nummodes].width = 640;
		modelist[nummodes].height = 480;
		sprintf(modelist[nummodes].modedesc, "640x480");
		modelist[nummodes].modenum = nummodes;
		modelist[nummodes].stretched = 0;
		modelist[nummodes].dib = 0;
		modelist[nummodes].fullscreen = 1;
		modelist[nummodes].halfscreen = 0;
		modelist[nummodes].bpp = 8;
		nummodes++;
	}

	for (int i = 0; i < nummodes - 1; i++)
		for (int j = i + 1; j < nummodes; j++)
			if (modelist[i].width > modelist[j].width ||
				(modelist[i].width == modelist[j].width && modelist[i].height > modelist[j].height)) {
				tmp = modelist[i];
				modelist[i] = modelist[j];
				modelist[j] = tmp;
			}
}

qboolean VID_SetWindowedMode(int modenum)
{
	int targ_winternal;
	int targ_hinternal;
	int scale;

	VID_ShutdownOpenGL();
	if (quake_surface) { SDL_FreeSurface(quake_surface); quake_surface = NULL; }
	if (quake_surface_world) { SDL_FreeSurface(quake_surface_world); quake_surface_world = NULL; }
	if (quake_surface32) { SDL_FreeSurface(quake_surface32); quake_surface32 = NULL; }
	if (window) { SDL_DestroyWindow(window); window = NULL; }

	Uint32 flags = SDL_WINDOW_SHOWN;
	if (gl_supported && vid_renderer.value != 0)
		flags |= SDL_WINDOW_OPENGL;

	// center automatically
	int posx = SDL_WINDOWPOS_CENTERED;
	int posy = SDL_WINDOWPOS_CENTERED;

	targ_winternal = modelist[modenum].width;
	targ_hinternal = modelist[modenum].height;

	scale = (int)vid_render_scale.value;
	if (scale < 50) scale = 50;
	if (scale > 150) scale = 150;
	targ_winternal = (targ_winternal * scale) / 100;
	targ_hinternal = (targ_hinternal * scale) / 100;
	if (targ_winternal < 320) targ_winternal = 320;
	if (targ_hinternal < 200) targ_hinternal = 200;
	if (targ_winternal > MAXWIDTH) targ_winternal = MAXWIDTH;
	if (targ_hinternal > MAXHEIGHT) targ_hinternal = MAXHEIGHT;

	if (gl_supported && vid_renderer.value != 0) {
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
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

	if (gl_supported && vid_renderer.value != 0) {
		gl_context = SDL_GL_CreateContext(window);
		if (!gl_context) {
			Sys_Error("SDL_GL_CreateContext failed: %s", SDL_GetError());
		}

		if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
			Sys_Error("Failed to initialize GLAD");
		}

		if (vid_vsync.value)
			SDL_GL_SetSwapInterval(1);
		else
			SDL_GL_SetSwapInterval(0);

		VID_InitOpenGL(targ_winternal, targ_hinternal);
	}

	quake_surface = SDL_CreateRGBSurface(0, targ_winternal, targ_hinternal, 8, 0, 0, 0, 0);
	if (!quake_surface) {
		Sys_Error("SDL_CreateRGBSurface failed: %s", SDL_GetError());
	}
	quake_surface_world = SDL_CreateRGBSurface(0, targ_winternal, targ_hinternal, 8, 0, 0, 0, 0);
	quake_surface32 = SDL_CreateRGBSurface(0, targ_winternal, targ_hinternal, 32, 0, 0, 0, 0);

	vid.buffer = vid.conbuffer = vid.direct = (byte*)quake_surface->pixels;
	vid.rowbytes = vid.conrowbytes = quake_surface->pitch;
	vid.numpages = 1;

	if (vid_renderer.value == 0)
	{
		vid.maxwarpwidth = WARP_WIDTH;
		vid.maxwarpheight = WARP_HEIGHT;
	}
	else {
		vid.maxwarpwidth = targ_winternal;
		vid.maxwarpheight = targ_hinternal;
	}
	
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
	int scale;

	VID_ShutdownOpenGL();
	if (quake_surface) { SDL_FreeSurface(quake_surface); quake_surface = NULL; }
	if (quake_surface_world) { SDL_FreeSurface(quake_surface_world); quake_surface_world = NULL; }
	if (window) { SDL_DestroyWindow(window); window = NULL; }
	if (quake_surface32) { SDL_FreeSurface(quake_surface32); quake_surface32 = NULL; }

	Uint32 flags = SDL_WINDOW_SHOWN;
		if (gl_supported && vid_renderer.value != 0)
			flags |= SDL_WINDOW_OPENGL;

	if (vid_fullscreen_mode.value == 1)
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	else
		flags |= SDL_WINDOW_FULLSCREEN;

	if (gl_supported && vid_renderer.value != 0) {
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
	}

	targ_winternal = modelist[modenum].width;
	targ_hinternal = modelist[modenum].height;

	scale = (int)vid_render_scale.value;
	if (scale < 50) scale = 50;
	if (scale > 150) scale = 150;
	targ_winternal = (targ_winternal * scale) / 100;
	targ_hinternal = (targ_hinternal * scale) / 100;
	if (targ_winternal < 320) targ_winternal = 320;
	if (targ_hinternal < 200) targ_hinternal = 200;
	if (targ_winternal > MAXWIDTH) targ_winternal = MAXWIDTH;
	if (targ_hinternal > MAXHEIGHT) targ_hinternal = MAXHEIGHT;

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
	if (vid_refreshrate.value > 0)
		dm.refresh_rate = (int)vid_refreshrate.value;
	SDL_SetWindowDisplayMode(window, &dm);

	if (gl_supported && vid_renderer.value != 0) {
		gl_context = SDL_GL_CreateContext(window);
		if (!gl_context) {
			Sys_Error("SDL_GL_CreateContext failed: %s", SDL_GetError());
		}

		if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
			Sys_Error("Failed to initialize GLAD");
		}

		if (vid_vsync.value)
			SDL_GL_SetSwapInterval(1);
		else
			SDL_GL_SetSwapInterval(0);

		VID_InitOpenGL(targ_winternal, targ_hinternal);
	}

	quake_surface = SDL_CreateRGBSurface(0, targ_winternal, targ_hinternal, 8, 0, 0, 0, 0);
	if (!quake_surface) {
		Sys_Error("SDL_CreateRGBSurface failed: %s", SDL_GetError());
	}
	quake_surface_world = SDL_CreateRGBSurface(0, targ_winternal, targ_hinternal, 8, 0, 0, 0, 0);
	quake_surface32 = SDL_CreateRGBSurface(0, targ_winternal, targ_hinternal, 32, 0, 0, 0, 0);

	vid.buffer = vid.conbuffer = vid.direct = (byte*)quake_surface->pixels;
	vid.rowbytes = vid.conrowbytes = quake_surface->pitch;
	vid.numpages = 1;

	if (vid_renderer.value == 0)
	{
		vid.maxwarpwidth = WARP_WIDTH;
		vid.maxwarpheight = WARP_HEIGHT;
	}
	else {
		vid.maxwarpwidth = targ_winternal;
		vid.maxwarpheight = targ_hinternal;
	}

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
		sprintf(pinfo, "%s fullscreen", pv->modedesc);
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
	int use_fullscreen = (int)vid_fullscreen_mode.value;
	if (use_fullscreen == 0) {
		stat = VID_SetWindowedMode(modenum);
	}
	else {
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
	
	if (vid_renderer.value == 0 || !gl_supported)
	{
		if (r_dowarp)
			d_viewbuffer = r_warpbuffer;
		else
			d_viewbuffer = (byte*)quake_surface->pixels;
	}
	else
		d_viewbuffer = (byte*)quake_surface->pixels;

	if (vid_renderer.value == 0 || !gl_supported)
	{
		if (r_dowarp)
			screenwidth = WARP_WIDTH;
		else
			screenwidth = vid.rowbytes;
	}
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

	if (lockcount < 0){
		lockcount = 0;
		return;
	}
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

			d_8to24table[i] = colors[i].r | (colors[i].g << 8) |
				(colors[i].b << 16) | (0xFF << 24);
		}

		SDL_SetPaletteColors(quake_surface->format->palette, colors, 0, 256);
	}

	memcpy (vid_curpal, palette, sizeof(vid_curpal));

	if (gl_palette_texture) {
		unsigned char pal32[256 * 4];
		float gamma = v_gamma.value;

		if (gamma != cached_gamma) {
			cached_gamma = gamma;
			for (int i = 0; i < 256; i++) {
				gamma_lut[i] = (unsigned char)(pow(i / 255.0f, gamma) * 255.0f);
			}
		}

		for (int i = 0; i < 256; i++) {
			pal32[i * 4 + 0] = gamma_lut[palette[i * 3]];
			pal32[i * 4 + 1] = gamma_lut[palette[i * 3 + 1]];
			pal32[i * 4 + 2] = gamma_lut[palette[i * 3 + 2]];
			pal32[i * 4 + 3] = 255;
		}
		glBindTexture(GL_TEXTURE_2D, gl_palette_texture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, pal32);
	}
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
	Cvar_RegisterVariable (&vid_refreshrate);
	Cvar_RegisterVariable (&vid_vsync);
	Cvar_RegisterVariable (&vid_renderer);
	Cvar_RegisterVariable (&vid_render_scale);

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

	// auto check opengl 2.0 support
	if (VID_CheckGLSupport()) {
		gl_supported = true;
		Con_Printf("OpenGL 2.0+ supported - using OpenGL renderer.\n");
	}
	else {
		gl_supported = false;
		Con_Printf("OpenGL 2.0+ not supported by GPU - falling back to SDL2 software renderer.\n");
	}

	VID_InitModes();

	if (vid_renderer.value == 0) {
		vid.maxwarpwidth = WARP_WIDTH;
		vid.maxwarpheight = WARP_HEIGHT;
	}
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong(*((int*)vid.colormap + 2048));
	vid_testingmode = 0;

	if (COM_CheckParm("-startwindowed"))
	{
		startwindowed = 1;
		Cvar_SetValue("vid_fullscreen_mode", 0.0f);
	}

// find saved resolution
	int saved_w = (int)vid_config_x.value;
	int saved_h = (int)vid_config_y.value;
	int target_modenum = 0;                    // fallback = smallest mode
	qboolean found = false;

	if (saved_w >= 320 && saved_h >= 200) {
		for (int i = 0; i < nummodes; i++) {
			if (modelist[i].width == saved_w && modelist[i].height == saved_h) {
				target_modenum = i;
				found = true;
				break;
			}
		}
	}
	if (!found) {
		// fallback to largest mode and update the saved cvars
		int max_area = 0;
		for (int i = 0; i < nummodes; i++) {
			int area = modelist[i].width * modelist[i].height;
			if (area > max_area) {
				max_area = area;
				target_modenum = i;
			}
		}
		Cvar_SetValue("vid_config_x", (float)modelist[target_modenum].width);
		Cvar_SetValue("vid_config_y", (float)modelist[target_modenum].height);
	}

	// init vid_refreshrate only if it was never saved
	if ((int)vid_refreshrate.value <= 0) {
		int refresh = 60;
		if (window) {
			SDL_DisplayMode dm;
			if (SDL_GetWindowDisplayMode(window, &dm) == 0 && dm.refresh_rate > 0)
				refresh = dm.refresh_rate;
		}
		Cvar_SetValue("vid_refreshrate", (float)refresh);
	}

	vid_initialized = true;
	vid_default = target_modenum;

	VID_SetMode(target_modenum, palette);
	S_Init();

	vid_realmode = vid_modenum;
	vid_current_mode = vid_modenum;

	VID_SetPalette(palette);

	vid_menudrawfn = VID_MenuDraw;
	vid_menukeyfn = VID_MenuKey;

	// force save settings on first init
	Cvar_SetValue("_vid_default_mode_win", (float)vid_current_mode);
}


void	VID_Shutdown (void)
{
	if (vid_initialized)
	{
		VID_ShutdownOpenGL();
		if (quake_surface) SDL_FreeSurface(quake_surface);
		if (quake_surface_world) SDL_FreeSurface(quake_surface_world);
		// if (screen_surface) SDL_FreeSurface(screen_surface);
		if (window) SDL_DestroyWindow(window);

		SDL_QuitSubSystem(SDL_INIT_VIDEO);

		vid_testingmode = 0;
		vid_initialized = 0;
	}
}

/*
=================
VID_FindMode
=================
*/
static int VID_FindMode(int width, int height)
{
	int i;
	for (i = 0; i < nummodes; i++) {
		if (modelist[i].width == width && modelist[i].height == height)
			return i;
	}
	// fallback if exact match not found
	for (i = 0; i < nummodes; i++) {
		if (modelist[i].width >= width && modelist[i].height >= height)
			return i;
	}
	return 0;
}

void VID_CaptureWorld(void)
{
	if (gl_supported && vid_renderer.value != 0 && quake_surface && quake_surface_world)
	{
		memcpy(quake_surface_world->pixels, quake_surface->pixels, quake_surface->pitch * quake_surface->h);
		memset(quake_surface->pixels, 255, quake_surface->pitch * quake_surface->h);
	}
}

void	VID_Update(vrect_t* rects)
{
	if (!quake_surface || !window)
		return;

	if (startup_count < 60)
	{
		startup_count++;

		int saved_fs = (int)vid_fullscreen_mode.value;
		int saved_vsync = (int)vid_vsync.value;
		int saved_refresh = (int)vid_refreshrate.value;

		qboolean need_reapply = false;

		int saved_w = (int)vid_config_x.value;
		int saved_h = (int)vid_config_y.value;

		if (saved_w > 0 && saved_h > 0 &&
			(modelist[vid_modenum].width != saved_w || modelist[vid_modenum].height != saved_h))
			need_reapply = true;

		if (saved_fs != 0 && modestate == MS_WINDOWED)
			need_reapply = true;
		if (saved_fs == 0 && modestate != MS_WINDOWED)
			need_reapply = true;

		qboolean should_be_gl = gl_supported && ((int)vid_renderer.value == 1);
		qboolean is_currently_gl = (gl_context != NULL);
		if (should_be_gl != is_currently_gl)
			need_reapply = true;

		if (need_reapply)
		{
			force_mode_set = true;
			if (saved_w > 0 && saved_h > 0)
				vid_modenum = VID_FindMode(saved_w, saved_h);

			if (saved_fs == 0)
				VID_SetWindowedMode(vid_modenum);
			else
				VID_SetFullscreenMode(vid_modenum);

			force_mode_set = false;

			if (!VID_AllocBuffers(vid.width, vid.height))
				Sys_Error("Couldn't reallocate video buffers after config restore");
			D_InitCaches(vid_surfcache, vid_surfcachesize);
			vid.recalc_refdef = 1;

			VID_SetPalette(vid_curpal);
			startup_count = 100;
		}

		prev_width = modelist[vid_modenum].width;
		prev_height = modelist[vid_modenum].height;
		prev_refresh = (saved_refresh > 0) ? saved_refresh : 60;
		prev_vsync = saved_vsync;
		prev_fullscreen = saved_fs;
		prev_renderer = (int)vid_renderer.value;
	}

	if (vid_renderer.value == 0 || !gl_supported)
	{
		SDL_Surface* screen_surface = SDL_GetWindowSurface(window);
		if (screen_surface)
		{
			float gamma = v_gamma.value;
			if (gamma != cached_gamma)
			{
				cached_gamma = gamma;
				for (int i = 0; i < 256; i++)
					gamma_lut[i] = (unsigned char)(pow(i / 255.0f, gamma) * 255.0f);
			}

			SDL_Color colors[256];
			for (int i = 0; i < 256; i++)
			{
				colors[i].r = gamma_lut[vid_curpal[i * 3]];
				colors[i].g = gamma_lut[vid_curpal[i * 3 + 1]];
				colors[i].b = gamma_lut[vid_curpal[i * 3 + 2]];
			}
			SDL_SetPaletteColors(quake_surface->format->palette, colors, 0, 256);
			if (quake_surface32) {
				SDL_BlitSurface(quake_surface, NULL, quake_surface32, NULL);
				SDL_BlitScaled(quake_surface32, NULL, screen_surface, NULL);
			}
			else {
				SDL_BlitScaled(quake_surface, NULL, screen_surface, NULL);
			}
			SDL_UpdateWindowSurface(window);
		}
	}
	else
	{
		// OpenGL path
		if (!gl_context || !gl_texture)
			return;

		// CLEAR first - prevents stuck old frames
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		int targ_winternal = quake_surface->w;
		int targ_hinternal = quake_surface->h;

		int draw_w, draw_h;
		SDL_GL_GetDrawableSize(window, &draw_w, &draw_h);

		glBindTexture(GL_TEXTURE_2D, gl_texture);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, quake_surface->pitch);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, targ_winternal, targ_hinternal, GL_LUMINANCE, GL_UNSIGNED_BYTE, quake_surface->pixels);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

		if (quake_surface_world) {
			glBindTexture(GL_TEXTURE_2D, gl_texture_world);
			glPixelStorei(GL_UNPACK_ROW_LENGTH, quake_surface_world->pitch);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, targ_winternal, targ_hinternal, GL_LUMINANCE, GL_UNSIGNED_BYTE, quake_surface_world->pixels);
			glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		}

		if (draw_w != cached_draw_w || draw_h != cached_draw_h)
		{
			glViewport(0, 0, draw_w, draw_h);
			cached_draw_w = draw_w;
			cached_draw_h = draw_h;
		}

		glUseProgram(gl_shader_program);

		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, gl_palette_texture);
		if (gl_uniform_palette != -1) glUniform1i(gl_uniform_palette, 2);

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, gl_texture);
		if (gl_uniform_texture_ui != -1) glUniform1i(gl_uniform_texture_ui, 1);

		glActiveTexture(GL_TEXTURE0);
		if (quake_surface_world) glBindTexture(GL_TEXTURE_2D, gl_texture_world);
		else glBindTexture(GL_TEXTURE_2D, gl_texture);
		if (gl_uniform_texture_world != -1) glUniform1i(gl_uniform_texture_world, 0);

		glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

		if (gl_uniform_time != -1)
			glUniform1f(gl_uniform_time, cl.time);
		if (gl_uniform_dowarp != -1)
			glUniform1f(gl_uniform_dowarp, r_dowarp ? 1.0f : 0.0f);
		if (gl_uniform_width != -1)
			glUniform1f(gl_uniform_width, (float)targ_winternal);
		if (gl_uniform_height != -1)
			glUniform1f(gl_uniform_height, (float)targ_hinternal);

		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		glDisableVertexAttribArray(0);
		glDisableVertexAttribArray(1);

		SDL_GL_SwapWindow(window);
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

	if (key_dest != key_game || cl.paused)
	{
		if (mouseactive)
			IN_DeactivateMouse();
		IN_ShowMouse();
	}
	else
	{
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
			if (vid_renderer.value == 0) {
				//SDL_BlitSurface(quake_surface, NULL, screen_surface, NULL);
				SDL_UpdateWindowSurface(window);
			}
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

extern void M_DrawSlider(int x, int y, float range);

/*
=================
VID_ApplyChanges
=================
*/
void VID_ApplyChanges(qboolean permanent)
{
	int w, h, refresh, fs_mode, vsync;
	vmode_t* pmode;
	int target_mode_index;
	float old_fs;
	float old_vsync;
	float old_refresh;
	float old_renderer;
	int renderer;

	if (vid_test_active && !permanent) {
		// revert test
		w = prev_width;
		h = prev_height;
		refresh = prev_refresh;
		fs_mode = prev_fullscreen;
		vsync = prev_vsync;

		// restore cvar values temporarily to ensure they are used if relevant
		vid_menu_fullscreen = fs_mode;
		vid_menu_vsync = vsync;
		renderer = prev_renderer;
		vid_menu_renderer = renderer;

		if (vid_menu_renderer == 1 && !gl_supported)
			vid_menu_renderer = 0;

		vid_renderer.value = (float)renderer;

		vid_fullscreen_mode.value = (float)fs_mode;
		vid_vsync.value = (float)vsync;
		vid_refreshrate.value = (float)refresh;
		vid_render_scale.value = (float)prev_render_scale;
		vid_menu_render_scale = prev_render_scale;

		target_mode_index = VID_FindMode(w, h);
		if (target_mode_index >= 0) {
			if (fs_mode == 0)
				VID_SetWindowedMode(target_mode_index);
			else
				VID_SetFullscreenMode(target_mode_index);
		}

		vid_current_mode = target_mode_index;
		VID_CollectRefreshRates(w, h);

		vid_refresh_index = 0;
		for (int i = 0; i < vid_num_refresh; i++) {
			if (vid_refresh_rates[i] == refresh) {
				vid_refresh_index = i;
				break;
			}
		}

		// reallocate buffers for old mode
		if (!VID_AllocBuffers(vid.width, vid.height)) {
			Sys_Error("VID_ApplyChanges: Couldn't reallocate video buffers");
		}
		D_InitCaches(vid_surfcache, vid_surfcachesize);
		ClearAllStates();
		vid.recalc_refdef = 1;

		vid_test_active = false;
		Con_Printf("Video changes reverted.\n");
		return;
	}

	// apply selected
	pmode = &modelist[vid_current_mode];
	w = pmode->width;
	h = pmode->height;
	refresh = (vid_num_refresh > 0) ? vid_refresh_rates[vid_refresh_index] : 60;
	fs_mode = vid_menu_fullscreen;
	vsync = vid_menu_vsync;

	// remember old cvar to restore after test
	old_fs = vid_fullscreen_mode.value;
	old_vsync = vid_vsync.value;
	old_refresh = vid_refreshrate.value;
	renderer = vid_menu_renderer;
	old_renderer = vid_renderer.value;
	vid_renderer.value = (float)renderer;
	vid_fullscreen_mode.value = (float)fs_mode;
	vid_vsync.value = (float)vsync;
	vid_refreshrate.value = (float)refresh;
	vid_render_scale.value = (float)vid_menu_render_scale;

	// apply
	if (fs_mode == 0)
		VID_SetWindowedMode(vid_current_mode);
	else
		VID_SetFullscreenMode(vid_current_mode);

	if (!permanent) {
		vid_renderer.value = old_renderer;
		vid_fullscreen_mode.value = old_fs;
		vid_vsync.value = old_vsync;
		vid_refreshrate.value = old_refresh;
	}

	// reallocate buffers for new mode
	if (!VID_AllocBuffers(vid.width, vid.height)) {
		Sys_Error("VID_ApplyChanges: Couldn't allocate video buffers");
	}
	D_InitCaches(vid_surfcache, vid_surfcachesize);
	ClearAllStates();
	vid.recalc_refdef = 1;

	if (permanent) {
		Cvar_SetValue("vid_renderer", (float)renderer);
		Cvar_SetValue("_vid_default_mode_win", (float)vid_current_mode);
		
		Cvar_SetValue("vid_config_x", (float)w);
		Cvar_SetValue("vid_config_y", (float)h);

		Cvar_SetValue("vid_refreshrate", (float)refresh);
		Cvar_SetValue("vid_vsync", (float)vsync);
		Cvar_SetValue("vid_fullscreen_mode", (float)fs_mode);
		Cvar_SetValue("vid_render_scale", (float)vid_menu_render_scale);

		Con_Printf("Video changes applied permanently.\n");

		// save into previous
		prev_renderer = renderer;
		prev_width = w;
		prev_height = h;
		prev_refresh = refresh;
		prev_fullscreen = fs_mode;
		prev_vsync = vsync;
		prev_render_scale = vid_menu_render_scale;
		
		vid_test_active = false;
	}
	else {
		vid_test_active = true;
		vid_test_start = realtime;
		Con_Printf("Testing video changes for %.0f seconds...\n", vid_test_duration);
	}
}

/*
================
VID_MenuDraw
================
*/
void VID_MenuDraw(void)
{
	qpic_t* p;
	int			lnummodes = VID_NumModes();
	char		temp[100];
	int temp_fs;
	vmode_t* pv;
	// draw options
	int y = 32;
	const int x_label = 16;
	const int x_value = 220;
	const int x_cursor = 200;

	p = Draw_CachePic("gfx/vidmodes.lmp");
	M_DrawPic((320 - p->width) / 2, 4, p);

	// store originals on first entry
	static qboolean first_entry = true;
	if (first_entry) {
		prev_renderer = (int)vid_renderer.value;

		if (prev_renderer != 0 && prev_renderer != 1)
		{
			if (gl_supported)
				prev_renderer = 1;
			else
				prev_renderer = 0;
		}

		prev_width = modelist[vid_modenum].width;
		prev_height = modelist[vid_modenum].height;
		prev_refresh = (vid_refreshrate.value > 0) ? (int)vid_refreshrate.value : 60;
		prev_vsync = (int)vid_vsync.value;
		if (prev_vsync != 0 && prev_vsync != 1) prev_vsync = 0;

		temp_fs = (int)vid_fullscreen_mode.value;
		if (temp_fs < 0 || temp_fs > 2) temp_fs = 2;
		prev_fullscreen = temp_fs;

		vid_menu_fullscreen = prev_fullscreen;
		vid_menu_vsync = prev_vsync;
		vid_menu_renderer = prev_renderer;
		vid_menu_render_scale = (int)vid_render_scale.value;
		if (vid_menu_render_scale < 50) vid_menu_render_scale = 50;
		if (vid_menu_render_scale > 150) vid_menu_render_scale = 150;
		prev_render_scale = vid_menu_render_scale;

		first_entry = false;

		// find current mode index
		for (vid_current_mode = 0; vid_current_mode < lnummodes; vid_current_mode++) {
			pv = VID_GetModePtr(vid_current_mode);
			if (pv->width == prev_width && pv->height == prev_height) break;
		}
		if (vid_current_mode >= lnummodes) vid_current_mode = 0;
		
		VID_CollectRefreshRates(modelist[vid_current_mode].width, modelist[vid_current_mode].height);
	}

	if (vid_test_active && (realtime - vid_test_start > vid_test_duration)) {
		VID_ApplyChanges(false); // revert
	}

	M_Print(x_label, y, "          Video Mode");
	pv = VID_GetModePtr(vid_current_mode);
	sprintf(temp, "%dx%d", pv->width, pv->height);
	M_Print(x_value, y, temp);
	if (vid_menuline == 0) M_DrawCharacter(x_cursor, y, 12 + ((int)(realtime * 4) & 1));
	y += 8;

	M_Print(x_label, y, "        Refresh Rate");
	int cur_refresh = (vid_num_refresh > 0) ? vid_refresh_rates[vid_refresh_index] : 60;
	sprintf(temp, "%d Hz", cur_refresh);
	M_Print(x_value, y, temp);
	if (vid_menuline == 1) M_DrawCharacter(x_cursor, y, 12 + ((int)(realtime * 4) & 1));
	y += 8;

	M_Print(x_label, y, "       Vertical Sync");
	sprintf(temp, "%s", vid_menu_vsync ? "On" : "Off");
	M_Print(x_value, y, temp);
	if (vid_menuline == 2) M_DrawCharacter(x_cursor, y, 12 + ((int)(realtime * 4) & 1));
	y += 8;

	M_Print(x_label, y,"          Fullscreen");
	const char* fs_str[] = { "Off", "Borderless", "On" };
	int fs_val = vid_menu_fullscreen;
	if (fs_val < 0) fs_val = 0;
	if (fs_val > 2) fs_val = 2;
	sprintf(temp, "%s", fs_str[fs_val]);
	M_Print(x_value, y, temp);
	if (vid_menuline == 3) M_DrawCharacter(x_cursor, y, 12 + ((int)(realtime * 4) & 1));
	y += 8;

	M_Print(x_label, y, "            Renderer");
	sprintf(temp, "%s", vid_menu_renderer ? "OpenGL" : "SDL2");
	M_Print(x_value, y, temp);
	if (vid_menuline == 4) M_DrawCharacter(x_cursor, y, 12 + ((int)(realtime * 4) & 1));
	y += 8;

	M_Print(x_label, y, "        Render Scale");
	float r = (vid_menu_render_scale - 50) / 100.0;
	M_DrawSlider(x_value, y, r);
	sprintf(temp, "%d%%", vid_menu_render_scale);
	M_Print(x_value + 12 * 8, y, temp);
	if (vid_menuline == 5) M_DrawCharacter(x_cursor, y, 12 + ((int)(realtime * 4) & 1));
	y += 16;

	M_Print(x_label, y, "         Test Changes");
	if (vid_menuline == 6) M_DrawCharacter(x_cursor, y, 12 + ((int)(realtime * 4) & 1));
	y += 8;

	M_Print(x_label, y, "        Apply Changes");
	if (vid_menuline == 7) M_DrawCharacter(x_cursor, y, 12 + ((int)(realtime * 4) & 1));
	y += 16;

	if (vid_test_active) {
		int remain = (int)(vid_test_duration - (realtime - vid_test_start));
		sprintf(temp, "Test active: %d sec remaining", remain);
		M_Print(x_label, y, temp);
	}
	
	// Print simple instructions
	M_Print(16, 200, "Arrow keys to change values");
	M_Print(16, 208, "Enter to select");
	M_Print(16, 216, "Esc to exit");
}

/*
================
VID_MenuKey
================
*/
void VID_MenuKey(int key)
{
	switch (key)
	{
	case K_ESCAPE:
		S_LocalSound("misc/menu1.wav");
		M_Menu_Options_f();
		break;
	case K_LEFTARROW:
		S_LocalSound("misc/menu1.wav");
		switch (vid_menuline) {
		case 0: // video mode
			if (--vid_current_mode < 0) vid_current_mode = VID_NumModes() - 1;
			VID_CollectRefreshRates(modelist[vid_current_mode].width, modelist[vid_current_mode].height);
			vid_refresh_index = 0;
			break;
		case 1: // refresh rate
			if (vid_num_refresh > 0 && --vid_refresh_index < 0) vid_refresh_index = vid_num_refresh - 1;
			break;
		case 2: // vsync
			vid_menu_vsync = !vid_menu_vsync;
			break;
		case 3: // fullscreen
			if (--vid_menu_fullscreen < 0) vid_menu_fullscreen = 2;
			break;
		case 4: // renderer
				vid_menu_renderer = !vid_menu_renderer;
			break;
		case 5:
			vid_menu_render_scale -= 5;
			if (vid_menu_render_scale < 50) vid_menu_render_scale = 50;
			break;
		}
		break;
	case K_RIGHTARROW:
		S_LocalSound("misc/menu1.wav");
		switch (vid_menuline) {
		case 0: // video mode
			if (++vid_current_mode >= VID_NumModes()) vid_current_mode = 0;
			VID_CollectRefreshRates(modelist[vid_current_mode].width, modelist[vid_current_mode].height);
			vid_refresh_index = 0;
			break;
		case 1: // refresh rate
			if (vid_num_refresh > 0 && ++vid_refresh_index >= vid_num_refresh) vid_refresh_index = 0;
			break;
		case 2: // vsync
			vid_menu_vsync = !vid_menu_vsync;
			break;
		case 3: // fullscreen
			if (++vid_menu_fullscreen > 2) vid_menu_fullscreen = 0;
			break;
		case 4: // renderer
				vid_menu_renderer = !vid_menu_renderer;
			break;
		case 5:
			vid_menu_render_scale += 5;
			if (vid_menu_render_scale > 150) vid_menu_render_scale = 150;
			break;
		}
		break;
	case K_UPARROW:
		S_LocalSound("misc/menu1.wav");
		vid_menuline--;
		if (vid_menuline < 0) vid_menuline = 7;
		break;
	case K_DOWNARROW:
		S_LocalSound("misc/menu1.wav");
		vid_menuline++;
		if (vid_menuline > 7) vid_menuline = 0;
		break;
	case K_ENTER:
	{
		S_LocalSound("misc/menu2.wav");
		if (vid_menuline == 6) {
			VID_ApplyChanges(false); // test changes
		}
		else if (vid_menuline == 7) {
			VID_ApplyChanges(true); // apply changes
		}
		break;
	}
	default:
		break;
	}
}
