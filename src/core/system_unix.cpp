/*
** Haaf's Game Engine 1.8
** Copyright (C) 2003-2007, Relish Games
** hge.relishgames.com
**
** Core system functions for Unix.
*/


#include "hge_impl_unix.h"

#if !PLATFORM_UNIX
#error This source file is for Unix and Mac OS X. Use system.cpp for Windows.
#endif

#if PLATFORM_UNIX

#if PLATFORM_MACOSX
#include <Carbon/Carbon.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#define LOWORDINT(n) ((int)((signed short)(LOWORD(n))))
#define HIWORDINT(n) ((int)((signed short)(HIWORD(n))))


int			nRef=0;
HGE_Impl*	pHGE=0;



HGE* CALL hgeCreate(int ver)
{
	if(ver==HGE_VERSION)
		return (HGE*)HGE_Impl::_Interface_Get();
	else
		return 0;
}


HGE_Impl* HGE_Impl::_Interface_Get()
{
	if(!pHGE) pHGE=new HGE_Impl();

	nRef++;

	return pHGE;
}


void CALL HGE_Impl::Release()
{
	nRef--;

	if(!nRef)
	{
		if(pHGE->hwnd) pHGE->System_Shutdown();
		Resource_RemoveAllPacks();
		delete pHGE;
		pHGE=0;
	}
}


bool CALL HGE_Impl::System_Initiate()
{
	int				width, height;

	// Log system info
	System_Log("HGE Started...");

	System_Log("HGE version: %X.%X", HGE_VERSION>>8, HGE_VERSION & 0xFF);

	time_t t = time(NULL);
	System_Log("Date: %s", asctime(localtime(&t)));

	System_Log("Application: %s",szWinTitle);

#if PLATFORM_MACOSX
    // !!! FIXME: this is wrong...it doesn't with with 10.y.xx, where 'xx'
    // !!! FIXME:  is more than one digit (it appears to clamp to 9 in these
    // !!! FIXME:  cases inside Gestalt(), for obvious reasons).
    // !!! FIXME: This is a legacy--and incorrect--way to get this information.
    long ver = 0x0000;
    char verbuf[16] = { '\0' };
	if (Gestalt(gestaltSystemVersion, &ver) == noErr)
    {
        char str[16];
        snprintf(str, sizeof (str), "%X", (int) ver);
        snprintf(verbuf, sizeof (verbuf), " %c%c.%c.%c", str[0], str[1], str[2], str[3]);
    } // if

    System_Log("OS: Mac OS X%s", verbuf);

	unsigned long phys = 0;
	size_t len = sizeof (phys);
	int mib[2] = { CTL_HW, HW_PHYSMEM };
	if ((sysctl(mib, 2, &phys, &len, NULL, 0) != 0) || (len != sizeof (phys)))
		phys = 0;  // oh well.
	System_Log("Memory: %ldK total",phys/1024);

#else
	System_Log("OS: Unix");
	STUBBED("OS version and memory");
#endif

	if (SDL_Init(SDL_INIT_VIDEO) == -1) {
		char buffer[1024];
		snprintf(buffer, sizeof (buffer), "SDL_Init() failed: %s\n", SDL_GetError());
		_PostError(buffer);
		return false;
	}

	if (SDL_GL_LoadLibrary(NULL) == -1) {
		char buffer[1024];
		snprintf(buffer, sizeof (buffer), "SDL_GL_LoadLibrary() failed: %s\n", SDL_GetError());
		_PostError(buffer);
		SDL_Quit();
		return false;
	}

	// Create window
	SDL_WM_SetCaption(szWinTitle, szWinTitle);
	Uint32 flags = SDL_OPENGL;
STUBBED("windowed/fullscreen mode");
//	if (!bWindowed)
//		flags |= SDL_FULLSCREEN;
	hwnd = SDL_SetVideoMode(nScreenWidth, nScreenHeight, nScreenBPP, flags);
	if (!hwnd)
	{
		char buffer[1024];
		snprintf(buffer, sizeof (buffer), "SDL_SetVideoMode() failed: %s\n", SDL_GetError());
		_PostError(buffer);
		SDL_Quit();
		return false;
	}

	// Init subsystems

	Random_Seed();
	_InitPowerStatus();
	_InputInit();
	if(!_GfxInit()) { System_Shutdown(); return false; }
	if(!_SoundInit()) { System_Shutdown(); return false; }

	System_Log("Init done.\n");

	fTime=0.0f;
	t0=t0fps=SDL_GetTicks();
	dt=cfps=0;
	nFPS=0;

	// Show splash

#ifdef DEMO

	bool			(*func)();
	bool			(*rfunc)();
	HWND			hwndTmp;

	if(pHGE->bDMO)
	{
		SDL_Delay(200);
		func=(bool(*)())pHGE->System_GetStateFunc(HGE_FRAMEFUNC);
		rfunc=(bool(*)())pHGE->System_GetStateFunc(HGE_RENDERFUNC);
		hwndTmp=hwndParent; hwndParent=0;
		pHGE->System_SetStateFunc(HGE_FRAMEFUNC, DFrame);
		pHGE->System_SetStateFunc(HGE_RENDERFUNC, 0);
		DInit();
		pHGE->System_Start();
		DDone();
		hwndParent=hwndTmp;
		pHGE->System_SetStateFunc(HGE_FRAMEFUNC, func);
		pHGE->System_SetStateFunc(HGE_RENDERFUNC, rfunc);
	}

#endif

	// Done

	return true;
}

void CALL HGE_Impl::System_Shutdown()
{
	System_Log("\nFinishing..");

	if(hSearch) { closedir(hSearch); hSearch=0; }
	_ClearQueue();
	_SoundDone();
	_GfxDone();
	_DonePowerStatus();

	SDL_Quit();
	hwnd=0;

	System_Log("The End.");
}


bool CALL HGE_Impl::System_Start()
{
	if(!hwnd)
	{
		_PostError("System_Start: System_Initiate wasn't called");
		return false;
	}

	if(!procFrameFunc) {
		_PostError("System_Start: No frame function defined");
		return false;
	}

	bActive=true;

	// MAIN LOOP
	for(;;)
	{
		SDL_Event e;
		bool keep_going = true;
		while (SDL_PollEvent(&e) && keep_going)
			keep_going = _ProcessSDLEvent(e);

		if (!keep_going)
			break;

		// Check if mouse is over HGE window for Input_IsMouseOver

		_UpdateMouse();

		// If HGE window is focused or we have the "don't suspend" state - process the main loop

		if(bActive || bDontSuspend)
		{
			// Ensure we have at least 1ms time step
			// to not confuse user's code with 0

			do { dt=SDL_GetTicks() - t0; } while(dt < 1);

			// If we reached the time for the next frame
			// or we just run in unlimited FPS mode, then
			// do the stuff

			if(dt >= nFixedDelta)
			{
				// fDeltaTime = time step in seconds returned by Timer_GetDelta

				fDeltaTime=dt/1000.0f;

				// Cap too large time steps usually caused by lost focus to avoid jerks

				if(fDeltaTime > 0.2f)
				{
					fDeltaTime = nFixedDelta ? nFixedDelta/1000.0f : 0.01f;
				}

				// Update time counter returned Timer_GetTime

				fTime += fDeltaTime;

				// Store current time for the next frame
				// and count FPS

				t0=SDL_GetTicks();
				if(t0-t0fps <= 1000) cfps++;
				else
				{
					nFPS=cfps; cfps=0; t0fps=t0;
					_UpdatePowerStatus();
				}

				// Do user's stuff

				if(procFrameFunc()) break;
				if(procRenderFunc) procRenderFunc();
				
				// If if "child mode" - return after processing single frame

				if(hwndParent) break;

				// Clean up input events that were generated by
				// WindowProc and weren't handled by user's code

				_ClearQueue();

				// If we use VSYNC - we could afford a little
				// sleep to lower CPU usage

				// if(!bWindowed && nHGEFPS==HGEFPS_VSYNC) Sleep(1);
			}

			// If we have a fixed frame rate and the time
			// for the next frame isn't too close, sleep a bit

			else
			{
				if(nFixedDelta && dt+3 < nFixedDelta) SDL_Delay(1);
			}
		}

		// If main loop is suspended - just sleep a bit
		// (though not too much to allow instant window
		// redraw if requested by OS)

		else SDL_Delay(1);
	}

	_ClearQueue();

	bActive=false;

	return true;
}

void CALL HGE_Impl::System_SetStateBool(hgeBoolState state, bool value)
{
	switch(state)
	{
		case HGE_WINDOWED:		if(VertArray || hwndParent) break;
								if(pOpenGLDevice && bWindowed != value)
								{
                                    STUBBED("backbuffer format");
									//if(d3dppW.BackBufferFormat==D3DFMT_UNKNOWN || d3dppFS.BackBufferFormat==D3DFMT_UNKNOWN) break;

									bWindowed=value;

									//if(_format_id(d3dpp->BackBufferFormat) < 4) nScreenBPP=16;
									//else nScreenBPP=32;

									_GfxRestore();
								}
								break;

		case HGE_ZBUFFER:		if(!pOpenGLDevice) bZBuffer=value;
								break;

		case HGE_TEXTUREFILTER: if (bTextureFilter==value) break;
								bTextureFilter=value;
								if(pOpenGLDevice)
									_render_batch();

								// bTextureFilter is pushed to the GL next time you draw.
								break;

		case HGE_USESOUND:		if(bUseSound!=value)
								{
									bUseSound=value;
									if(bUseSound) _SoundInit();
									if(!bUseSound) _SoundDone();
								}
								break;

		case HGE_HIDEMOUSE:		bHideMouse=value; break;

		case HGE_DONTSUSPEND:	bDontSuspend=value; break;

		#ifdef DEMO
		case HGE_SHOWSPLASH:	bDMO=value; break;
		#endif
	}
}

void CALL HGE_Impl::System_SetStateFunc(hgeFuncState state, hgeCallback value)
{
	switch(state)
	{
		case HGE_FRAMEFUNC:		 procFrameFunc=value; break;
		case HGE_RENDERFUNC:	 procRenderFunc=value; break;
		case HGE_FOCUSLOSTFUNC:	 procFocusLostFunc=value; break;
		case HGE_FOCUSGAINFUNC:	 procFocusGainFunc=value; break;
		case HGE_GFXRESTOREFUNC: procGfxRestoreFunc=value; break;
		case HGE_EXITFUNC:		 procExitFunc=value; break;
	}
}

void CALL HGE_Impl::System_SetStateHwnd(hgeHwndState state, HWND value)
{
	switch(state)
	{
		case HGE_HWNDPARENT:
			if (value != 0) {
				System_Log("WARNING: Trying to set HGE_HWNDPARENT is unsupported!\n");
				System_Log("WARNING: You will not get the behaviour you expect\n");
			}
			if(!hwnd)
				hwndParent=value;
			break;
	}
}

void CALL HGE_Impl::System_SetStateInt(hgeIntState state, int value)
{
	switch(state)
	{
		case HGE_SCREENWIDTH:	if(!pOpenGLDevice) nScreenWidth=value; break;

		case HGE_SCREENHEIGHT:	if(!pOpenGLDevice) nScreenHeight=value; break;

		case HGE_SCREENBPP:		if(!pOpenGLDevice) nScreenBPP=value; break;

		case HGE_SAMPLERATE:	if(!hOpenALDevice) nSampleRate=value;
								break;

		case HGE_FXVOLUME:		nFXVolume=value;
								_SetFXVolume(nFXVolume);
								break;

		case HGE_MUSVOLUME:		nMusVolume=value;
								_SetMusVolume(nMusVolume);
								break;

		case HGE_STREAMVOLUME:	nStreamVolume=value;
								_SetStreamVolume(nStreamVolume);
								break;

		case HGE_FPS:			if(VertArray) break;

								if(pOpenGLDevice)
								{
									if((nHGEFPS>=0 && value <0) || (nHGEFPS<0 && value>=0))
									{
										STUBBED("vsync stuff");
										#if 0
										if(value==HGEFPS_VSYNC)
										{
											d3dppW.SwapEffect = D3DSWAPEFFECT_COPY_VSYNC;
											d3dppFS.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;
										}
										else
										{
											d3dppW.SwapEffect = D3DSWAPEFFECT_COPY;
											d3dppFS.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
										}
										//if(procFocusLostFunc) procFocusLostFunc();
										_GfxRestore();
										//if(procFocusGainFunc) procFocusGainFunc();
										#endif
									}
								}
								nHGEFPS=value;
								if(nHGEFPS>0) nFixedDelta=int(1000.0f/value);
								else nFixedDelta=0;
								break;
	}
}

void CALL HGE_Impl::System_SetStateString(hgeStringState state, const char *value)
{
	FILE *hf;
	
	switch(state)
	{
		case HGE_ICON:			szIcon=value;
								STUBBED("icon");
								//if(pHGE->hwnd) SetClassLong(pHGE->hwnd, GCL_HICON, (LONG)LoadIcon(pHGE->hInstance, szIcon));
								break;
		case HGE_TITLE:			strcpy(szWinTitle,value);
								if(pHGE->hwnd) SDL_WM_SetCaption(value, value);
								break;
		case HGE_INIFILE:		if(value) strcpy(szIniFile,Resource_MakePath(value));
								else szIniFile[0]=0;
								break;
		case HGE_LOGFILE:		if(value)
								{
									strcpy(szLogFile,Resource_MakePath(value));
									hf=fopen(szLogFile, "w");
									if(!hf) szLogFile[0]=0;
									else fclose(hf);
								}
								else szLogFile[0]=0;
								break;
	}
}

bool CALL HGE_Impl::System_GetStateBool(hgeBoolState state)
{
	switch(state)
	{
		case HGE_WINDOWED:		return bWindowed;
		case HGE_ZBUFFER:		return bZBuffer;
		case HGE_TEXTUREFILTER:	return bTextureFilter;
		case HGE_USESOUND:		return bUseSound;
		case HGE_DONTSUSPEND:	return bDontSuspend;
		case HGE_HIDEMOUSE:		return bHideMouse;

		#ifdef DEMO
		case HGE_SHOWSPLASH:	return bDMO;
		#endif
	}

	return false;
}

hgeCallback CALL HGE_Impl::System_GetStateFunc(hgeFuncState state)
{
	switch(state)
	{
		case HGE_FRAMEFUNC:		return procFrameFunc;
		case HGE_RENDERFUNC:	return procRenderFunc;
		case HGE_FOCUSLOSTFUNC:	return procFocusLostFunc;
		case HGE_FOCUSGAINFUNC:	return procFocusGainFunc;
		case HGE_EXITFUNC:		return procExitFunc;
	}

	return NULL;
}

HWND CALL HGE_Impl::System_GetStateHwnd(hgeHwndState state)
{
	switch(state)
	{
		case HGE_HWND:			return hwnd;
		case HGE_HWNDPARENT:	return hwndParent;
	}

	return 0;
}

int CALL HGE_Impl::System_GetStateInt(hgeIntState state)
{
	switch(state)
	{
		case HGE_SCREENWIDTH:	return nScreenWidth;
		case HGE_SCREENHEIGHT:	return nScreenHeight;
		case HGE_SCREENBPP:		return nScreenBPP;
		case HGE_SAMPLERATE:	return nSampleRate;
		case HGE_FXVOLUME:		return nFXVolume;
		case HGE_MUSVOLUME:		return nMusVolume;
		case HGE_STREAMVOLUME:	return nStreamVolume;
		case HGE_FPS:			return nHGEFPS;
		case HGE_POWERSTATUS:	return nPowerStatus;
	}

	return 0;
}

const char* CALL HGE_Impl::System_GetStateString(hgeStringState state) {
	switch(state) {
		case HGE_ICON:			return szIcon;
		case HGE_TITLE:			return szWinTitle;
		case HGE_INIFILE:		if(szIniFile[0]) return szIniFile;
								else return 0;
		case HGE_LOGFILE:		if(szLogFile[0]) return szLogFile;
								else return 0;
	}

	return NULL;
}

char* CALL HGE_Impl::System_GetErrorMessage()
{
	return szError;
}

void CALL HGE_Impl::System_Log(const char *szFormat, ...)
{
	FILE *hf = NULL;
	va_list ap;
	
	if(!szLogFile[0]) return;

	hf = fopen(szLogFile, "a");
	if(!hf) return;

	va_start(ap, szFormat);
	vfprintf(hf, szFormat, ap);
	va_end(ap);

	fprintf(hf, "\n");

	fclose(hf);
}

bool CALL HGE_Impl::System_Launch(const char *url)
{
#if PLATFORM_MACOSX
	CFURLRef cfurl = CFURLCreateWithBytes(NULL, (const UInt8 *) url,
	                                      strlen(url), kCFStringEncodingUTF8, NULL);
	const OSStatus err = LSOpenCFURLRef(cfurl, NULL);
	CFRelease(cfurl);
	return (err == noErr);
#else
	STUBBED("launch URL");
	return false;
#endif
}

void CALL HGE_Impl::System_Snapshot(const char *filename)
{
	char *shotname, tempname[_MAX_PATH];
	int i;

	if(!filename)
	{
		i=0;
		shotname=Resource_EnumFiles("shot???.bmp");
		while(shotname)
		{
			i++;
			shotname=Resource_EnumFiles();
		}
		sprintf(tempname, "shot%03d.bmp", i);
		filename=Resource_MakePath(tempname);
	}

	if(pOpenGLDevice)
	{
		#if SDL_BYTEORDER == SDL_BIG_ENDIAN
		const Uint32 rmask = 0xFF0000;
		const Uint32 gmask = 0x00FF00;
		const Uint32 bmask = 0x0000FF;
		#else
		const Uint32 rmask = 0x0000FF;
		const Uint32 gmask = 0x00FF00;
		const Uint32 bmask = 0xFF0000;
		#endif

		pOpenGLDevice->glFinish();  // make sure screenshot is ready.
		SDL_Surface *screen = SDL_GetVideoSurface();
		SDL_Surface *surface = SDL_CreateRGBSurface(SDL_SWSURFACE, screen->w, screen->h, 24, rmask, gmask, bmask, 0);
		pOpenGLDevice->glReadPixels(0, 0, screen->w, screen->h, GL_RGB, GL_UNSIGNED_BYTE, surface->pixels);
		STUBBED("image is probably upside down");
		SDL_SaveBMP(surface, filename);
		SDL_FreeSurface(surface);
	}
}

//////// Implementation ////////


HGE_Impl::HGE_Impl()
{
	//hInstance=GetModuleHandle(0);
	hwnd=0;
	bActive=false;
	szError[0]=0;

	pOpenGLDevice=0;
	pTargets=0;
	pCurTarget=0;
	//pScreenSurf=0;
	//pScreenDepth=0;
	pVB=0;
	pIB=0;
	VertArray=0;
	textures=0;

	hOpenALDevice=0;
	bSilent=false;
	streams=0;

	hSearch=0;
	res=0;

	queue=0;
	Char=VKey=Zpos=0;
	Xpos=Ypos=0.0f;
	bMouseOver=false;
	bCaptured=false;

	nHGEFPS=HGEFPS_UNLIMITED;
	fTime=0.0f;
	fDeltaTime=0.0f;
	nFPS=0;
	
	procFrameFunc=0;
	procRenderFunc=0;
	procFocusLostFunc=0;
	procFocusGainFunc=0;
	procGfxRestoreFunc=0;
	procExitFunc=0;
	szIcon=0;
	strcpy(szWinTitle,"HGE");
	nScreenWidth=800;
	nScreenHeight=600;
	nScreenBPP=32;
	bWindowed=false;
	bZBuffer=false;
	bTextureFilter=true;
	szLogFile[0]=0;
	szIniFile[0]=0;
	bUseSound=true;
	nSampleRate=44100;
	nFXVolume=100;
	nMusVolume=100;
	nStreamVolume=100;
	nFixedDelta=0;
	bHideMouse=true;
	bDontSuspend=false;
	hwndParent=0;
	keymods=KMOD_NONE;

	nPowerStatus=HGEPWR_UNSUPPORTED;

#ifdef DEMO
	bDMO=true;
#endif

	STUBBED("get basedir");
//	GetModuleFileName(GetModuleHandle(NULL), szAppPath, sizeof(szAppPath));
	szAppPath[0] = '\0';
	int i;
	for(i=strlen(szAppPath)-1; i>0; i--) if(szAppPath[i]=='/') break;
	szAppPath[i+1]=0;
}

void HGE_Impl::_PostError(char *error)
{
	System_Log(error);
	strcpy(szError,error);
}

void HGE_Impl::_FocusChange(bool bAct)
{
	bActive=bAct;

	if(bActive)
	{
		if(procFocusGainFunc) procFocusGainFunc();
	}
	else
	{
		if(procFocusLostFunc) procFocusLostFunc();
	}
}

bool HGE_Impl::_ProcessSDLEvent(const SDL_Event &e)
{
	switch(e.type)
	{
		case SDL_VIDEOEXPOSE:
			if(pHGE->procRenderFunc && pHGE->bWindowed) procRenderFunc();
			break;

		case SDL_QUIT:
			if(pHGE->procExitFunc && !pHGE->procExitFunc()) break;
			return false;

		case SDL_ACTIVEEVENT:
			const bool bActivating = (e.active.gain != 0);
			if (e.active.state & SDL_APPINPUTFOCUS) {
				if(pHGE->bActive != bActivating) pHGE->_FocusChange(bActivating);
			}
			if (e.active.state & SDL_APPMOUSEFOCUS) {
				bMouseOver = bActivating;
			}
			break;

		case SDL_KEYDOWN:
			keymods = e.key.keysym.mod;
			#if PLATFORM_MACOSX  // handle Apple-Q hotkey.
			if ((keymods & KMOD_META) && (e.key.keysym.sym == SDLK_q)) {
				if(pHGE->procExitFunc && !pHGE->procExitFunc()) break;
				return false;
			}
			#endif
			pHGE->_BuildEvent(INPUT_KEYDOWN, e.key.keysym.sym, 0, 0 /*(lparam & 0x40000000) ? HGEINP_REPEAT:0*/, -1, -1);
			break;

		case SDL_KEYUP:
			keymods = e.key.keysym.mod;
			pHGE->_BuildEvent(INPUT_KEYUP, e.key.keysym.sym, 0, 0, -1, -1);
			break;

		case SDL_MOUSEBUTTONDOWN:
			if (e.button.button == SDL_BUTTON_LEFT)
				pHGE->_BuildEvent(INPUT_MBUTTONDOWN, HGEK_LBUTTON, 0, 0, e.button.x, e.button.y);
			else if (e.button.button == SDL_BUTTON_RIGHT)
				pHGE->_BuildEvent(INPUT_MBUTTONDOWN, HGEK_RBUTTON, 0, 0, e.button.x, e.button.y);
			else if (e.button.button == SDL_BUTTON_MIDDLE)
				pHGE->_BuildEvent(INPUT_MBUTTONDOWN, HGEK_MBUTTON, 0, 0, e.button.x, e.button.y);
			else if (e.button.button == SDL_BUTTON_WHEELUP)
				pHGE->_BuildEvent(INPUT_MOUSEWHEEL, 1, 0, 0, e.button.x, e.button.y);
			else if (e.button.button == SDL_BUTTON_WHEELDOWN)
				pHGE->_BuildEvent(INPUT_MOUSEWHEEL, -1, 0, 0, e.button.x, e.button.y);
			break;

#if 0
		case WM_LBUTTONDBLCLK:
			pHGE->_BuildEvent(INPUT_MBUTTONDOWN, HGEK_LBUTTON, 0, HGEINP_REPEAT, LOWORDINT(lparam), HIWORDINT(lparam));
			return FALSE;
		case WM_MBUTTONDBLCLK:
			pHGE->_BuildEvent(INPUT_MBUTTONDOWN, HGEK_MBUTTON, 0, HGEINP_REPEAT, LOWORDINT(lparam), HIWORDINT(lparam));
			return FALSE;
		case WM_RBUTTONDBLCLK:
			pHGE->_BuildEvent(INPUT_MBUTTONDOWN, HGEK_RBUTTON, 0, HGEINP_REPEAT, LOWORDINT(lparam), HIWORDINT(lparam));
			return FALSE;
#endif

		case SDL_MOUSEBUTTONUP:
			if (e.button.button == SDL_BUTTON_LEFT)
				pHGE->_BuildEvent(INPUT_MBUTTONUP, HGEK_LBUTTON, 0, 0, e.button.x, e.button.y);
			else if (e.button.button == SDL_BUTTON_RIGHT)
				pHGE->_BuildEvent(INPUT_MBUTTONUP, HGEK_RBUTTON, 0, 0, e.button.x, e.button.y);
			else if (e.button.button == SDL_BUTTON_MIDDLE)
				pHGE->_BuildEvent(INPUT_MBUTTONUP, HGEK_MBUTTON, 0, 0, e.button.x, e.button.y);
			break;

		case SDL_MOUSEMOTION:
			pHGE->_BuildEvent(INPUT_MOUSEMOVE, 0, 0, 0, e.motion.x, e.motion.y);
			break;
			

#if 0  // !!! FIXME
		case WM_SIZE:
			if(pHGE->pD3D && wparam==SIZE_RESTORED) pHGE->_Resize(LOWORD(lparam), HIWORD(lparam));
			//return FALSE;
			break;
#endif
	}

	return true;
}

#endif  // PLATFORM_UNIX

// end of system_unix.cpp ...

