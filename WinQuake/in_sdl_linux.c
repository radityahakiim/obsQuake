// ====================================
// in_sdl_linux.c: linux input handling with SDL2
// ====================================
#include "quakedef.h"
#include <SDL.h>
#include "linuxcrossplat.h"

qboolean mouseactive = false;
int mx_accum = 0;
int my_accum = 0;
qboolean ActiveApp = true, Minimized = false;

extern SDL_Window* window;

void IN_Init(void){}
void IN_Shutdown (void){}
void IN_Commands (void){}

void IN_Move (usercmd_t *cmd)
{
	float mouse_x, mouse_y;
	if (!mouseactive)
		return;

	mouse_x = mx_accum * sensitivity.value;
	mouse_y = my_accum * sensitivity.value;

	mx_accum = 0;
	my_accum = 0;

	if (window)
	{
		int w, h;
		SDL_GetWindowSize(window, &w, &h);
		SDL_WarpMouseInWindow(window, w / 2, h / 2);
	}

	if (in_strafe.state & 1)
		cmd->sidemove += m_side.value * mouse_x;
	else
		cl.viewangles[YAW] -= m_yaw.value * mouse_x;

	// stop centering pitch if mlook active
	if (in_mlook.state & 1)
		V_StopPitchDrift();

	if ((in_mlook.state & 1) && !(in_strafe.state & 1))
	{
		cl.viewangles[PITCH] += m_pitch.value * mouse_y;
		if (cl.viewangles[PITCH] > 90)
		    cl.viewangles[PITCH] = 90;
		if (cl.viewangles[PITCH] < -90)
		    cl.viewangles[PITCH] = -90;
	}
	else
	{
		if ((in_strafe.state & 1) && noclip_anglehack)
			cmd->upmove -= m_forward.value * mouse_y;
		else
			cmd->forwardmove -= m_forward.value * mouse_y;
	}
}

void IN_ShowMouse(void)
{
	SDL_ShowCursor(SDL_ENABLE);
}

void IN_DeactivateMouse(void)
{
	SDL_SetRelativeMouseMode(SDL_FALSE);
	if (window)
		SDL_SetWindowGrab(window, SDL_FALSE);
	mouseactive = false;
}

void IN_HideMouse(void)
{
	SDL_ShowCursor(SDL_DISABLE);
}

void IN_ActivateMouse(void)
{
	SDL_SetRelativeMouseMode(SDL_TRUE);
	if (window)
		SDL_SetWindowGrab(window, SDL_TRUE);
	mouseactive = true;
}

void IN_UpdateClipCursor (void) {}
void IN_ClearStates(void) { mx_accum = 0; my_accum = 0;}
