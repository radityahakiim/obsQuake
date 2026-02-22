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
// chase.c -- chase camera code

#include "quakedef.h"

cvar_t	chase_back = {"chase_back", "100"};
cvar_t	chase_up = {"chase_up", "16"};
cvar_t	chase_right = {"chase_right", "0"};
cvar_t	chase_active = {"chase_active", "0"};

vec3_t	chase_pos;
vec3_t	chase_angles;

vec3_t	chase_dest;
vec3_t	chase_dest_angles;

vec3_t	smoothed_angles;

float	chase_lasttime;
float	chase_lastactive;

float		player_alpha; // from render main (gl_rmain.c for glquake)

float	smoothed_player_z;
qboolean chase_forcefirstperson = false;

qboolean SV_RecursiveHullCheck(hull_t* hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t* trace);


void Chase_Init (void)
{
	Cvar_RegisterVariable (&chase_back);
	Cvar_RegisterVariable (&chase_up);
	Cvar_RegisterVariable (&chase_right);
	Cvar_RegisterVariable (&chase_active);
	chase_lastactive = -1;
	smoothed_player_z = 0.0f;
}

void Chase_Reset (void)
{
	// for respawning and teleporting
//	start position 12 units behind head
	VectorCopy(r_refdef.vieworg, chase_pos);
	chase_lasttime = cl.time;
	smoothed_player_z = r_refdef.vieworg[2];
}

trace_t TraceLine (vec3_t start, vec3_t end)
{
	trace_t	trace;

	memset (&trace, 0, sizeof(trace));
	trace.fraction = 1.0f;
	VectorCopy(end, trace.endpos);
	SV_RecursiveHullCheck (cl.worldmodel->hulls + 1, 0, 0, 1, start, end, &trace);

	return trace;
}

// normalize angle in range from -180 to 180
float AngleNormalize(float angle)
{
	angle = fmodf(angle, 360.0f);
	if (angle > 180.0f)
		angle -= 360.0f;
	if (angle < -180.0f)
		angle += 360.0f;
	return angle;
}

void Chase_Update(void)
{
	int		i;
	float	dist;
	vec3_t	forward, up, right;
	vec3_t	dest, stop;
	vec3_t player_org;
	entity_t* ent;
	vec3_t virtual_player_org;


	if (!chase_active.value)
	{
		chase_lastactive = 0;
		return;
	}

	if (chase_lastactive <= 0)
	{
		Chase_Reset();
		chase_lastactive = 1;
	}

	// get player eye position
	ent = &cl_entities[cl.viewentity];
	VectorCopy(ent->origin, player_org);
	player_org[2] += 22; // standard viewheight

	// smooth the player z for camera calculation to reduce jumps on stairs
	if (smoothed_player_z == 0.0f)
		smoothed_player_z = player_org[2];
	smoothed_player_z += 0.5f * (player_org[2] - smoothed_player_z);

	VectorCopy(ent->origin, virtual_player_org);
	virtual_player_org[2] = smoothed_player_z;

	AngleVectors(cl.viewangles, forward, right, up);

	// calc exact destination
	for (i = 0; i < 3; i++)
		chase_dest[i] = player_org[i]
		- forward[i] * chase_back.value
		- right[i] * chase_right.value;
	chase_dest[2] = player_org[2] + chase_up.value;

	// add collision detection: trace from player to desired camera pos and clip if necessary
	vec3_t desired;
	VectorCopy(chase_dest, desired);

	trace_t trace = TraceLine(player_org, desired);
	if (trace.fraction < 1.0f) {
		vec3_t dir;
		VectorSubtract(desired, player_org, dir);
		VectorNormalize(dir);

		// pull camera in front of the hit surface
		VectorMA(trace.endpos, 6.0f, dir, chase_dest);
	}
	else {
		VectorCopy(desired, chase_dest);
	}
	vec3_t cam_delta;
	VectorSubtract(chase_dest, player_org, cam_delta);
	float cam_dist = VectorLength(cam_delta);

	trace_t los = TraceLine(chase_dest, player_org);

	// force first person if camera view is blocked or too close
	if ((los.fraction < 1.0f || cam_dist < 8) && player_alpha < 0.05)
	{
		chase_forcefirstperson = true;
	}
	else if (cam_dist > 16.0f || player_alpha > 0.1)
	{
		chase_forcefirstperson = false;
	}

	if (chase_forcefirstperson)
	{
		// first person camera
		VectorCopy(player_org, r_refdef.vieworg);
		VectorCopy(cl.viewangles, r_refdef.viewangles);
		return;
	}

	// check if we hit something: if so, back off slightly to avoid clipping into the wall
	if (trace.fraction < 1.0f) {
		vec3_t delta;
		VectorSubtract(chase_dest, player_org, delta);
		float achieved_dist = VectorLength(delta);

		float backoff = 8.0f;
		backoff = fmin(backoff, achieved_dist - 1.0f);
		if (backoff > 0.0f)
		{
			VectorNormalize(delta);
			VectorMA(chase_dest, -backoff, delta, chase_dest);
		}

		// dynamically adjust height if clipped to see over the player better
		float clip_ratio = 1.0 - trace.fraction;
		float scale = achieved_dist / chase_back.value;
		scale = fmax(0.2f, scale);
		chase_dest[2] += chase_up.value * clip_ratio * scale;
	}

	// smooth interpolation towards chase_dest
	float partial = 0.8f;
	if (cl.time - chase_lasttime > 0.2f)
		partial = 0.95f;
	chase_lasttime = cl.time;

	vec3_t old;
	VectorCopy(chase_pos, old);
	for (i = 0; i < 3; i++)
		chase_pos[i] = old[i] + partial * (chase_dest[i] - old[i]);

	// launder death/jump height using smoothed z
	if (chase_pos[2] < smoothed_player_z + 8)
		chase_pos[2] = smoothed_player_z + 8;
	if (chase_pos[2] > smoothed_player_z + 48)
		chase_pos[2] = smoothed_player_z + 48;

	// set view origin to smoothed position
	VectorCopy(chase_pos, r_refdef.vieworg);

	// use player view angles directly
	// vec3_t desired_angles;
	// VectorCopy(cl.viewangles, desired_angles);

	// find the spot the player is looking at
	/*
	VectorMA (virtual_player_org, 4096, forward, dest);
	trace_t aim_trace = TraceLine(virtual_player_org, dest);
	VectorCopy(aim_trace.endpos, stop);

	// calculate pitch to look at the same spot from camera
	vec3_t lookdir;
	VectorSubtract (stop, r_refdef.vieworg, lookdir);
	dist = VectorLength (lookdir);
	if (dist < 1)
		dist = 1;

	float horiz = sqrt(lookdir[0] * lookdir[0] + lookdir[1] * lookdir[1]);
	if (horiz < 0.0001f)
		horiz = 0.0001f;

	r_refdef.viewangles[YAW]   = atan2(lookdir[1], lookdir[0]) * 180 / M_PI;
	r_refdef.viewangles[PITCH] = -atan(lookdir[2] / horiz) * 180 / M_PI;
	r_refdef.viewangles[ROLL]  = 0;
	*/

	float angle_lerp = 0.15f;
	float yaw_delta = AngleNormalize(r_refdef.viewangles[YAW] - smoothed_angles[YAW]);
	float pitch_delta = AngleNormalize(r_refdef.viewangles[PITCH] - smoothed_angles[PITCH]);

	// deadzone to stop micro jitter
	if (fabs(yaw_delta) < 0.2f) yaw_delta = 0;
	if (fabs(pitch_delta) < 0.2f) pitch_delta = 0;

	smoothed_angles[YAW]   += yaw_delta * angle_lerp;
	smoothed_angles[PITCH] += pitch_delta * angle_lerp;
	smoothed_angles[ROLL] = 0;

	// clamp pitch
	if (smoothed_angles[PITCH] < -89)
		smoothed_angles[PITCH] = -89;
	else if (smoothed_angles[PITCH] > 89)
		smoothed_angles[PITCH] = 89;

	// final camera angle
	VectorCopy(smoothed_angles, r_refdef.viewangles);

}


