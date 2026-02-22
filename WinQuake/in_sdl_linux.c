// ====================================
// in_sdl_linux.c: linux input handling with SDL2
// ====================================
#include "quakedef.h"
#include <SDL.h>

qboolean mouseactive = false;
int mx_accum = 0;
int my_accum = 0;
qboolean ActiveApp = true, Minimized = false;

void IN_Init(void){}
void IN_Shutdown (void){}
void IN_Commands (void){}

void IN_Move (usercmd_t *cmd)
{
	if (!mouseactive)
		return;

	cmd->forwardmove -= m_forward.value * my_accum;
	cmd->sidemove += m_side.value * mx_accum;

	if ((in_mlook.state & 1) && !(in_strafe.state & 1))
	{
		cl.viewangles[PITCH] += m_pitch.value * my_accum;
		if (cl.viewangles[PITCH] > 90)
		    cl.viewangles[PITCH] = 90;
		if (cl.viewangles[PITCH] < -90)
		    cl.viewangles[PITCH] = -90;
	}
	else
	{
		if ((in_strafe.state & 1) && noclip_anglehack)
			cmd->upmove -= m_forward.value * my_accum;
		else
			cmd->forwardmove -= m_forward.value * my_accum;
	}
	if (!(in_strafe.state & 1))
		cl.viewangles[YAW] -= m_yaw.value * mx_accum;

	mx_accum = 0;
	my_accum = 0;
}

void IN_ShowMouse(void)
{
	SDL_ShowCursor(SDL_ENABLE);
}

void IN_DeactivateMouse(void)
{
	SDL_SetRelativeMouseMode(SDL_FALSE);
	mouseactive = false;
}

void IN_HideMouse(void)
{
	SDL_ShowCursor(SDL_DISABLE);
}

void IN_ActivateMouse(void)
{
	SDL_SetRelativeMouseMode(SDL_TRUE);
	mouseactive = true;
}

void IN_UpdateClipCursor (void) {}
void IN_ClearStates(void) { mx_accum = 0; my_accum = 0;}
