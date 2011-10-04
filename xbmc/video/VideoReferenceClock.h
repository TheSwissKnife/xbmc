#pragma once
/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "system.h" // for HAS_XRANDR, and Win32 types
#include "threads/Thread.h"
#include "threads/CriticalSection.h"

//TODO: get rid of #ifdef hell, abstract implementations in separate classes

#if defined(HAS_GLX) && defined(HAS_XRANDR)
  #include <X11/X.h>
  #include <X11/Xlib.h>
  #include <GL/glx.h>
#elif defined(_WIN32) && defined(HAS_DX)
  #include <d3d9.h>
  #include "guilib/D3DResource.h"

class CD3DCallback : public ID3DResource
{
  public:
    void Reset();
    void OnDestroyDevice();
    void OnCreateDevice();
    void Aquire();
    void Release();
    bool IsValid();

  private:
    bool m_devicevalid;
    bool m_deviceused;

    CCriticalSection m_critsection;
    CEvent           m_createevent;
    CEvent           m_releaseevent;
};

#endif

// size of vblank times array
#define VBLANKTIMESSIZE 201
// confidence levels for the recorded vblank times
#define VBLANKTIMESQUITECONFIDENT 30
#define VBLANKTIMESVERYCONFIDENT 100

class CVideoReferenceClock : public CThread
{
  public:
    CVideoReferenceClock();

    int64_t GetTime(bool interpolated = true);
    int64_t GetTime(int64_t* InterpolatedTime);
    int64_t GetNextTickTime(int64_t Target = 0);
    int64_t GetFrequency();
    bool    SetSpeed(double Speed);
    double  GetSpeed();
    int     GetRefreshRate(double* interval = NULL);
    int64_t ConvertSystemDurToClockDur(int64_t duration);

    int64_t DurUntilNextVBlank(int64_t ClockInterval = 0);
    int64_t TimeOfNextVBlank(int64_t wait = 0, int safetyTolerance = 0);
    int64_t Wait(int64_t Target = 0, int64_t* WaitedTime = 0, bool lateWaitTick = false);

    bool    WaitStarted(int MSecs);
    bool    WaitStable(int MSecs);
    bool    GetClockInfo(int& MissedVblanks, double& ClockSpeed, int& RefreshRate, double& MeasuredRefreshRate);
    void    SetFineAdjust(double fineadjust);
    void    RefreshChanged() { m_RefreshChanged = 1; }

#if defined(__APPLE__)
    void VblankHandler(int64_t nowtime, double fps);
#endif

  private:
    void    Process();
    bool    UpdateRefreshrate(bool Forced = false);
    void    SendVblankSignal();
    void    UpdateClock(int NrVBlanks, int64_t measuredVblankTime);
    double  UpdateInterval();
    void    NotifyRefreshChanged();
    int     CorrectVBlankTracking(int64_t* measuredTime);

    int64_t m_CurrTime;          //the current time of the clock when using vblank as clock source
    int64_t m_LastIntTime;       //last interpolated clock value, to make sure the clock doesn't go backwards
    double  m_CurrTimeFract;     //fractional part that is lost due to rounding when updating the clock
    double  m_ClockSpeed;        //the frequency of the clock set by dvdplayer
    int64_t m_ClockOffset;       //the difference between the vblank clock and systemclock, set when vblank clock is stopped
    int64_t m_LastRefreshTime;   //last time we updated the refreshrate
    int64_t m_RefreshRateChangeStartTime;   //last time we were asked to look for a refreshrate change
    int64_t m_SystemFrequency;   //frequency of the systemclock
    double  m_fineadjust;

    bool    m_UseVblank;         //set to true when vblank is used as clock source
    int64_t m_RefreshRate;       //current refreshrate
    int     m_PrevRefreshRate;   //previous refreshrate, used for log printing and getting refreshrate from nvidia-settings
    int     m_MissedVblanks;     //number of clock updates missed by the vblank clock
    int     m_RefreshChanged;    //1 = we changed the refreshrate, 2 = we should check the refreshrate forced
    int     m_TotalMissedVblanks;//total number of clock updates missed, used by codec information screen
    int64_t m_VblankTime;        //last time the clock was updated when using vblank as clock
    int     m_SafeSample;//the safe sample number
    int64_t m_SafeSampleTime; //the vblanktime (system time) of the safe sample
    int     m_SampledVblankCount; //the count of vblanks sampled from querying driver since stream start 
    int64_t m_SampledVblankTimes[VBLANKTIMESSIZE]; //array of last VBLANKTIMESSIZE system times of sampled vblanks
    int     m_VblankSampleConfidence; //counter to keep track of our confidence in sample accuracy
    int     m_PreviousRefreshRate; //previous refresh rate 
    int64_t m_PVblankInterval; //estimated actual system clock interval between vblanks
    double  m_ClockTickInterval;	//clock tick size (ie m_CurrTime increments)

    CEvent  m_Stable;             //set when the vblank clock is stable
    CEvent  m_Started;            //set when the vblank clock is started
    CEvent  m_VblankEvent;        //set when a vblank happens

    CCriticalSection m_CritSection;

#if defined(HAS_GLX) && defined(HAS_XRANDR)
    bool SetupGLX();
    void RunGLX();
    void CleanupGLX();
    bool ParseNvSettings(int& RefreshRate);
    int  GetRandRRate();

    int  (*m_glXWaitVideoSyncSGI) (int, int, unsigned int*);
    int  (*m_glXGetVideoSyncSGI)  (unsigned int*);

    Display*     m_Dpy;
    XVisualInfo *m_vInfo;
    Window       m_Window;
    GLXContext   m_Context;
    int          m_RREventBase;

    bool         m_UseNvSettings;

#elif defined(_WIN32) && defined(HAS_DX)
    bool   SetupD3D();
    double MeasureRefreshrate(int MSecs);
    void   RunD3D();
    void   CleanupD3D();

    LPDIRECT3DDEVICE9 m_D3dDev;
    CD3DCallback      m_D3dCallback;

    unsigned int  m_Width;
    unsigned int  m_Height;
    bool          m_Interlaced;

#elif defined(__APPLE__)
    bool SetupCocoa();
    void RunCocoa();
    void CleanupCocoa();

    int64_t m_LastVBlankTime;  //timestamp of the last vblank, used for calculating how many vblanks happened
                               //not the same as m_VblankTime
#endif
};

extern CVideoReferenceClock g_VideoReferenceClock;
