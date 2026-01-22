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
// r_vars.c: global refresh variables

#include	"quakedef.h"

// all global and static refresh variables are collected in a contiguous block
// to avoid cache conflicts.

//-------------------------------------------------------
// global refresh variables
//-------------------------------------------------------

// FIXME: make into one big structure, like cl or sv
// FIXME: do separately for refresh engine and driver

float	d_sdivzstepu, d_tdivzstepu, d_zistepu;
float	d_sdivzstepv, d_tdivzstepv, d_zistepv;
float	d_sdivzorigin, d_tdivzorigin, d_ziorigin;

fixed16_t	sadjust, tadjust, bbextents, bbextentt;

pixel_t			*cacheblock;
int				cachewidth;
pixel_t			*d_viewbuffer;
short			*d_pzbuffer;
unsigned int	d_zrowbytes;
unsigned int	d_zwidth;

//-------------------------------------------------------
// migrated variables from d_varsa.s
//-------------------------------------------------------
int izi;
int izistep;
int counttemp;
int jumptemp;
pixel_t *pspantemp;
int sstep, tstep;
int advancetable[2]; // 8 bytes
int s, t, snext, tnext;
int sfracf, tfracf;
pixel_t *pbase;
float zi8stepu, sdivz8stepu, tdivz8stepu;
float zi16stepu, sdivz16stepu, tdivz16stepu;
int spancountminus1;
short *pz;



// reciprocal tables
unsigned int reciprocal_table[6] = {
	0x40000000, 0x2aaaaaaa, 0x20000000,
	0x19999999, 0x15555555, 0x12492492
};



//-------------------------------------------------------
// symbol aliasing for assembly
//-------------------------------------------------------

#pragma comment(linker, "/ALTERNATENAME:izi=_izi")
#pragma comment(linker, "/ALTERNATENAME:izistep=_izistep")
#pragma comment(linker, "/ALTERNATENAME:counttemp=_counttemp")
#pragma comment(linker, "/ALTERNATENAME:jumptemp=_jumptemp")
#pragma comment(linker, "/ALTERNATENAME:pspantemp=_pspantemp")
#pragma comment(linker, "/ALTERNATENAME:sstep=_sstep")
#pragma comment(linker, "/ALTERNATENAME:tstep=_tstep")
#pragma comment(linker, "/ALTERNATENAME:advancetable=_advancetable")
#pragma comment(linker, "/ALTERNATENAME:s=_s")
#pragma comment(linker, "/ALTERNATENAME:t=_t")
#pragma comment(linker, "/ALTERNATENAME:snext=_snext")
#pragma comment(linker, "/ALTERNATENAME:tnext=_tnext")
#pragma comment(linker, "/ALTERNATENAME:sfracf=_sfracf")
#pragma comment(linker, "/ALTERNATENAME:tfracf=_tfracf")
#pragma comment(linker, "/ALTERNATENAME:pbase=_pbase")
#pragma comment(linker, "/ALTERNATENAME:zi8stepu=_zi8stepu")
#pragma comment(linker, "/ALTERNATENAME:sdivz8stepu=_sdivz8stepu")
#pragma comment(linker, "/ALTERNATENAME:tdivz8stepu=_tdivz8stepu")
#pragma comment(linker, "/ALTERNATENAME:zi16stepu=_zi16stepu")
#pragma comment(linker, "/ALTERNATENAME:sdivz16stepu=_sdivz16stepu")
#pragma comment(linker, "/ALTERNATENAME:tdivz16stepu=_tdivz16stepu")
#pragma comment(linker, "/ALTERNATENAME:spancountminus1=_spancountminus1")
#pragma comment(linker, "/ALTERNATENAME:pz=_pz")



#pragma comment(linker, "/ALTERNATENAME:reciprocal_table=_reciprocal_table")


extern void Spr8Entry2_8(void);
extern void Spr8Entry3_8(void);
extern void Spr8Entry4_8(void);
extern void Spr8Entry5_8(void);
extern void Spr8Entry6_8(void);
extern void Spr8Entry7_8(void);
extern void Spr8Entry8_8(void);



// alias underscore-prefixed C references to assembly symbols


#pragma comment(linker, "/ALTERNATENAME:_Spr8Entry2_8=Spr8Entry2_8")
#pragma comment(linker, "/ALTERNATENAME:_Spr8Entry3_8=Spr8Entry3_8")
#pragma comment(linker, "/ALTERNATENAME:_Spr8Entry4_8=Spr8Entry4_8")
#pragma comment(linker, "/ALTERNATENAME:_Spr8Entry5_8=Spr8Entry5_8")
#pragma comment(linker, "/ALTERNATENAME:_Spr8Entry6_8=Spr8Entry6_8")
#pragma comment(linker, "/ALTERNATENAME:_Spr8Entry7_8=Spr8Entry7_8")
#pragma comment(linker, "/ALTERNATENAME:_Spr8Entry8_8=Spr8Entry8_8")


// aliases for tables themselves

#pragma comment(linker, "/ALTERNATENAME:spr8entryvec_table=_spr8entryvec_table")


// dummy entryvec_table for d_draw.s (which is removed)
#pragma comment(linker, "/ALTERNATENAME:Entry2_8=_Entry2_8")
#pragma comment(linker, "/ALTERNATENAME:Entry3_8=_Entry3_8")
#pragma comment(linker, "/ALTERNATENAME:Entry4_8=_Entry4_8")
#pragma comment(linker, "/ALTERNATENAME:Entry5_8=_Entry5_8")
#pragma comment(linker, "/ALTERNATENAME:Entry6_8=_Entry6_8")
#pragma comment(linker, "/ALTERNATENAME:Entry7_8=_Entry7_8")
#pragma comment(linker, "/ALTERNATENAME:Entry8_8=_Entry8_8")

void Entry2_8(void) {}
void Entry3_8(void) {}
void Entry4_8(void) {}
void Entry5_8(void) {}
void Entry6_8(void) {}
void Entry7_8(void) {}
void Entry8_8(void) {}

void *entryvec_table[] = {
	0,
	Entry2_8, Entry3_8, Entry4_8,
	Entry5_8, Entry6_8, Entry7_8, Entry8_8
};
#pragma comment(linker, "/ALTERNATENAME:entryvec_table=_entryvec_table")

