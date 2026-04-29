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
// d_modech.c: called when mode has just changed

#include "quakedef.h"
#include "d_local.h"

int	d_vrectx, d_vrecty, d_vrectright_particle, d_vrectbottom_particle;

int	d_y_aspect_shift, d_pix_min, d_pix_max, d_pix_shift;

int		d_scantable[MAXHEIGHT];
short	*zspantable[MAXHEIGHT]; 
extern	cvar_t	vid_renderer;

/*
================
D_ViewChanged
================
*/
void D_ViewChanged (void)
{
	int rowbytes;
	int d_pix_res;

	if (vid_renderer.value == 0)
	{
		if (r_dowarp)
			rowbytes = WARP_WIDTH;
		else
			rowbytes = vid.rowbytes;
	}
	else
		rowbytes = vid.rowbytes;

	scale_for_mip = xscale;
	if (yscale > xscale)
		scale_for_mip = yscale;

	d_zrowbytes = vid.width * 2;
	d_zwidth = vid.width;

	// fix particles (blood, debris) too huge on high resolution
	if (vid.width >= 1280) {
		d_pix_res = 1280;
	}
	else {
		d_pix_res = r_refdef.vrect.width;
	}

	d_pix_min = d_pix_res / 320;
	if (d_pix_min < 1)
		d_pix_min = 1;

	d_pix_max = (int)(d_pix_res / (320.0 / 4.0) + 0.5);
	d_pix_shift = 8 - (int)(d_pix_res / 320.0 + 0.5);
	if (d_pix_max < 1)
		d_pix_max = 1;

	if (pixelAspect > 1.4)
		d_y_aspect_shift = 1;
	else
		d_y_aspect_shift = 0;

	d_vrectx = r_refdef.vrect.x;
	d_vrecty = r_refdef.vrect.y;
	d_vrectright_particle = r_refdef.vrectright - d_pix_max;
	d_vrectbottom_particle =
			r_refdef.vrectbottom - (d_pix_max << d_y_aspect_shift);

	{
		int		i;

		for (i=0 ; i<vid.height; i++)
		{
			d_scantable[i] = i*rowbytes;
			zspantable[i] = d_pzbuffer + i*d_zwidth;
		}
	}
}

