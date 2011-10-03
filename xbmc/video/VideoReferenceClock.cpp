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
#include "system.h"
#include <list>
#include "utils/StdString.h"
#include "VideoReferenceClock.h"
#include "utils/MathUtils.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"
#include "threads/SingleLock.h"
#include "Application.h"

#if defined(HAS_GLX) && defined(HAS_XRANDR)
  #include <sstream>
  #include <X11/extensions/Xrandr.h>
  #define NVSETTINGSCMD "nvidia-settings -nt -q RefreshRate3"
#elif defined(__APPLE__) && !defined(__arm__)
  #include <QuartzCore/CVDisplayLink.h>
  #include "CocoaInterface.h"
#elif defined(__APPLE__) && defined(__arm__)
  #include "WindowingFactory.h"
#elif defined(_WIN32) && defined(HAS_DX)
  #pragma comment (lib,"d3d9.lib")
  #if (D3DX_SDK_VERSION >= 42) //aug 2009 sdk and up there is no dxerr9 anymore
    #include <Dxerr.h>
    #pragma comment (lib,"DxErr.lib")
  #else
    #include <dxerr9.h>
    #define DXGetErrorString(hr)      DXGetErrorString9(hr)
    #define DXGetErrorDescription(hr) DXGetErrorDescription9(hr)
    #pragma comment (lib,"Dxerr9.lib")
  #endif
  #include "windowing/WindowingFactory.h"
  #include "settings/AdvancedSettings.h"
#endif

using namespace std;

#if defined(_WIN32) && defined(HAS_DX)

  void CD3DCallback::Reset()
  {
    m_devicevalid = true;
    m_deviceused = false;
  }

  void CD3DCallback::OnDestroyDevice()
  {
    CSingleLock lock(m_critsection);
    m_devicevalid = false;
    while (m_deviceused)
    {
      lock.Leave();
      m_releaseevent.Wait();
      lock.Enter();
    }
  }

  void CD3DCallback::OnCreateDevice()
  {
    CSingleLock lock(m_critsection);
    m_devicevalid = true;
    m_createevent.Set();
  }

  void CD3DCallback::Aquire()
  {
    CSingleLock lock(m_critsection);
    while(!m_devicevalid)
    {
      lock.Leave();
      m_createevent.Wait();
      lock.Enter();
    }
    m_deviceused = true;
  }

  void CD3DCallback::Release()
  {
    CSingleLock lock(m_critsection);
    m_deviceused = false;
    m_releaseevent.Set();
  }

  bool CD3DCallback::IsValid()
  {
    return m_devicevalid;
  }

#endif

CVideoReferenceClock::CVideoReferenceClock() : CThread("CVideoReferenceClock")
{
  m_SystemFrequency = CurrentHostFrequency();
  m_ClockSpeed = 1.0;
  m_ClockOffset = 0;
  m_TotalMissedVblanks = 0;
  m_Stable.Reset();
  m_UseVblank = false;
  m_Started.Reset();

#if defined(HAS_GLX) && defined(HAS_XRANDR)
  m_Dpy = NULL;
#endif
}

void CVideoReferenceClock::Process()
{
  bool SetupSuccess = false;
  int64_t Now;

#if defined(_WIN32) && defined(HAS_DX)
  //register callback
  m_D3dCallback.Reset();
  g_Windowing.Register(&m_D3dCallback);
#endif

  while(!m_bStop)
  {
    //set up the vblank clock
#if defined(HAS_GLX) && defined(HAS_XRANDR)
    SetupSuccess = SetupGLX();
#elif defined(_WIN32) && defined(HAS_DX)
    SetupSuccess = SetupD3D();
#elif defined(__APPLE__)
    SetupSuccess = SetupCocoa();
#elif defined(HAS_GLX)
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: compiled without RandR support");
#elif defined(_WIN32)
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: only available on directx build");
#else
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: no implementation available");
#endif

    m_Stable.Reset();
    m_VblankSampleConfidence = 0;
    m_SampledVblankCount = 0;
    m_SafeSample = 0;

    CSingleLock SingleLock(m_CritSection);
    Now = CurrentHostCounter();
    m_CurrTime = Now + m_ClockOffset; //add the clock offset from the previous time we stopped
    m_LastIntTime = m_CurrTime;
    m_CurrTimeFract = 0.0;
    m_ClockSpeed = 1.0;
    m_TotalMissedVblanks = 0;
    m_fineadjust = 1.0;
    m_RefreshChanged = 0;
    m_Started.Set();

    if (SetupSuccess)
    {
      m_UseVblank = true;          //tell other threads we're using vblank as clock
      m_VblankTime = Now;          //initialize the timestamp of the last vblank
      SingleLock.Leave();

      //run the clock
#if defined(HAS_GLX) && defined(HAS_XRANDR)
      RunGLX();
#elif defined(_WIN32) && defined(HAS_DX)
      RunD3D();
#elif defined(__APPLE__)
      RunCocoa();
#endif

    }
    else
    {
      SingleLock.Leave();
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: Setup failed, falling back to CurrentHostCounter()");
    }

    SingleLock.Enter();
    m_UseVblank = false;                       //we're back to using the systemclock
    Now = CurrentHostCounter();                //set the clockoffset between the vblank clock and systemclock
    m_ClockOffset = m_CurrTime - Now;
    SingleLock.Leave();

    //clean up the vblank clock
#if defined(HAS_GLX) && defined(HAS_XRANDR)
    CleanupGLX();
#elif defined(_WIN32) && defined(HAS_DX)
    CleanupD3D();
#elif defined(__APPLE__)
    CleanupCocoa();
#endif
    if (!SetupSuccess) break;
  }

#if defined(_WIN32) && defined(HAS_DX)
  g_Windowing.Unregister(&m_D3dCallback);
#endif
}

bool CVideoReferenceClock::WaitStarted(int MSecs)
{
  //not waiting here can cause issues with alsa
  return m_Started.WaitMSec(MSecs);
}

bool CVideoReferenceClock::WaitStable(int MSecs)
{
  CSingleLock SingleLock(m_CritSection);
  // wait for stability confidence
  if (m_UseVblank)
  {
     if (m_VblankSampleConfidence >= VBLANKTIMESVERYCONFIDENT)
        return true;
     SingleLock.Leave();
     return m_Stable.WaitMSec(MSecs);
  }
  else
     return true;
}

#if defined(HAS_GLX) && defined(HAS_XRANDR)
bool CVideoReferenceClock::SetupGLX()
{
  int singleBufferAttributes[] = {
    GLX_RGBA,
    GLX_RED_SIZE,      0,
    GLX_GREEN_SIZE,    0,
    GLX_BLUE_SIZE,     0,
    None
  };

  int ReturnV, SwaMask;
  unsigned int GlxTest;
  XSetWindowAttributes Swa;

  m_vInfo = NULL;
  m_Context = NULL;
  m_Window = 0;

  CLog::Log(LOGDEBUG, "CVideoReferenceClock: Setting up GLX");

  if (!m_Dpy)
  {
    m_Dpy = XOpenDisplay(NULL);
    if (!m_Dpy)
    {
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: Unable to open display");
      return false;
    }
  }

  if (!glXQueryExtension(m_Dpy, NULL, NULL))
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: X server does not support GLX");
    return false;
  }

  bool          ExtensionFound = false;
  istringstream Extensions(glXQueryExtensionsString(m_Dpy, DefaultScreen(m_Dpy)));
  string        ExtensionStr;

  while (!ExtensionFound)
  {
    Extensions >> ExtensionStr;
    if (Extensions.fail())
      break;

    if (ExtensionStr == "GLX_SGI_video_sync")
      ExtensionFound = true;
  }

  if (!ExtensionFound)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: X server does not support GLX_SGI_video_sync");
    return false;
  }

  m_vInfo = glXChooseVisual(m_Dpy, DefaultScreen(m_Dpy), singleBufferAttributes);
  if (!m_vInfo)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: glXChooseVisual returned NULL");
    return false;
  }

  Swa.border_pixel = 0;
  Swa.event_mask = StructureNotifyMask;
  Swa.colormap = XCreateColormap(m_Dpy, RootWindow(m_Dpy, m_vInfo->screen), m_vInfo->visual, AllocNone );
  SwaMask = CWBorderPixel | CWColormap | CWEventMask;

  m_Window = XCreateWindow(m_Dpy, RootWindow(m_Dpy, m_vInfo->screen), 0, 0, 256, 256, 0,
                           m_vInfo->depth, InputOutput, m_vInfo->visual, SwaMask, &Swa);

  m_Context = glXCreateContext(m_Dpy, m_vInfo, NULL, True);
  if (!m_Context)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: glXCreateContext returned NULL");
    return false;
  }

  ReturnV = glXMakeCurrent(m_Dpy, m_Window, m_Context);
  if (ReturnV != True)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: glXMakeCurrent returned %i", ReturnV);
    return false;
  }

  m_glXWaitVideoSyncSGI = (int (*)(int, int, unsigned int*))glXGetProcAddress((const GLubyte*)"glXWaitVideoSyncSGI");
  if (!m_glXWaitVideoSyncSGI)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: glXWaitVideoSyncSGI not found");
    return false;
  }

  ReturnV = m_glXWaitVideoSyncSGI(2, 0, &GlxTest);
  if (ReturnV)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: glXWaitVideoSyncSGI returned %i", ReturnV);
    return false;
  }

  m_glXGetVideoSyncSGI = (int (*)(unsigned int*))glXGetProcAddress((const GLubyte*)"glXGetVideoSyncSGI");
  if (!m_glXGetVideoSyncSGI)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: glXGetVideoSyncSGI not found");
    return false;
  }

  ReturnV = m_glXGetVideoSyncSGI(&GlxTest);
  if (ReturnV)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: glXGetVideoSyncSGI returned %i", ReturnV);
    return false;
  }

  XRRSizes(m_Dpy, m_vInfo->screen, &ReturnV);
  if (ReturnV == 0)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: RandR not supported");
    return false;
  }

  //set up receiving of RandR events, we'll get one when the refreshrate changes
  XRRQueryExtension(m_Dpy, &m_RREventBase, &ReturnV);
  XRRSelectInput(m_Dpy, RootWindow(m_Dpy, m_vInfo->screen), RRScreenChangeNotifyMask);

  UpdateRefreshrate(true); //forced refreshrate update
  m_MissedVblanks = 0;

  return true;
}

bool CVideoReferenceClock::ParseNvSettings(int& RefreshRate)
{
  double fRefreshRate;
  char   Buff[255];
  int    ReturnV;
  struct lconv *Locale = localeconv();
  FILE*  NvSettings;

  const char* VendorPtr = (const char*)glGetString(GL_VENDOR);
  if (!VendorPtr)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: glGetString(GL_VENDOR) returned NULL, not using nvidia-settings");
    return false;
  }

  CStdString Vendor = VendorPtr;
  Vendor.ToLower();
  if (Vendor.find("nvidia") == std::string::npos)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: GL_VENDOR:%s, not using nvidia-settings", Vendor.c_str());
    return false;
  }

  NvSettings = popen(NVSETTINGSCMD, "r");
  if (!NvSettings)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: %s: %s", NVSETTINGSCMD, strerror(errno));
    return false;
  }

  ReturnV = fscanf(NvSettings, "%254[^\n]", Buff);
  pclose(NvSettings);
  if (ReturnV != 1)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: %s produced no output", NVSETTINGSCMD);
    return false;
  }

  CLog::Log(LOGDEBUG, "CVideoReferenceClock: output of %s: %s", NVSETTINGSCMD, Buff);

  for (int i = 0; i < 255 && Buff[i]; i++)
  {
      //workaround for locale mismatch
    if (Buff[i] == '.' || Buff[i] == ',')
      Buff[i] = *Locale->decimal_point;
  }

  ReturnV = sscanf(Buff, "%lf", &fRefreshRate);
  if (ReturnV != 1 || fRefreshRate <= 0.0)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: can't make sense of that");
    return false;
  }

  RefreshRate = MathUtils::round_int(fRefreshRate);
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: Detected refreshrate by nvidia-settings: %f hertz, rounding to %i hertz",
            fRefreshRate, RefreshRate);

  return true;
}

int CVideoReferenceClock::GetRandRRate()
{
  int RefreshRate;
  XRRScreenConfiguration *CurrInfo;

  CurrInfo = XRRGetScreenInfo(m_Dpy, RootWindow(m_Dpy, m_vInfo->screen));
  RefreshRate = XRRConfigCurrentRate(CurrInfo);
  XRRFreeScreenConfigInfo(CurrInfo);

  return RefreshRate;
}

void CVideoReferenceClock::CleanupGLX()
{
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: Cleaning up GLX");

  bool AtiWorkaround = false;
  const char* VendorPtr = (const char*)glGetString(GL_VENDOR);
  if (VendorPtr)
  {
    CStdString Vendor = VendorPtr;
    Vendor.ToLower();
    if (Vendor.compare(0, 3, "ati") == 0)
    {
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: GL_VENDOR: %s, using ati dpy workaround", VendorPtr);
      AtiWorkaround = true;
    }
  }

  if (m_vInfo)
  {
    XFree(m_vInfo);
    m_vInfo = NULL;
  }
  if (m_Context)
  {
    glXMakeCurrent(m_Dpy, None, NULL);
     CLog::Log(LOGDEBUG, "ASB: CVideoReferenceClock: CleanupGLX glXDestroyContext");
    glXDestroyContext(m_Dpy, m_Context);
    m_Context = NULL;
  }
     CLog::Log(LOGDEBUG, "ASB: CVideoReferenceClock: CleanupGLX XDestroyWindow");
  if (m_Window)
  {
    XDestroyWindow(m_Dpy, m_Window);
    m_Window = 0;
  }

     CLog::Log(LOGDEBUG, "ASB: CVideoReferenceClock: CleanupGLX XCloseDisplay");
  //ati saves the Display* in their libGL, if we close it here, we crash
  if (m_Dpy && !AtiWorkaround)
  {
    XCloseDisplay(m_Dpy);
    m_Dpy = NULL;
  }
}

void CVideoReferenceClock::RunGLX()
{
  unsigned int  PrevVblankCount;
  unsigned int  VblankCount;
  int           ReturnV;
  bool          IsReset = false;
  int64_t       Now;

  CSingleLock SingleLock(m_CritSection);
  SingleLock.Leave();

  //get the current vblank counter
  m_glXGetVideoSyncSGI(&VblankCount);
  PrevVblankCount = VblankCount;

  while(!m_bStop)
  {
    //wait for the next vblank
    ReturnV = m_glXWaitVideoSyncSGI(2, (VblankCount + 1) % 2, &VblankCount);
    m_glXGetVideoSyncSGI(&VblankCount); //the vblank count returned by glXWaitVideoSyncSGI is not always correct
    Now = CurrentHostCounter();         //get the timestamp of this vblank

    if(ReturnV)
    {
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: glXWaitVideoSyncSGI returned %i", ReturnV);
      return;
    }

    if (VblankCount > PrevVblankCount)
    {
      //update the vblank timestamp, update the clock and send a signal that we got a vblank
      UpdateClock((int)(VblankCount - PrevVblankCount), Now);
      SendVblankSignal();
      UpdateRefreshrate();

      IsReset = false;
    }
    else
    {
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: Vblank counter has reset");

      //only try reattaching once
      if (IsReset)
        return;

      //because of a bug in the nvidia driver, glXWaitVideoSyncSGI breaks when the vblank counter resets
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: Detaching glX context");
      ReturnV = glXMakeCurrent(m_Dpy, None, NULL);
      if (ReturnV != True)
      {
        CLog::Log(LOGDEBUG, "CVideoReferenceClock: glXMakeCurrent returned %i", ReturnV);
        return;
      }

      CLog::Log(LOGDEBUG, "CVideoReferenceClock: Attaching glX context");
      ReturnV = glXMakeCurrent(m_Dpy, m_Window, m_Context);
      if (ReturnV != True)
      {
        CLog::Log(LOGDEBUG, "CVideoReferenceClock: glXMakeCurrent returned %i", ReturnV);
        return;
      }

      m_glXGetVideoSyncSGI(&VblankCount);

      IsReset = true;
    }
    PrevVblankCount = VblankCount;
  }
}

#elif defined(_WIN32) && defined(HAS_DX)

void CVideoReferenceClock::RunD3D()
{
  D3DRASTER_STATUS RasterStatus;
  int64_t       Now;
  int64_t       LastVBlankTime;
  unsigned int  LastLine;
  int           NrVBlanks;
  double        VBlankTime;
  int           ReturnV;

  CSingleLock SingleLock(m_CritSection);
  SingleLock.Leave();

  //get the scanline we're currently at
  m_D3dDev->GetRasterStatus(0, &RasterStatus);
  if (RasterStatus.InVBlank) LastLine = 0;
  else LastLine = RasterStatus.ScanLine;

  //init the vblanktime
  Now = CurrentHostCounter();
  LastVBlankTime = Now;

  while(!m_bStop && m_D3dCallback.IsValid())
  {
    //get the scanline we're currently at
    ReturnV = m_D3dDev->GetRasterStatus(0, &RasterStatus);
    if (ReturnV != D3D_OK)
    {
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: GetRasterStatus returned returned %s: %s",
                DXGetErrorString(ReturnV), DXGetErrorDescription(ReturnV));
      return;
    }

    //if InVBlank is set, or the current scanline is lower than the previous scanline, a vblank happened
    if ((RasterStatus.InVBlank && LastLine > 0) || (RasterStatus.ScanLine < LastLine))
    {
      //calculate how many vblanks happened
      Now = CurrentHostCounter();
      VBlankTime = (double)(Now - LastVBlankTime) / (double)m_SystemFrequency;
      NrVBlanks = MathUtils::round_int(VBlankTime * (double)m_RefreshRate);

      //update the vblank timestamp, update the clock and send a signal that we got a vblank
      UpdateClock(NrVBlanks, Now);
      SendVblankSignal();

      if (UpdateRefreshrate())
      {
        //we have to measure the refreshrate again
        CLog::Log(LOGDEBUG, "CVideoReferenceClock: Displaymode changed");
        return;
      }

      //save the timestamp of this vblank so we can calculate how many vblanks happened next time
      LastVBlankTime = Now;

      //because we had a vblank, sleep until half the refreshrate period
      Now = CurrentHostCounter();
      int SleepTime = (int)((LastVBlankTime + (m_SystemFrequency / m_RefreshRate / 2) - Now) * 1000 / m_SystemFrequency);
      if (SleepTime > 100) SleepTime = 100; //failsafe
      if (SleepTime > 0) ::Sleep(SleepTime);
    }
    else
    {
      ::Sleep(1);
    }

    if (RasterStatus.InVBlank) LastLine = 0;
    else LastLine = RasterStatus.ScanLine;
  }
}

//how many times we measure the refreshrate
#define NRMEASURES 6
//how long to measure in milliseconds
#define MEASURETIME 250

bool CVideoReferenceClock::SetupD3D()
{
  int ReturnV;

  CLog::Log(LOGDEBUG, "CVideoReferenceClock: Setting up Direct3d");

  m_D3dCallback.Aquire();

  //get d3d device
  m_D3dDev = g_Windowing.Get3DDevice();

  //we need a high priority thread to get accurate timing
  if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: SetThreadPriority failed");

  D3DCAPS9 DevCaps;
  ReturnV = m_D3dDev->GetDeviceCaps(&DevCaps);
  if (ReturnV != D3D_OK)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: GetDeviceCaps returned %s: %s",
                         DXGetErrorString(ReturnV), DXGetErrorDescription(ReturnV));
    return false;
  }

  if ((DevCaps.Caps & D3DCAPS_READ_SCANLINE) != D3DCAPS_READ_SCANLINE)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: Hardware does not support GetRasterStatus");
    return false;
  }

  D3DRASTER_STATUS RasterStatus;
  ReturnV = m_D3dDev->GetRasterStatus(0, &RasterStatus);
  if (ReturnV != D3D_OK)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: GetRasterStatus returned returned %s: %s",
              DXGetErrorString(ReturnV), DXGetErrorDescription(ReturnV));
    return false;
  }

  D3DDISPLAYMODE DisplayMode;
  ReturnV = m_D3dDev->GetDisplayMode(0, &DisplayMode);
  if (ReturnV != D3D_OK)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: GetDisplayMode returned returned %s: %s",
              DXGetErrorString(ReturnV), DXGetErrorDescription(ReturnV));
    return false;
  }

  //forced update of windows refreshrate
  UpdateRefreshrate(true);

  if (g_advancedSettings.m_measureRefreshrate)
  {
    //measure the refreshrate a couple times
    list<double> Measures;
    for (int i = 0; i < NRMEASURES; i++)
      Measures.push_back(MeasureRefreshrate(MEASURETIME));

    //build up a string of measured rates
    CStdString StrRates;
    for (list<double>::iterator it = Measures.begin(); it != Measures.end(); it++)
      StrRates.AppendFormat("%.2f ", *it);

    //get the top half of the measured rates
    Measures.sort();
    double RefreshRate = 0.0;
    int    NrMeasurements = 0;
    while (NrMeasurements < NRMEASURES / 2 && !Measures.empty())
    {
      if (Measures.back() > 0.0)
      {
        RefreshRate += Measures.back();
        NrMeasurements++;
      }
      Measures.pop_back();
    }

    if (NrMeasurements < NRMEASURES / 2)
    {
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: refreshrate measurements: %s, unable to get a good measurement",
        StrRates.c_str(), m_RefreshRate);
      return false;
    }

    RefreshRate /= NrMeasurements;
    m_RefreshRate = MathUtils::round_int(RefreshRate);

    CLog::Log(LOGDEBUG, "CVideoReferenceClock: refreshrate measurements: %s, assuming %i hertz", StrRates.c_str(), m_RefreshRate);
  }
  else
  {
    m_RefreshRate = m_PrevRefreshRate;
    if (m_RefreshRate == 23 || m_RefreshRate == 29 || m_RefreshRate == 59)
      m_RefreshRate++;

    if (m_Interlaced)
    {
      m_RefreshRate *= 2;
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: display is interlaced");
    }

    CLog::Log(LOGDEBUG, "CVideoReferenceClock: detected refreshrate: %i hertz, assuming %i hertz", m_PrevRefreshRate, (int)m_RefreshRate);
  }

  m_MissedVblanks = 0;

  return true;
}

double CVideoReferenceClock::MeasureRefreshrate(int MSecs)
{
  D3DRASTER_STATUS RasterStatus;
  int64_t          Now;
  int64_t          Target;
  int64_t          Prev;
  int64_t          AvgInterval;
  int64_t          MeasureCount;
  unsigned int     LastLine;
  int              ReturnV;

  Now = CurrentHostCounter();
  Target = Now + (m_SystemFrequency * MSecs / 1000);
  Prev = -1;
  AvgInterval = 0;
  MeasureCount = 0;

  //start measuring vblanks
  LastLine = 0;
  while(Now <= Target)
  {
    ReturnV = m_D3dDev->GetRasterStatus(0, &RasterStatus);
    Now = CurrentHostCounter();
    if (ReturnV != D3D_OK)
    {
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: GetRasterStatus returned returned %s: %s",
                DXGetErrorString(ReturnV), DXGetErrorDescription(ReturnV));
      return -1.0;
    }

    if ((RasterStatus.InVBlank && LastLine != 0) || (!RasterStatus.InVBlank && RasterStatus.ScanLine < LastLine))
    { //we got a vblank
      if (Prev != -1) //need two for a measurement
      {
        AvgInterval += Now - Prev; //save how long this vblank lasted
        MeasureCount++;
      }
      Prev = Now; //save this time for the next measurement
    }

    //save the current scanline
    if (RasterStatus.InVBlank)
      LastLine = 0;
    else
      LastLine = RasterStatus.ScanLine;

    ::Sleep(1);
  }

  if (MeasureCount < 1)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: Didn't measure any vblanks");
    return -1.0;
  }

  double fRefreshRate = 1.0 / ((double)AvgInterval / (double)MeasureCount / (double)m_SystemFrequency);

  return fRefreshRate;
}

void CVideoReferenceClock::CleanupD3D()
{
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: Cleaning up Direct3d");
  m_D3dCallback.Release();
}

#elif defined(__APPLE__)
#if !defined(__arm__)
// Called by the Core Video Display Link whenever it's appropriate to render a frame.
static CVReturn DisplayLinkCallBack(CVDisplayLinkRef displayLink, const CVTimeStamp* inNow, const CVTimeStamp* inOutputTime, CVOptionFlags flagsIn, CVOptionFlags* flagsOut, void* displayLinkContext)
{
  double fps = 60.0;

  if (inNow->videoRefreshPeriod > 0)
    fps = (double)inOutputTime->videoTimeScale / (double)inOutputTime->videoRefreshPeriod;

  // Create an autorelease pool (necessary to call into non-Obj-C code from Obj-C code)
  void* pool = Cocoa_Create_AutoReleasePool();

  CVideoReferenceClock *VideoReferenceClock = reinterpret_cast<CVideoReferenceClock*>(displayLinkContext);
  VideoReferenceClock->VblankHandler(inNow->hostTime, fps);

  // Destroy the autorelease pool
  Cocoa_Destroy_AutoReleasePool(pool);

  return kCVReturnSuccess;
}
#endif
bool CVideoReferenceClock::SetupCocoa()
{
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: setting up Cocoa");

  //init the vblank timestamp
  m_LastVBlankTime = CurrentHostCounter();
  m_MissedVblanks = 0;
  m_RefreshRate = 60;              //init the refreshrate so we don't get any division by 0 errors

  #if defined(__arm__)
  {
    g_Windowing.InitDisplayLink();
  }
  #else
  if (!Cocoa_CVDisplayLinkCreate((void*)DisplayLinkCallBack, reinterpret_cast<void*>(this)))
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: Cocoa_CVDisplayLinkCreate failed");
    return false;
  }
  else
  #endif
  {
    UpdateRefreshrate(true);
    return true;
  }
}

void CVideoReferenceClock::RunCocoa()
{
  //because cocoa has a vblank callback, we just keep sleeping until we're asked to stop the thread
  while(!m_bStop)
  {
    Sleep(1000);
  }
}

void CVideoReferenceClock::CleanupCocoa()
{
  CLog::Log(LOGDEBUG, "CVideoReferenceClock: cleaning up Cocoa");
  #if defined(__arm__)
    g_Windowing.DeinitDisplayLink();
  #else
    Cocoa_CVDisplayLinkRelease();
  #endif
}

void CVideoReferenceClock::VblankHandler(int64_t nowtime, double fps)
{
  int           NrVBlanks;
  double        VBlankTime;
  int           RefreshRate = MathUtils::round_int(fps);

  CSingleLock SingleLock(m_CritSection);

  if (RefreshRate != m_RefreshRate)
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: Detected refreshrate: %f hertz, rounding to %i hertz", fps, RefreshRate);
    m_RefreshRate = RefreshRate;
  }
  m_LastRefreshTime = CurrentHostCounter(); //can't trust m_CurrTime during refresh rate changes

  //calculate how many vblanks happened
  VBlankTime = (double)(nowtime - m_LastVBlankTime) / (double)m_SystemFrequency;
  NrVBlanks = MathUtils::round_int(VBlankTime * (double)m_RefreshRate);

  //save the timestamp of this vblank so we can calculate how many happened next time
  m_LastVBlankTime = nowtime;

  SingleLock.Leave();
  //update the vblank timestamp, update the clock and send a signal that we got a vblank
  UpdateClock(NrVBlanks, nowtime);

  SendVblankSignal();
  UpdateRefreshrate();
}
#endif

//this is called from the vblank run function
void CVideoReferenceClock::UpdateClock(int NrVBlanks, int64_t measuredVblankTime)
{
  if (NrVBlanks > 0) //update the clock with the adjusted frequency if we have any vblanks
  {
    int64_t measuredTime = measuredVblankTime;
    NrVBlanks = CorrectVBlankTracking(&measuredTime);
    if (NrVBlanks < 1) return;

    // now we should have number of vblanks to update by and corrected vblanktime, and number new misses this time round

    // update m_CurrTime, m_VblankTime, m_CurrTimeFract within a lock to ensure they update atomically for readers
    CSingleLock SingleLock(m_CritSection);
    double increment = UpdateInterval() * NrVBlanks;
    double integer   = floor(increment);
    m_CurrTime      += (int64_t)(integer + 0.5); //make sure it gets correctly converted to int

    //accumulate what we lost due to rounding in m_CurrTimeFract, then add the integer part of that to m_CurrTime
    m_CurrTimeFract += increment - integer;
    integer          = floor(m_CurrTimeFract);
    m_CurrTime      += (int64_t)(integer + 0.5);
    m_CurrTimeFract -= integer;
    m_VblankTime = measuredTime;

    SingleLock.Leave();
  }
}

double CVideoReferenceClock::UpdateInterval()
{
  return m_ClockSpeed * m_fineadjust / (double)m_RefreshRate * (double)m_SystemFrequency;
}

int CVideoReferenceClock::CorrectVBlankTracking(int64_t* measuredTime)
  // return number of vBlanks determined as should have occurred since last call,
  // and update m_VblankTime to corrected value, tune m_PVblankInterval to physical vblank duration,
  // and maintain m_VblankSampleConfidence and m_SampledVblankCount to track our progress
{
       // we are trying here to maintain a rolling cache of recent samples that are not late so that we may determine
       // accurately when the current vBlank time is late and/or that vBlanks have been missed

       // snapshot all shared global values
       CSingleLock SingleLock(m_CritSection);
       int lm_PreviousRefreshRate = m_PreviousRefreshRate;
       int64_t lm_RefreshRate = m_RefreshRate;
       int64_t lm_SystemFrequency = m_SystemFrequency;
       int lm_VblankSampleConfidence = m_VblankSampleConfidence;
       int64_t lm_PVblankInterval = m_PVblankInterval;
       //if m_RefreshChanged value is 1 then reference clock has been told to expect a refresh rate change
       //if m_RefreshChanged value is 2 then reference clock is now looking for a refresh rate change event
       int lm_RefreshChanged = m_RefreshChanged;
       SingleLock.Leave();

       //int64_t Now = CurrentHostCounter();

       int64_t measured_vBlankTime = *measuredTime; // record the measured time of vBlank event as passed to us
       int64_t vBlankTime = measured_vBlankTime;

       int i_t = 0; // the current 't' sample index
       int i_tminus1 = 0; // the 't minus 1' sample index
       int i_tminus2 = 0; // the 't minus 2' sample index
       int i_tminus3 = 0; // the 't minus 3' sample index

       int64_t error_tolerance_fine = lm_SystemFrequency * 1250 / 1000000; // 1.25ms fine error tolerance for timestamps of vblank events
       int64_t error_tolerance = lm_SystemFrequency * 2500 / 1000000; // 2.5ms reasonable error tolerance for timestamps of vblank events
       int64_t error_tolerance_max = lm_SystemFrequency * 5000 / 1000000; // 5.0ms max error tolerance for timestamps of vblank events

       // int quiteConfident = 30; // confidence level threshold to identify when we have confidence that we
                                // now have quite reliable previous samples
       // int veryConfident = 100; // confidence level threshold to identify when we have confidence that we
                                // now have very reliable previous samples
       int minConfidence = 0;  // minimum level for m_VblankSampleConfidence
       int maxConfidence = 200;  // maximum level for m_VblankSampleConfidence
       int confidence_adj_reduction = 0; //amount to reduce any up-coming confidence level adjustment by
       int confidence_adj_increase = 0; //amount to increase any up-coming confidence level adjustment by

       int vBlanks_missed = 0; // our estimate of number of vBlanks we missed

       int64_t remainder = 0; // used to store estimate duration beyond any missed vblanks
       int64_t earliness = 0; // used to store any earliness duration
                              // - which should be considered as implying previous reference was in fact late
       int64_t max_lateness = 0;  // should be used to control what we consider as the maximum lateness period to control
                                 // the maximum adjustment we will make on this run and to determine when to consider
                                 // that vblanks were missed
       bool verylate = false; // flag samples determined as very late
       bool goodsample = false; // flag sample determined as good for future reference
       int64_t elapsedVblankDuration1 = 0; // calculated duration between tminus1 and new vblank times
       int64_t elapsedVblankDuration2 = 0; // calculated duration between tminus2 and new vblank times
       int64_t elapsedVblankDuration3 = 0; // calculated duration between tminus3 and new vblank times

       // if starting over use the integer rounded refresh rate as our starting estimate of how long a vBlank should take
       // - this should be generally easily be within 0.2% of the actual (because all known non-integer rates are
       //   supposed to be within 0.1% of an integer and clocks should not be too inaccurate)
       int64_t pvblank_interval_estimate = lm_SystemFrequency / lm_RefreshRate;

       if (lm_PreviousRefreshRate != (int)lm_RefreshRate || lm_RefreshChanged > 0)  // reset our sample set if refreshrate changes are expected
       {
          m_SampledVblankCount = 0;
          m_SafeSample = 0;
          m_TotalMissedVblanks = 0; // reset out total missed count for codec screen after a refresh rate change
          lm_VblankSampleConfidence = 0;
          lm_PreviousRefreshRate = (int)lm_RefreshRate;
          m_Stable.Reset();
       }

       // bring m_VblankSampleConfidence back into range {minConfidence, maxConfidence} when required
       if (lm_VblankSampleConfidence > maxConfidence) 
          lm_VblankSampleConfidence = maxConfidence;
       else if (lm_VblankSampleConfidence < minConfidence) 
          lm_VblankSampleConfidence = minConfidence;

       // signal that the reference clock can be considered as stable if we have reached very confident
       // and set as unstable if below quite confident - in between it should remain as whatever it was prior
       if (lm_VblankSampleConfidence >= VBLANKTIMESVERYCONFIDENT)
          m_Stable.Set();
       else if (lm_VblankSampleConfidence < VBLANKTIMESQUITECONFIDENT)
          m_Stable.Reset();

       // next sample index t to populate (with evaluated vBlankTime)
       i_t = m_SampledVblankCount % VBLANKTIMESSIZE;

       // first 3 samples should be grabbed with minimal logic to give us something to work with - only thing to do
       // is try to detect misses and insert interpolated values for those until we have 3 samples
       if ( m_SampledVblankCount <= 2 )
       {
          if ( m_SampledVblankCount == 0 )
          {
             // reset the interval value to simple estimate
             lm_PVblankInterval = pvblank_interval_estimate;
             lm_VblankSampleConfidence = minConfidence;
             // don't correct vBlankTime
          }
          else
          {
             // the index of last populated
             i_tminus1 = m_SampledVblankCount - 1;
             // assume a miss if more than half a m_PVblankInterval extra
             max_lateness = lm_PVblankInterval / 2;
             elapsedVblankDuration1 = vBlankTime - m_SampledVblankTimes[i_tminus1];
             vBlanks_missed = (elapsedVblankDuration1 - max_lateness) / lm_PVblankInterval;
             remainder = elapsedVblankDuration1 - (vBlanks_missed * lm_PVblankInterval);
             for (int v = 0; v < vBlanks_missed; v++ )
             {
                 m_SampledVblankTimes[i_t] = vBlankTime - remainder - ( vBlanks_missed - v ) * lm_PVblankInterval;
                 m_SampledVblankCount++;
                 i_t = m_SampledVblankCount % VBLANKTIMESSIZE;
             }

             // don't correct vBlankTime
          }
       }
       else // we have at least 3 samples
       {
          // the index of t-1 last populated
          i_tminus1 = (m_SampledVblankCount - 1) % VBLANKTIMESSIZE;
          // the index of t-2 last populated
          i_tminus2 = (m_SampledVblankCount - 2) % VBLANKTIMESSIZE;
          // the index of t-3 last populated
          i_tminus3 = (m_SampledVblankCount - 3) % VBLANKTIMESSIZE;
          elapsedVblankDuration1 = vBlankTime - m_SampledVblankTimes[i_tminus1];
          elapsedVblankDuration2 = vBlankTime - m_SampledVblankTimes[i_tminus2];
          elapsedVblankDuration3 = vBlankTime - m_SampledVblankTimes[i_tminus3];

          // first look for earliness as this should be a useful way to detect previous lateness that we can retrospectively
          // adjust for to improve our tracking from here on
          // - note that because the m_PVblankInterval estimate can be assumed to be quite accurate even at start
          //   and any noticeable inaccuracy should only be in it being marginally too short a duration eg with
          //   1.001 speed factor), it should therefore be quite safe to interpolate to that.
          // - just try to fix last 2 samples should be adequate

          // it seems sometimes we can end up with extra vblanks (at least that is how it measures based on last 3 samples)
          // - here we should discard if we appear to have an extra one - and not update the clock
          if ( (elapsedVblankDuration3 < (int64_t)((double)lm_PVblankInterval * 2.25)) &&
              (elapsedVblankDuration2 < (int64_t)((double)lm_PVblankInterval * 1.3)) &&
              (elapsedVblankDuration1 < (int64_t)((double)lm_PVblankInterval * 0.35)) )
          {
              CLog::Log(LOGDEBUG, "CVideoReferenceClock::CorrectVBlankTracking WARNING Detected a probable extra vblank current vblanktime: %"PRId64" t-1: %"PRId64" t-2: %"PRId64" t-3: %"PRId64" interval: %"PRId64"", vBlankTime, m_SampledVblankTimes[i_tminus1], m_SampledVblankTimes[i_tminus2], m_SampledVblankTimes[i_tminus3], lm_PVblankInterval);
              return 0;
          }

          // i_tminus1
          earliness = lm_PVblankInterval - error_tolerance_fine - elapsedVblankDuration1;
          if (earliness > 0) // too short a duration suggests tminus1 was late
          {
              // correct the previous sample to be within the error tolerance from new
              m_SampledVblankTimes[i_tminus1] = vBlankTime - (lm_PVblankInterval - error_tolerance_fine);
              elapsedVblankDuration1 = vBlankTime - m_SampledVblankTimes[i_tminus1];
              // if the adjustment was large then probably a good idea to reduce the confidence level increase we might apply
              if (earliness > error_tolerance_max - error_tolerance_fine)
                 confidence_adj_reduction = 1;
          }

          // i_tminus2
          earliness = 2 * lm_PVblankInterval - error_tolerance_fine - elapsedVblankDuration2;
          if (earliness > 0) // too short a duration suggests tminus2 was late
          {
              // correct the previous sample to be within the error tolerance from new
              m_SampledVblankTimes[i_tminus2] = vBlankTime - (2 * lm_PVblankInterval - error_tolerance_fine);
              elapsedVblankDuration2 = vBlankTime - m_SampledVblankTimes[i_tminus2];
              // if the adjustment was large then probably a good idea to reduce the confidence level increase we might apply
              if (earliness > error_tolerance_max - error_tolerance_fine)
                 confidence_adj_reduction = 1;
          }

          // now look for missed samples and fill in as best we can (based on confidence level)
          // estimate the number of vblanks that should have occurred based on a max lateness
          // of 50% of a frame if we have not enough confidence that previous samples are good
          // or of 65% of a frame if we have medium confidence in previous samples
          // or of 80% of a frame if we have high confidence in previous samples
          if (lm_VblankSampleConfidence >= VBLANKTIMESVERYCONFIDENT) 
             max_lateness = lm_PVblankInterval * 80 / 100;
          else if (lm_VblankSampleConfidence >= VBLANKTIMESQUITECONFIDENT) 
             max_lateness = lm_PVblankInterval * 65 / 100;
          else max_lateness = lm_PVblankInterval / 2;

          vBlanks_missed = (int)((elapsedVblankDuration1 - max_lateness) / lm_PVblankInterval);

          remainder = elapsedVblankDuration1 - (vBlanks_missed * lm_PVblankInterval);

          if (vBlanks_missed > 0) // fill in missing with some estimated times
          {
             for (int v = 0; v < vBlanks_missed; v++)
             {
                 m_SampledVblankTimes[i_t] = vBlankTime - remainder - ( vBlanks_missed - 1 - v ) * lm_PVblankInterval;
                 m_SampledVblankCount++;
                 i_t = m_SampledVblankCount % VBLANKTIMESSIZE;
                 confidence_adj_reduction = min(2, confidence_adj_reduction + 1);
             }

             // now get deltas again!
             // the index of t-1 last populated
             i_tminus1 = (m_SampledVblankCount - 1) % VBLANKTIMESSIZE;
             // the index of t-2 last populated
             i_tminus2 = (m_SampledVblankCount - 2) % VBLANKTIMESSIZE;
             // the index of t-3 last populated
             i_tminus3 = (m_SampledVblankCount - 3) % VBLANKTIMESSIZE;
             elapsedVblankDuration1 = vBlankTime - m_SampledVblankTimes[i_tminus1];
             elapsedVblankDuration2 = vBlankTime - m_SampledVblankTimes[i_tminus2];
             elapsedVblankDuration3 = vBlankTime - m_SampledVblankTimes[i_tminus3];
          }

          // we should have now sorted out early samples and missed samples so that recorded previous samples are now
          // corrected with our own interpolation and the deltas (elapsedVblankDuration#) are adjusted accordingly

          // now we should take the deltas and based on the discrepancy from expected, and confidence level adjust the
          // new sample value accordingly and correspondingly adjust our condfidence level for next iteration


          int64_t avg_error = ( (elapsedVblankDuration1 - lm_PVblankInterval) +
                                (elapsedVblankDuration2 - (2 * lm_PVblankInterval)) +
                                (elapsedVblankDuration3 - (3 * lm_PVblankInterval)) ) / 3;

          if ( (abs(elapsedVblankDuration1 - lm_PVblankInterval) <= error_tolerance_fine) &&
               (abs(elapsedVblankDuration2 - lm_PVblankInterval * 2) <= error_tolerance_fine) &&
               (abs(elapsedVblankDuration3 - lm_PVblankInterval * 3) <= error_tolerance_fine) )
          {
              // as good tracking as we can hope for so maximum confidence increase of 3
              verylate = false;
              if (! vBlanks_missed && lm_VblankSampleConfidence >= VBLANKTIMESVERYCONFIDENT)
                 goodsample = true; // no misses and good tracking mark this as a good sample candidate
              confidence_adj_increase = 3;
          }
          else if ( (abs(elapsedVblankDuration1 - lm_PVblankInterval) <= error_tolerance_fine) &&
                    (abs(elapsedVblankDuration3 - lm_PVblankInterval * 3) <= error_tolerance_fine) )
          {
              // not quite as great tracking so do same but with less confidence increase
              verylate = false;
              confidence_adj_increase = 2;
          }
          else if ( (abs(elapsedVblankDuration1 - lm_PVblankInterval) <= error_tolerance) &&
                    (abs(elapsedVblankDuration2 - lm_PVblankInterval * 2) <= error_tolerance) &&
                    (abs(elapsedVblankDuration3 - lm_PVblankInterval * 3) <= error_tolerance) )
          {
              // ok tracking so do same but with even less confidence increase
              verylate = false;
              confidence_adj_increase = 1;
          }
          else
          {
              // ok tracking with hopefully just a late current sample so do not increase confidence and consider
              // to make larger adjustment to current if we are already confident
              verylate = true;
              confidence_adj_increase = 0;
          }

          if (! verylate)
          {
              // lets adjust current slightly based on confidence level towards estimate (adjust by up to 75% of avg_error)
              // it is possible that small delta relative early sample is here and we don't want to perform corrections for
              // that case so use avg_error positive only
              if (avg_error > 0)
                 vBlankTime -= avg_error * lm_VblankSampleConfidence * 75 / (maxConfidence * 100);
              lm_VblankSampleConfidence += max(0, confidence_adj_increase - confidence_adj_reduction);
          }
          else // verylate
          {
              // adjust current using a large number of samples (10-50) if we are not confident, or assume a single late
              // if very confident (and simply update to within error_tolerance), and if just quite confident go half-way
              // between interpolated from last 3 samples and the actual

              if ((lm_VblankSampleConfidence < VBLANKTIMESQUITECONFIDENT) && (m_SampledVblankCount > 9))
              {
                 // calculate average tick position using 10-50 previous samples (range)
                 int range = min(m_SampledVblankCount, 50);
                 range = min(range, VBLANKTIMESSIZE);
                 int i_start = (m_SampledVblankCount - range) % VBLANKTIMESSIZE; // start index

                 int64_t start_totaldelta = 0; // estimated position delta based from start sample
                                               // - positive value implying that the start sample is earlier than the average tick
                 for (int i = 1; i < range; i++)
                 {
                     start_totaldelta += m_SampledVblankTimes[(i_start + i) % VBLANKTIMESSIZE] - (m_SampledVblankTimes[i_start] + (lm_PVblankInterval * i));
                 }

                 // if all has been going to plan the number of samples and total duration should match within about
                 // half a sample (using m_PVblankInterval sized samples) - and it should not make too much difference
                 // which denominator we use for the average if we are out by 1 sample somehow.  Perhaps worth doing a
                 // a sanity check if there is a discrepancy though it would not be easy to correct the sample set at this
                 // stage anyway.
                 int64_t estimate = m_SampledVblankTimes[i_start] + (lm_PVblankInterval * range) + (start_totaldelta / (range - 1));
                 if (abs(estimate - vBlankTime) >= lm_PVblankInterval)
                 {
                    CLog::Log(LOGDEBUG, "CVideoReferenceClock::CorrectVBlankTracking WARNING samples appear to be out-of-whack (ie more than one frame discrepancy), estimate: %"PRId64" vBlankTime: %"PRId64" VblankInterval: %"PRId64"", estimate, vBlankTime, lm_PVblankInterval);
                 }

                 // adjust towards estimate by upto error_tolerance
                 if (abs(estimate - vBlankTime) < error_tolerance)
                    vBlankTime = estimate;
                 else if ( estimate > vBlankTime )
                    vBlankTime += error_tolerance;
                 else
                    vBlankTime -= error_tolerance;

                 lm_VblankSampleConfidence -= 2;

              }
              else if (lm_VblankSampleConfidence >= VBLANKTIMESVERYCONFIDENT)
              {
                 // adjust vBlankTime to be predicted within (error_tolerance_fine / 2) of t-1 if not already
                 // otherwise leave as-is and let any remaining error be resolved later via early detection code
                 if ( elapsedVblankDuration1 - lm_PVblankInterval > (error_tolerance_fine / 2) )
                    vBlankTime = m_SampledVblankTimes[i_tminus1] + lm_PVblankInterval + (error_tolerance_fine / 2);
                 // reduce confidence by 1
                 lm_VblankSampleConfidence--;
              }
              else if (lm_VblankSampleConfidence >= VBLANKTIMESQUITECONFIDENT)
              {
                 // adjust vBlankTime to be between the interpolated using last 3 samples and the actual
                 vBlankTime -= avg_error / 2;
                 // reduce confidence down by one or to one above quiteconfident level whichever is lower (to ensure
                 // 2 consecutive events like this reduce us to less than quiteconfident)
                 lm_VblankSampleConfidence = min(VBLANKTIMESQUITECONFIDENT + 1, lm_VblankSampleConfidence - 1);
              }
              else
              {
                 // don't change vBlankTime because we have not enough samples yet but reduce confidence by 1
                 lm_VblankSampleConfidence--;
              }
          }

       } // else  (  we have at least 3 samples )
       // safety/sanity check that we never move vBlankTime back further than our max_lateness value allows
       // and never nmove it forward - both should be satisfied by above code but nothing like safety net comfort!
       if (measured_vBlankTime - vBlankTime > max_lateness)
          vBlankTime = measured_vBlankTime + max_lateness;
       else if (measured_vBlankTime - vBlankTime < 0)
          vBlankTime = measured_vBlankTime;

       // store and move forward index
       m_SampledVblankTimes[i_t] = vBlankTime;
       m_SampledVblankCount++;

       // if we have not yet picked a safe sample try to pick
       if (m_SampledVblankCount > 1000 && ! m_SafeSample &&
            lm_VblankSampleConfidence >= VBLANKTIMESVERYCONFIDENT && goodsample)
       {
          m_SafeSample = m_SampledVblankCount;
          m_SafeSampleTime = vBlankTime;
       }

       // adjust m_PVblankInterval closer to the average value determined from the samples so that we can
       // converge to a more accurate value
       if (m_SafeSample && m_SampledVblankCount - m_SafeSample > 1000)
       {
          // use m_SafeSample and m_SafeSampleTime as our starting point and trust m_SampledVblankCount
           int64_t total_elapsed = m_SampledVblankTimes[i_t] - m_SafeSampleTime;
           int total_pvblanks = (int)((total_elapsed + (lm_PVblankInterval / 2)) / lm_PVblankInterval);
           if (total_pvblanks) // division by zero safety check - should never fail
           {
               int64_t delta = (total_elapsed / total_pvblanks) - lm_PVblankInterval;
               if (delta > 100)
                 lm_PVblankInterval += delta / 100 ;
               else
                 lm_PVblankInterval += delta / 10 ;
           }
       }
       else if (m_SampledVblankCount > min(20, VBLANKTIMESSIZE)) //at least 20 or VBLANKTIMESSIZE samples
       {
          int i_ts = max((m_SampledVblankCount - VBLANKTIMESSIZE), 0) % VBLANKTIMESSIZE; // start index
          if (i_t != i_ts)  // safety sanity check - should never fail
          {
              int64_t total_elapsed = m_SampledVblankTimes[i_t] - m_SampledVblankTimes[i_ts];
              int total_pvblanks = (int)((total_elapsed + (lm_PVblankInterval / 2)) / lm_PVblankInterval);
              if (total_pvblanks) // division by zero safety check - should never fail
              {
                  int64_t delta = (total_elapsed / total_pvblanks) - lm_PVblankInterval;

                  // only use this value for convergence if it passes our sanity check that it is within 0.2%
                  // of first estimate value - since we know that estimate should be at least that close to actual

                  // converge towards measured quickly at first to get close early
                  // but slowly later to help further avoid jitter

                  int integral;
                  if (m_SampledVblankCount > 2000) integral = 1000;
                  else if (m_SampledVblankCount > 1000) integral = 500;
                  else if (m_SampledVblankCount > 500) integral = 100;
                  else if (m_SampledVblankCount > 100) integral = 50;
                  else integral = 10;

                  if (abs(delta) < integral)
                     lm_PVblankInterval += delta / (integral / 5) ;
                  else if (abs(delta) < ( pvblank_interval_estimate * 2 / 10000))
                     lm_PVblankInterval += delta / integral ;
                  else if (abs(delta) < ( pvblank_interval_estimate * 2 / 1000))
                     lm_PVblankInterval += delta / 10 / integral ;
              }
          }
       } // if (m_SampledVblankCount > min(20, VBLANKTIMESSIZE))

       SingleLock.Enter();
       m_PreviousRefreshRate = lm_PreviousRefreshRate;
       m_VblankSampleConfidence = lm_VblankSampleConfidence;
       m_PVblankInterval = lm_PVblankInterval;
       SingleLock.Leave();

       // increment our total missed vblank count for codec info screen
       m_TotalMissedVblanks += vBlanks_missed;

       // finally update the passed VblankTime measuredTime and return the number of vBlanks that should have occurred
       *measuredTime = vBlankTime;

       return vBlanks_missed + 1;
}

//called from dvdclock to get the time
int64_t CVideoReferenceClock::GetTime(bool interpolated /* = true */)
{
   int64_t InterpolatedTime;
   int64_t TickTime;
   TickTime = GetTime(&InterpolatedTime);
   if (interpolated)
      return InterpolatedTime;
   else
      return TickTime;
}

int64_t CVideoReferenceClock::GetTime(int64_t* InterpolatedTime)
{
  CSingleLock SingleLock(m_CritSection);
  //when using vblank, get the time from that, otherwise use the systemclock
  if (m_UseVblank)
  {
    int64_t CurrTime = m_CurrTime;
    int vBlankSampleConfidence = m_VblankSampleConfidence;
    int64_t vBlankTime = m_VblankTime;
    int64_t pVBlankInterval = m_PVblankInterval;
    double clockTickInterval = UpdateInterval();
    int64_t refreshRate = m_RefreshRate;
    SingleLock.Leave();

    int64_t Now = CurrentHostCounter();        //get current system time
    int64_t missed_tol;
    double overshoot_control;
    int vBlanksMissed = 0;

    if ( pVBlankInterval == 0 )
       pVBlankInterval = m_SystemFrequency / refreshRate;

    // estimate the number of vblanks that should have occurred based on a tolerance
    // of 20% of a frame if we have not enough confidence that previous samples are good
    // or of 10% of a frame if we have medium confidence in previous samples
    // or of 5% of a frame if we have high confidence in previous samples

    // and define the fraction overshoot control to protect against current inaccuracies
    // sending our time past the next vblanktime yet to happen
    // of 70% of a frame if we have not enough confidence that previous samples are good
    // or of 85% of a frame if we have medium confidence in previous samples
    // or of 93% of a frame if we have high confidence in previous samples

    if ( vBlankSampleConfidence >= VBLANKTIMESVERYCONFIDENT )
    {
       missed_tol = pVBlankInterval / 20;
       overshoot_control = 0.95;
    }
    else if ( vBlankSampleConfidence >= VBLANKTIMESQUITECONFIDENT )
    {
       missed_tol = pVBlankInterval / 10;
       overshoot_control = 0.90;
    }
    else
    {
       missed_tol = pVBlankInterval / 5;
       overshoot_control = 0.80;
    }   

    vBlanksMissed = (int)((Now - vBlankTime - missed_tol) / pVBlankInterval);
    // safety check;
    vBlanksMissed = max(vBlanksMissed, 0);

    if (InterpolatedTime)
    {
        // calculate fraction of a vBlank period that has elapsed passed this last vBlank
        double fractionVBlank = (double)(Now - (vBlankTime + (vBlanksMissed * pVBlankInterval))) / (double)pVBlankInterval;
        // safety check;
        fractionVBlank = max(fractionVBlank, 0.0);
        fractionVBlank = min(fractionVBlank, 0.99);

        // use overshoot_control fraction applied to fractionVBlank to try to avoid any overshoot past next actual vBlank due
        // to inaccuracy in pVBlankInterval and previous m_VblankTime and thus avoid the clock appearing to go backwards
        // in the future
        double increment = (double)(vBlanksMissed + (overshoot_control * fractionVBlank)) * clockTickInterval;
        int64_t interpolatedTime = CurrTime + (int64_t)increment;
        if (interpolatedTime > m_LastIntTime)
           m_LastIntTime = interpolatedTime;
        *InterpolatedTime = m_LastIntTime;
    }
    return CurrTime + (int64_t)((double)vBlanksMissed * clockTickInterval);
  }
  else
  {
    int64_t CurrTime = CurrentHostCounter() + m_ClockOffset;
    if (InterpolatedTime)
      *InterpolatedTime = CurrTime;
    return CurrTime;
  }
}

//called from dvdclock to get the clock frequency
int64_t CVideoReferenceClock::GetFrequency()
{
  return m_SystemFrequency;
}

bool CVideoReferenceClock::SetSpeed(double Speed)
{
  CSingleLock SingleLock(m_CritSection);
  //dvdplayer can change the speed to fit the rereshrate
  if (m_UseVblank)
  {
    if (Speed != m_ClockSpeed)
    {
      m_ClockSpeed = Speed;
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: Clock speed %f%%", m_ClockSpeed * 100.0);
      return true;
    }
  }
  return false;
}

double CVideoReferenceClock::GetSpeed()
{
  CSingleLock SingleLock(m_CritSection);

  //dvdplayer needs to know the speed for the resampler
  if (m_UseVblank)
    return m_ClockSpeed;
  else
    return 1.0;
}

bool CVideoReferenceClock::UpdateRefreshrate(bool Forced /*= false*/)
{
  //if the graphicscontext signaled that the refreshrate changed, we check it about half a second later
  if (m_RefreshChanged == 1)
  {
     //we should consider the clock as unstable now
     CSingleLock SingleLock(m_CritSection);
     m_VblankSampleConfidence = 0;
     m_Stable.Reset();
  }

  if (m_RefreshChanged == 1 && !Forced)
  {
    // check refresh in 200ms time
    m_LastRefreshTime = CurrentHostCounter(); //can't trust m_CurrTime during refresh rate changes
    m_RefreshRateChangeStartTime = CurrentHostCounter();
    m_RefreshChanged = 2;
    return false;
  }

  //update the refreshrate about once a second, or update immediately if a forced update is required
  //or while waiting for a refresh rate change check every 200ms 
  if ((CurrentHostCounter() - m_LastRefreshTime < m_SystemFrequency && !Forced && m_RefreshChanged != 2) ||
     (CurrentHostCounter() - m_LastRefreshTime < m_SystemFrequency / 5 && m_RefreshChanged == 2))
    return false;

  if (Forced)
    m_LastRefreshTime = 0;
  else 
    m_LastRefreshTime = CurrentHostCounter(); //can't trust m_CurrTime during refresh rate changes

#if defined(HAS_GLX) && defined(HAS_XRANDR)

  //check for RandR events
  bool   GotEvent = Forced || m_RefreshChanged == 2;
  XEvent Event;
  while (XCheckTypedEvent(m_Dpy, m_RREventBase + RRScreenChangeNotify, &Event))
  {
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: Checking RandR event");
    if (Event.type == m_RREventBase + RRScreenChangeNotify)
    {
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: Received RandR event %i", Event.type);
      GotEvent = true;
    }
    XRRUpdateConfiguration(&Event);
  }

  // reset refresh changed state if not forced and we have waited 5 seconds
  if (!Forced && (m_RefreshChanged != 2 || CurrentHostCounter() - m_RefreshRateChangeStartTime > 5 * m_SystemFrequency))
    m_RefreshChanged = 0;

  if (!GotEvent) //refreshrate did not change
    return false;
  else if (!Forced)
  {
    NotifyRefreshChanged();
    m_RefreshChanged = 0; //if we got an event and not forced then assume we can reset refresh rate change wait state
  }

  //the refreshrate can be wrong on nvidia drivers, so read it from nvidia-settings when it's available
  if (m_UseNvSettings || Forced)
  {
    int NvRefreshRate;
    //if this fails we can't get the refreshrate from nvidia-settings
    m_UseNvSettings = ParseNvSettings(NvRefreshRate);

    if (m_UseNvSettings)
    {
      CSingleLock SingleLock(m_CritSection);
      m_RefreshRate = NvRefreshRate;
      return true;
    }

    CLog::Log(LOGDEBUG, "CVideoReferenceClock: Using RandR for refreshrate detection");
  }

  CSingleLock SingleLock(m_CritSection);
  m_RefreshRate = GetRandRRate();

  CLog::Log(LOGDEBUG, "CVideoReferenceClock: Detected refreshrate: %i hertz", (int)m_RefreshRate);

  return true;

#elif defined(_WIN32) && defined(HAS_DX)

  D3DDISPLAYMODE DisplayMode;
  m_D3dDev->GetDisplayMode(0, &DisplayMode);

  //0 indicates adapter default
  if (DisplayMode.RefreshRate == 0)
    DisplayMode.RefreshRate = 60;

  if (m_PrevRefreshRate != DisplayMode.RefreshRate || m_Width != DisplayMode.Width || m_Height != DisplayMode.Height ||
      m_Interlaced != g_Windowing.Interlaced() || Forced )
  {
    m_PrevRefreshRate = DisplayMode.RefreshRate;
    m_Width = DisplayMode.Width;
    m_Height = DisplayMode.Height;
    m_Interlaced = g_Windowing.Interlaced();
    return true;
  }

  return false;

#elif defined(__APPLE__)
  #if defined(__arm__)
    int RefreshRate = round(g_Windowing.GetDisplayLinkFPS() + 0.5);
  #else
    int RefreshRate = MathUtils::round_int(Cocoa_GetCVDisplayLinkRefreshPeriod());
  #endif

  if (RefreshRate != m_RefreshRate || Forced)
  {
    CSingleLock SingleLock(m_CritSection);
    CLog::Log(LOGDEBUG, "CVideoReferenceClock: Detected refreshrate: %i hertz", RefreshRate);
    m_RefreshRate = RefreshRate;
    return true;
  }
  return false;
#endif

  return false;
}

//dvdplayer needs to know the refreshrate for matching the fps of the video playing to it
int CVideoReferenceClock::GetRefreshRate(double* interval /*= NULL*/)
{
  CSingleLock SingleLock(m_CritSection);

  if (m_UseVblank)
  {
    if (interval)
      *interval = m_ClockSpeed * m_fineadjust / (double)m_RefreshRate;
    return (int)m_RefreshRate;
  }
  else
  {
    // return approxmimate vblank interval and result -1 if non using vblank based clock
    if (interval)
      *interval = 1.0 / m_RefreshRate;
    return -1;
  }
}

// return the vblank tick clock time that immediately follows the target time (only attempt reasonable accuracy for reporting purposes)
int64_t CVideoReferenceClock::GetNextTickTime(int64_t Target /* = 0 */)
{

  if (m_UseVblank)
  {
     CSingleLock SingleLock(m_CritSection);
     int64_t pVBlankInterval = m_PVblankInterval;
     if ( pVBlankInterval == 0 )
        pVBlankInterval = m_SystemFrequency / m_RefreshRate;
     double clockTickInterval = UpdateInterval();
     SingleLock.Leave();
     int64_t CurrTime = GetTime(false);

     if (Target == 0)
     {
        return CurrTime + (int64_t)clockTickInterval;
     }
     else if (Target < CurrTime) 
     {
        int64_t ticks = (CurrTime - Target) / pVBlankInterval;
        return CurrTime - (int64_t)(ticks * clockTickInterval);
     }
     else
     {
        int64_t ticks = (Target - CurrTime + pVBlankInterval) / pVBlankInterval;
        return CurrTime + (int64_t)(ticks * clockTickInterval);
     }
  }
  else
    return -1;
}
//this is called from CDVDClock::WaitAbsoluteClock, which is called from CXBMCRenderManager::WaitPresentTime
//it waits until a certain timestamp has passed, used for displaying videoframes at the correct moment (0 value targets next vblank)
//if WaitedTime supplied return the interpolated time duration we actually waited or if we did not wait, the negative or zero time between target and interpolated
//if using vblank we wait for a vblank to make clock tick past the target time if not already late based on tick time
int64_t CVideoReferenceClock::Wait(int64_t Target /* = 0 */, int64_t* WaitedTime /* = 0 */, bool lateWaitTick /* = false */)
{
  int SleepTime = 0;
  int64_t NowStart;
  int64_t Now;

  CSingleLock SingleLock(m_CritSection);
  if (m_UseVblank) //when true the vblank is used as clock source
  {
    if (Target == 0)
      Target = GetNextTickTime() - (2 * m_SystemFrequency / 1000); // next tick guess less 2ms
    if (WaitedTime)
      *WaitedTime = 0;
    int64_t CurrInterpolatedTime;
    int64_t CurrTime = GetTime(&CurrInterpolatedTime);
    int64_t StartTime = CurrInterpolatedTime;
    int64_t estimatedWait = 0;
    int64_t pVBlankInterval = m_PVblankInterval;
    if (CurrTime >= Target)
    {
      if (lateWaitTick)
      {
        // loop looking for CurrTime to increment and return that
        int64_t StartCurrTime = CurrTime;
        int i = 0;
        while (StartCurrTime == CurrTime && i < 50)
        {
          m_VblankEvent.WaitMSec(1);
          CurrTime = GetTime(&CurrInterpolatedTime);
        }
      }
      if (WaitedTime)
        *WaitedTime = std::min((int64_t)0, Target - CurrInterpolatedTime);
      return CurrTime;
    }

    estimatedWait = DurUntilNextVBlank(Target - CurrInterpolatedTime);
    if (estimatedWait > 5 * m_SystemFrequency)  // cap to 5 seconds in this unexpected case (to protect blocking application for longer than that)
      estimatedWait = 5 * m_SystemFrequency;
    SingleLock.Leave();

    NowStart = CurrentHostCounter();

    // first do normal sleep in 50ms chunks until around 1.1 vblanks before estimate
    // to avoid high too much wasted overhead when sleep is relatively long compared to pvblank interval
    int waitMs = (int)(estimatedWait * 1000 / m_SystemFrequency);
    int safeDistance = (int)(pVBlankInterval * 1100 / m_SystemFrequency);
    while (waitMs > safeDistance )
    {
      SleepTime = min(waitMs - safeDistance, 50);
      Sleep(SleepTime);
      waitMs -= SleepTime;
      if (SleepTime == 50)  // if we have slept the full 50ms then check time again - just in case unexpected events occur
      {
        CurrTime = GetTime(&CurrInterpolatedTime);
        if (CurrTime >= Target)
        {
          if (WaitedTime)
            *WaitedTime = std::max((int64_t)1, CurrInterpolatedTime - StartTime);
          return CurrTime;
        }
      }
    }
    m_VblankEvent.WaitMSec(max(waitMs + 2, 1));
    CurrTime = GetTime(&CurrInterpolatedTime);  // hopefully we have waited long enough - if not just keep checking every 2 ms for around one vblank time beyond target system time then give up (timeout) as something is wrong

     //looks like we are getting late now so we check a few more times and then timeout (returning our clock time which will be earlier than expected)
     int timeout = (int)(pVBlankInterval * 1000 / m_SystemFrequency) + 1;
     int remainMs = (int)((NowStart + estimatedWait - CurrentHostCounter()) * 1000 / m_SystemFrequency) + 1;
     while (CurrTime < Target && remainMs > -timeout)
     {
       // wait now with min wait 1 ms
       if (remainMs > 1)
         m_VblankEvent.WaitMSec(remainMs);
       else
         m_VblankEvent.WaitMSec(1);

       CurrTime = GetTime(&CurrInterpolatedTime);
       if (CurrTime < Target)
         remainMs = (int)((NowStart + estimatedWait - CurrentHostCounter()) * 1000 / m_SystemFrequency) + 1;
     }

    // return in WaitedTime if supplied our total wait
    if (WaitedTime)
      *WaitedTime = std::max((int64_t)1, CurrInterpolatedTime - StartTime);
    return CurrTime;
  }
  else
  {
    int64_t ClockOffset = m_ClockOffset;
    SingleLock.Leave();
    NowStart = CurrentHostCounter();
    if (WaitedTime)
      *WaitedTime = 0;
    //sleep until the timestamp has passed
    SleepTime = (int)((Target - (NowStart + ClockOffset)) * 1000 / m_SystemFrequency);
    if (SleepTime > 0)
      ::Sleep(SleepTime);

    Now = CurrentHostCounter();
    if (SleepTime > 0 && WaitedTime)
      *WaitedTime = Now - NowStart;
    return Now + ClockOffset;
  }
}

void CVideoReferenceClock::SendVblankSignal()
{
  m_VblankEvent.Set();
}

// try to find accurate system time of vblank that will occur after wait time with the optional condition that 
// that the chosen vblank must be at least tolerance ms after our wait would expire
int64_t CVideoReferenceClock::TimeOfNextVBlank(int64_t wait /*= 0*/, int safetyTolerance /*= 0*/)
{
    CSingleLock SingleLock(m_CritSection);
    int64_t pVBlankInterval = m_PVblankInterval;
    int64_t vBlankTime = m_VblankTime;

    if ( pVBlankInterval == 0 ) // this could become very inaccurate if this condition was really met (but it should not be met in practice)
       pVBlankInterval = m_SystemFrequency / m_RefreshRate;
    SingleLock.Leave();

    int64_t Now = CurrentHostCounter();

    int64_t ret = vBlankTime;
    int64_t threshold = safetyTolerance * m_SystemFrequency / 1000;
    while (ret < Now + wait + threshold)
        ret += pVBlankInterval;

    return ret;
}

int64_t CVideoReferenceClock::ConvertSystemDurToClockDur(int64_t duration)
{
    CSingleLock SingleLock(m_CritSection);
    int64_t pVBlankInterval = m_PVblankInterval;

    if ( pVBlankInterval == 0 )
       pVBlankInterval = m_SystemFrequency / m_RefreshRate;

    return (int64_t)((double)duration / (double)pVBlankInterval * m_ClockTickInterval);
}

// system time duration from now until when the vblank should occur immediately following an elapsed duration of clock time interval argument
int64_t CVideoReferenceClock::DurUntilNextVBlank(int64_t ClockInterval /* = 0 */) //ClockInterval in system int64_t units
{
    CSingleLock SingleLock(m_CritSection);
    int64_t pVBlankInterval = m_PVblankInterval;
    int64_t vBlankTime = m_VblankTime;
    double clockTickInterval = UpdateInterval();

    if ( pVBlankInterval == 0 )
       pVBlankInterval = m_SystemFrequency / m_RefreshRate;

    if (clockTickInterval == 0.0)
        clockTickInterval = UpdateInterval();

    SingleLock.Leave();
    int64_t Now = CurrentHostCounter();        //get current system time

    //convert clock interval to system time
    int64_t sysTimeInterval = (int64_t)((double)ClockInterval * pVBlankInterval / clockTickInterval);

    while (Now + sysTimeInterval > vBlankTime)
    {
       vBlankTime += pVBlankInterval;
    }

    return vBlankTime - CurrentHostCounter();
}

//for the codec information screen
bool CVideoReferenceClock::GetClockInfo(int& MissedVblanks, double& ClockSpeed, int& RefreshRate, double& MeasuredRefreshRate)
{
  if (m_UseVblank)
  {
    MissedVblanks = m_TotalMissedVblanks;
    ClockSpeed = m_ClockSpeed * 100.0;
    RefreshRate = (int)m_RefreshRate;
    MeasuredRefreshRate = (double)m_SystemFrequency / (double)m_PVblankInterval; // system clock based measured rate
    return true;
  }
  return false;
}

void CVideoReferenceClock::SetFineAdjust(double fineadjust)
{
  CSingleLock SingleLock(m_CritSection);
  m_fineadjust = fineadjust;
}

void CVideoReferenceClock::NotifyRefreshChanged()
{
  ThreadMessage msg = {TMSG_REFRESHCHANGED};
  g_application.getApplicationMessenger().SendMessage(msg, false);
}

CVideoReferenceClock g_VideoReferenceClock;
