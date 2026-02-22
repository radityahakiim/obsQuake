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
#include "quakedef.h"
#include "linuxcrossplat.h"

static qboolean snd_inited = false;
static qboolean snd_firsttime = true;

static SDL_AudioDeviceID sdl_audio;
static unsigned int gSndBufSize;

static void SNDDMA_Callback(void* userdata, Uint8* stream, int len)
{
	int pos 	= shm->samplepos *  (shm->samplebits / 8);
	int bufsize = shm->samples 	 *	(shm->samplebits / 8);
	int len1 	= bufsize - pos;
	int len2 	= 0;
	
	if (len1 > len)
		len1 = len;
	else
		len2 = len - len1;
		
	memcpy(stream, shm->buffer + pos, len1);
	if (len2 > 0)
		memcpy(stream + len1, shm->buffer, len2);
	
	shm->samplepos += len / (shm->samplebits / 8);
	if (shm->samplepos >= shm->samples)
		shm->samplepos -= shm->samples;
}

void S_BlockSound (void){
	if (!snd_inited)
		return;
		
	if (snd_blocked == 0)
	{
		SDL_PauseAudioDevice(sdl_audio, 1);
	}
	snd_blocked++;
}

void S_UnblockSound (void){
	if (!snd_inited)
		return;
		
	snd_blocked--;
	if (snd_blocked == 0)
	{
		SDL_PauseAudioDevice(sdl_audio, 1);
	}
}

qboolean SNDDMA_Init(void)
{
	SDL_AudioSpec desired, obtained;
	int buffer_frames;
	int frame_size;
	
	int possible_rates[] = { 48000, 44100, 22050, 11025, 8000 };
	int num_rates = sizeof(possible_rates) / sizeof(possible_rates[0]);
	int selected_rate = 0;

	memset ((void *)&sn, 0, sizeof (sn));
	
	shm = &sn;

	shm->channels = 2;
	shm->samplebits = 16;
	// shm->speed = 48000;

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
	{
		Con_SafePrintf("Couldn't init SDL audio: %s\n", SDL_GetError());
		return 0;
	}

	memset (&desired, 0, sizeof(desired));
	// desired.freq = shm->speed;
	desired.format = AUDIO_S16LSB;
	desired.channels = shm->channels;
	desired.samples = 1024;
	desired.callback = SNDDMA_Callback;
	desired.userdata = NULL;

	for (int i = 0; i < num_rates; i++) {
		desired.freq = possible_rates[i];
		sdl_audio = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
		if (sdl_audio != 0) {
			selected_rate = obtained.freq;
			break;
		}
	}
	if (sdl_audio == 0)
	{
		Con_SafePrintf("Couldn't open SDL audio: %s\n", SDL_GetError());
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return 0;
	}

	shm->speed	  = selected_rate;
	shm->channels = obtained.channels;
	if (obtained.format != AUDIO_S16LSB && obtained.format != AUDIO_S16MSB)
	{
		Con_SafePrintf("Set audio format failed\n");
		SDL_CloseAudioDevice(sdl_audio);
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return 0;
	}
	shm->samplebits = 16;

	if (snd_firsttime)
		Con_SafePrintf("Set audio format: yes\n");

	frame_size	  = obtained.channels * (shm->samplebits / 8);
	buffer_frames = obtained.samples * 8; // mix ahead
	gSndBufSize   = buffer_frames * frame_size;

	shm->buffer = malloc(gSndBufSize);
	if (!shm->buffer)
	{
		Con_SafePrintf("Sound: out of memory\n");
		SDL_CloseAudioDevice(sdl_audio);
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return 0;
	}
	memset(shm->buffer, 0, gSndBufSize);

	shm->samples = gSndBufSize / (shm->samplebits / 8);
	shm->samplepos = 0;
	shm->submission_chunk = 1;
	shm->soundalive = true;
	shm->splitbuffer = true;

	snd_inited = true;
	snd_firsttime = false;

	SDL_PauseAudioDevice(sdl_audio, 0);

	if (snd_firsttime)
		Con_SafePrintf(" %d channels(s)\n"
			" %d bits/sample\n"
			" %d bytes/sec\n",
			shm->channels, shm->samplebits, shm->speed);

	return 1;
}

int SNDDMA_GetDMAPos(void)
{
	if (!snd_inited) return 0;
	return shm->samplepos & (shm->samples - 1);

}

void SNDDMA_Shutdown(void)
{
	if (snd_inited)
	{
		SDL_CloseAudioDevice(sdl_audio);
		sdl_audio = 0;
		if (shm && shm->buffer)
		{
				free(shm->buffer);
				shm->buffer = NULL;
		}
		snd_inited = false;
	}
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

/*
==============
SNDDMA_Submit

Send sound to device if buffer isn't really the dma buffer
===============
*/
void SNDDMA_Submit(void)
{
}

