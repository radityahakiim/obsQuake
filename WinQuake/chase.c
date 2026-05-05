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
cvar_t	chase_up = {"chase_up", "32"};
cvar_t	chase_right = {"chase_right", "0"};
cvar_t	chase_active = {"chase_active", "0"};
cvar_t	chase_orbit = {"chase_orbit", "1"};

vec3_t	chase_pos;
vec3_t	chase_angles;

vec3_t	chase_dest;
vec3_t	chase_dest_angles;

vec3_t	smoothed_angles;

float	chase_lasttime;
float	chase_lastactive;

float		player_alpha; // from render main (gl_rmain.c for glquake)

float	smoothed_player_z = 0.0f;
// qboolean chase_forcefirstperson = false;
float	chase_smoothed_dist;

qboolean SV_RecursiveHullCheck(hull_t* hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t* trace);


void Chase_Init (void)
{
	Cvar_RegisterVariable (&chase_back);
	Cvar_RegisterVariable (&chase_up);
	Cvar_RegisterVariable (&chase_right);
	Cvar_RegisterVariable (&chase_active);
	Cvar_RegisterVariable (&chase_orbit);
	chase_lastactive = -1;
	chase_smoothed_dist = 1024.0f;
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
	trace_t		trace;
	int			i;
	entity_t*	ent;
	model_t*	model;
	trace_t		ent_trace;
	vec3_t		start_l, end_l;
	hull_t*		hull;

	memset (&trace, 0, sizeof(trace));
	trace.fraction = 1.0f;
	VectorCopy(end, trace.endpos);
	SV_RecursiveHullCheck (cl.worldmodel->hulls, 0, 0, 1, start, end, &trace);

	// trace against all client-side brush entities (doors, buttons, platforms, etc.)
	for (i = 1; i < cl.num_entities; i++)
	{
		ent = &cl_entities[i];
		model = ent->model;

		if (!model || model->type != mod_brush)
			continue;

		// skip the world model itself (submodel 0)
		if (model == cl.worldmodel)
			continue;

		hull = &model->hulls[0];  // point-sized hull for ray trace

		// transform trace into entity's local space
		VectorSubtract (start, ent->origin, start_l);
		VectorSubtract (end, ent->origin, end_l);

		memset (&ent_trace, 0, sizeof(ent_trace));
		ent_trace.fraction = 1.0f;
		VectorCopy (end, ent_trace.endpos);

		SV_RecursiveHullCheck (hull, hull->firstclipnode, 0, 1, start_l, end_l, &ent_trace);

		// fix up endpos back to world space
		if (ent_trace.fraction < 1.0f)
		{
			VectorAdd (ent_trace.endpos, ent->origin, ent_trace.endpos);
		}

		// keep the closer hit
		if (ent_trace.fraction < trace.fraction)
		{
			trace = ent_trace;
		}
	}

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
	vec3_t desired, old;

	trace_t final_trace, trace;
	float angle_lerp, pitch_delta, yaw_delta;


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

	if (chase_orbit.value)
	{
		// in orbital mode, we use the Z component from the view vector,
		// but we still add chase_up as a vertical bias (camera height).
		chase_dest[2] += chase_up.value;
	}
	else
	{
		// fixed vertical offset
		chase_dest[2] = player_org[2] + chase_up.value;
	}

	// add collision detection: trace from player to desired camera pos and clip if necessary
	VectorCopy(chase_dest, desired);

	trace = TraceLine(player_org, desired);
	if (trace.fraction < 1.0f) {
		vec3_t dir;
		VectorSubtract(desired, player_org, dir);
		VectorNormalize(dir);

		// pull camera in front of the hit surface
		VectorMA(trace.endpos, -6.0f, dir, chase_dest);
	}
	else {
		VectorCopy(desired, chase_dest);
	}
	// force first person if camera view is blocked or too close
	/*
	vec3_t cam_delta;
	VectorSubtract(chase_dest, player_org, cam_delta);
	float cam_dist = VectorLength(cam_delta);

	trace_t los = TraceLine(chase_dest, player_org);

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
	*/

	// check if we hit something
	// if so, back off slightly to avoid clipping into the wall
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
	// use host_frametime for framerate-independent smoothing
	float speed = 15.0f;
	float partial = 1.0f - expf(-speed * host_frametime);

	// if it's been too long, just snap
	if (cl.time - chase_lasttime > 0.2f)
		partial = 1.0f;
	chase_lasttime = cl.time;

	VectorCopy(chase_pos, old);
	for (i = 0; i < 3; i++)
		chase_pos[i] = old[i] + partial * (chase_dest[i] - old[i]);

	// launder death/jump height using smoothed z
	if (chase_orbit.value)
	{
		// relaxed constraints for orbital movement
		if (chase_pos[2] < smoothed_player_z - 64)
			chase_pos[2] = smoothed_player_z - 64;
		if (chase_pos[2] > smoothed_player_z + 160)
			chase_pos[2] = smoothed_player_z + 160;
	}
	else
	{
		if (chase_pos[2] < smoothed_player_z + 8)
			chase_pos[2] = smoothed_player_z + 8;
		if (chase_pos[2] > smoothed_player_z + 48)
			chase_pos[2] = smoothed_player_z + 48;
	}

	// final collision check for interpolated and laundered position
	final_trace = TraceLine(player_org, chase_pos);
	if (final_trace.fraction < 1.0f) {
		vec3_t final_dir;

		VectorCopy(final_trace.endpos, chase_pos);

		// back off slightly from the wall
		VectorSubtract(chase_pos, player_org, final_dir);
		float final_dist = VectorLength(final_dir);
		if (final_dist > 2.0f) {
			VectorNormalize(final_dir);
			VectorMA(chase_pos, -2.0f, final_dir, chase_pos);
		} else {
			VectorCopy(player_org, chase_pos);
		}
	}

	// set view origin to smoothed position
	VectorCopy(chase_pos, r_refdef.vieworg);

	// calculate desired angles
	vec3_t desired_angles;
	VectorCopy(cl.viewangles, desired_angles);

	if (chase_orbit.value)
	{
		vec3_t impact_point, aim_dir;
		trace_t aim_trace;

		// find where the player is actually looking
		VectorMA(player_org, 8192, forward, impact_point);
		aim_trace = TraceLine(player_org, impact_point);
		
		float impact_dist = aim_trace.fraction * 8192.0f;

		// heavily smooth the impact distance to keep the camera steady.
		// this prevents the camera from jumping when aiming past edges.
		float dist_lerp = 1.0f - expf(-3.0f * host_frametime);
		chase_smoothed_dist += (impact_dist - chase_smoothed_dist) * dist_lerp;

		// use the smoothed distance to project a steady focus point
		VectorMA(player_org, chase_smoothed_dist, forward, impact_point);

		// calculate angles from camera to that impact point
		VectorSubtract(impact_point, r_refdef.vieworg, aim_dir);
		float cam_dist = VectorLength(aim_dir);

		if (cam_dist > 1.0f)
		{
			float horiz = sqrtf(aim_dir[0] * aim_dir[0] + aim_dir[1] * aim_dir[1]);
			if (horiz < 0.001f) horiz = 0.001f;

			float corr_yaw = atan2f(aim_dir[1], aim_dir[0]) * 180.0f / M_PI;
			float corr_pitch = -atan2f(aim_dir[2], horiz) * 180.0f / M_PI;

			// avoid wobble snapping near walls.
			float blend = 1.0f;
			if (chase_smoothed_dist < 256.0f)
			{
				blend = (chase_smoothed_dist - 64.0f) / 192.0f;
				if (blend < 0) blend = 0;
			}

			desired_angles[YAW] = cl.viewangles[YAW] + AngleNormalize(corr_yaw - cl.viewangles[YAW]) * blend;
			desired_angles[PITCH] = cl.viewangles[PITCH] + AngleNormalize(corr_pitch - cl.viewangles[PITCH]) * blend;
		}
	}

	// framerate independent angle smoothing
	angle_lerp = 1.0f - expf(-20.0f * host_frametime);
	yaw_delta = AngleNormalize(desired_angles[YAW] - smoothed_angles[YAW]);
	pitch_delta = AngleNormalize(desired_angles[PITCH] - smoothed_angles[PITCH]);

	// deadzone to stop micro jitter
	if (fabs(yaw_delta) < 0.05f) yaw_delta = 0;
	if (fabs(pitch_delta) < 0.05f) pitch_delta = 0;

	smoothed_angles[YAW] += yaw_delta * angle_lerp;
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


