// Linux compatibility for vid_sdl.c crossplatform
// between Windows and Linux

#include <SDL.h>
void IN_ShowMouse (void);
void IN_DeactivateMouse (void);
void IN_HideMouse (void);
void IN_ActivateMouse (void);
void IN_UpdateClipCursor (void);
void IN_ClearStates (void);

void S_BlockSound(void);
void S_UnblockSound(void);

extern qboolean ActiveApp, Minimized;

#undef VID_LockBuffer
#undef VID_UnlockBuffer

void VID_LockBuffer(void);
void VID_UnlockBuffer(void);

typedef enum { MS_WINDOWED, MS_FULLSCREEN, MS_FULLDIB, MS_UNINIT } modestate_t;
