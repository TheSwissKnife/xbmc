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
#include "RenderManager.h"
#include "threads/CriticalSection.h"
#include "video/VideoReferenceClock.h"
#include "utils/MathUtils.h"
#include "threads/SingleLock.h"
#include "utils/log.h"

#include "Application.h"
#include "settings/Settings.h"
#include "settings/GUISettings.h"

#ifdef _LINUX
#include "PlatformInclude.h"
#endif

#if defined(HAS_GL)
  #include "LinuxRendererGL.h"
#elif HAS_GLES == 2
  #include "LinuxRendererGLES.h"
#elif defined(HAS_DX)
  #include "WinRenderer.h"
#elif defined(HAS_SDL)
  #include "LinuxRenderer.h"
#endif

#include "RenderCapture.h"
#include "../dvdplayer/DVDCodecs/Video/VDPAU.h"

/* to use the same as player */
#include "../dvdplayer/DVDClock.h"
#include "../dvdplayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "../dvdplayer/DVDCodecs/DVDCodecUtils.h"

#define MAXPRESENTDELAY 0.500

/* at any point we want an exclusive lock on rendermanager */
/* we must make sure we don't have a graphiccontext lock */
/* these two functions allow us to step out from that lock */
/* and reaquire it after having the exclusive lock */

template<class T>
class CRetakeLock
{
public:
  CRetakeLock(CSharedSection &section, bool immidiate = true, CCriticalSection &owned = g_graphicsContext)
    : m_owned(owned)
  {
    m_count = m_owned.exit();
    m_lock  = new T(section);
    if(immidiate)
    {
      m_owned.restore(m_count);
      m_count = 0;
    }
  }
  ~CRetakeLock()
  {
    delete m_lock;
    m_owned.restore(m_count);
  }
  void Leave() { m_lock->Leave(); }
  void Enter() { m_lock->Enter(); }

private:
  T*                m_lock;
  CCriticalSection &m_owned;
  DWORD             m_count;
};

CXBMCRenderManager::CXBMCRenderManager()
{
  m_pRenderer = NULL;
  m_bPauseDrawing = false;
  m_bIsStarted = false;

  m_presentfield = FS_NONE;
  m_presenttime = 0;
  m_presentstep = PRESENT_IDLE;
  m_rendermethod = 0;
  m_presentsource = 0;
  m_presentmethod = VS_INTERLACEMETHOD_NONE;
  m_bReconfigured = false;
  m_hasCaptures = false;
//  m_pClock = 0;
}

CXBMCRenderManager::~CXBMCRenderManager()
{
  delete m_pRenderer;
  m_pRenderer = NULL;
}

/* These is based on CurrentHostCounter() */
double CXBMCRenderManager::GetPresentTime()
{
  return CDVDClock::GetAbsoluteClock(false) / DVD_TIME_BASE;
}

static double wrap(double x, double minimum, double maximum)
{
  if(x >= minimum
  && x <= maximum)
    return x;
  x = fmod(x - minimum, maximum - minimum) + minimum;
  if(x < minimum)
    x += maximum - minimum;
  if(x > maximum)
    x -= maximum - minimum;
  return x;
}

void CXBMCRenderManager::WaitPresentTime(double presenttime, bool reset_corr /* = false */)
{
  // The presenttime should reflect as best as possible the clock time when the 
  // frame about to flipped to front buffer will then display
  // For this we assume it will take one more vblank beyond the one we choose to wait for plus 
  // the delay from signal to visible of the physical display device 

  // In the case of smooth video mode (ie vblank based reference clock) we perform additional
  // tracking correction:
  //  To avoid any possible indeterminate behaviour around which clock tick value 
  //  will best match in the wait function when presenttime is very close to tick time
  //  we begin by setting our wait target time back half a tick from actual desired (so 
  //  that the desired tick will take it distinctly past), and then we try to
  //  track the delta between desired and tick value so that we can maintain this safe uniform 
  //  position possibly drifting but if so gradually and smoothly until such time as we may 
  //  then need to target a closer tick (resulting in a short or long display of a frame and if
  //  short likely subsequently a frame drop from player)

  double signal_to_view_delay = GetDisplaySignalToViewDelay();
  double frametime;
  int fps = g_VideoReferenceClock.GetRefreshRate(&frametime);
  if(fps <= 0)
  {
    /* smooth video not enabled */
    // estimate half a frametime to get to front buffer on average
    CDVDClock::WaitAbsoluteClock((presenttime - (0.5 * frametime) - signal_to_view_delay) * DVD_TIME_BASE);
    return;
  }

  bool resetbuff = false;
  // when asked to reset our wait correction position state by player do so (due to known video sync position adjustment)
  if (reset_corr)
  {
    m_presentcorr = 0.0;
    resetbuff = true;
  }
  double presentcorr = m_presentcorr;

  // fraction of frame ahead of targetwaitclock that the abs dvd clock should ideally be after our wait has completed
  double targetpos = 0.5;
  // target wait should be earlier by 1 frametime to get to frontbuffer + targetpos as well as signal to view delay
  // and finally corrected by our wait clock tracking correction factor (m_presentcorr)
  double targetwaitclock = presenttime - ((1 + targetpos) * frametime) - signal_to_view_delay;
  targetwaitclock += presentcorr * frametime;  //adjust by our accumulated correction
  //CLog::Log(LOGDEBUG, "ASB: CRenderManager::WaitPresentTime targetwaitclock: %f secs presenttime: %f frametime: %f signal_to_view_delay: %f", targetwaitclock, presenttime, frametime, signal_to_view_delay);

  // we now wait and wish our clock tick result to be targetpos out from target wait
  double clock = CDVDClock::WaitAbsoluteClock(targetwaitclock * DVD_TIME_BASE) / DVD_TIME_BASE;

  // error is number(fraction) of frames out we are from where we were trying to correct to
  double error = (clock - targetwaitclock) / frametime - targetpos;
  // abserror is number(fraction) of frames out we are from where we shuuld be without correction factor
  double abserror = error + presentcorr;
  m_presenterr = error;

  // we should be careful not too overshoot if we are getting close to a wrap boundary to avoid
  // swinging back and forward unnecessarily:

  // if wrapped abserror value changes by more 10% of frame from last then reset presentcorr and avgs
  // we should wrap error combine with m_presentcorr and if abs value greater 90% targetpos but 
  //   less than 105% move presentcorr by 1% avg error
  // else move by 5% - reset avg
  // if presentcorr approx 0 and error-wrapped approx abs(0.5) then steer by 10% in 
  //   that direction to get it quickly away from beat spot
  // if presentcorr + error-wrapped > 0.55 (we are required to make too large a forward correction) 
  //   then reset presentcorr and avgs
  // if presentcorr + error-wrapped < -0.55 targetpos then wrap presentcorr to 0.5 targetpos 
  //   to avoid any issues

  error = wrap(error, 0.0 - targetpos, 1.0 - targetpos);
  abserror = wrap(abserror, 0.0 - targetpos, 1.0 - targetpos);
  if ( (m_prevwaitabserror != DVD_NOPTS_VALUE && (fabs(abserror - m_prevwaitabserror) > 0.1)) || 
       (presentcorr > 0.55) || (fabs(presentcorr + error) > 0.6) )
  {
     // unstable fluctuations in position (implying no point in trying to target), 
     // or targetting too far forward a correction,
     // or simply too large the current error target...then reset presentcorr and error buffer
     // note: allowing the small forward correction overshoot helps to avoid hanging around 
     // the 0.5 beat zone for long enough to reduce the chances of targetting forward then
     // backward with high frequency.
     presentcorr = 0.0;
     m_prevwaitabserror = DVD_NOPTS_VALUE; //avoid next iteration using the prevabserror value
     resetbuff = true;
  }
  else if (presentcorr < -0.55)
  {
     // targetting to far backward, force it to next vblank at next iteration by wrapping
     // presentcorr straight to 0.5 and reset error buffer
     // - this avoids travelling through the troublesome beat zone
     presentcorr = 0.5;
     m_prevwaitabserror = DVD_NOPTS_VALUE; //avoid next iteration using the prevabserror value
     resetbuff = true;
  }
  else if (fabs(presentcorr) < 0.05 && fabs(error) > 0.45)
  {
     // strongly target in error direction to move clear of beat zone quickly initially
     presentcorr += 0.1 * error;
  }
  else if (fabs(presentcorr + error) > 0.45)
  {
     // we have drifted close to the beat zone so now we try to drift carefully so that 
     // we only wrap when surely required this is done with 1% adjustments of recent 
     // error average save error in the buffer
     m_errorindex = (m_errorindex + 1) % ERRORBUFFSIZE;
     m_errorbuff[m_errorindex] = error;

     //get the average error from the buffer
     double avgerror = 0.0;
     for (int i = 0; i < ERRORBUFFSIZE; i++)
           avgerror += m_errorbuff[i];
     avgerror /= ERRORBUFFSIZE;

     presentcorr = presentcorr + avgerror * 0.01;
  }
  else
  {
     // adjust by 5% of error directly
     resetbuff = true; //adjustment is large enough to warrant clearing averages
     presentcorr = presentcorr + (error * 0.05);
  }
  if (resetbuff)
  {
     memset(m_errorbuff, 0, sizeof(m_errorbuff));
  }
  // store new value back in global for access by codec info
  m_presentcorr = presentcorr;
}

CStdString CXBMCRenderManager::GetVSyncState()
{
  double avgerror = 0.0;
  for (int i = 0; i < ERRORBUFFSIZE; i++)
    avgerror += m_errorbuff[i];
  avgerror /= ERRORBUFFSIZE;

  CStdString state;
  state.Format("sh:%i lo:%i sync:%+3d%% avg:%3d%% error:%2d%%"
              , m_shortdisplaycount
              , m_longdisplaycount
              ,     MathUtils::round_int(m_presentcorr * 100)
              ,     MathUtils::round_int(avgerror      * 100)
              , abs(MathUtils::round_int(m_presenterr  * 100)));
  return state;
}

bool CXBMCRenderManager::Configure(unsigned int width, unsigned int height, unsigned int d_width, unsigned int d_height, float fps, unsigned flags, bool &bResChange)
{
  /* make sure any queued frame was fully presented */
  double timeout = m_presenttime + 0.1;
  while(m_presentstep != PRESENT_IDLE)
  {
    if(!m_presentevent.WaitMSec(100) && GetPresentTime() > timeout)
    {
      CLog::Log(LOGWARNING, "CRenderManager::Configure - timeout waiting for previous frame");
      break;
    }
  };

  CRetakeLock<CExclusiveLock> lock(m_sharedSection, false);
  if(!m_pRenderer)
  {
    CLog::Log(LOGERROR, "%s called without a valid Renderer object", __FUNCTION__);
    return false;
  }

  RESOLUTION res = GetResolution();

  bool result = m_pRenderer->Configure(width, height, d_width, d_height, fps, flags);
  if(result)
  {
    if( flags & CONF_FLAGS_FULLSCREEN )
    {
      lock.Leave();
      g_application.getApplicationMessenger().SwitchToFullscreen();
      lock.Enter();
    }
    m_pRenderer->Update(false);
    m_bIsStarted = true;
//    m_bReconfigured = true;
    m_presentstep = PRESENT_IDLE;
    m_presentevent.Set();
//    m_pClock = 0;
    m_bDrain = false;
    g_graphicsContext.AllowSetResolution(false);
  }

  bResChange = (res != m_pRenderer->GetResolution(true)) ? true : false;

  return result;
}

bool CXBMCRenderManager::IsConfigured()
{
  if (!m_pRenderer)
    return false;
  return m_pRenderer->IsConfigured();
}

void CXBMCRenderManager::Update(bool bPauseDrawing)
{
  CRetakeLock<CExclusiveLock> lock(m_sharedSection);

  m_bPauseDrawing = bPauseDrawing;
  if (m_pRenderer)
  {
    m_pRenderer->Update(bPauseDrawing);
  }

  m_presentevent.Set();
}

void CXBMCRenderManager::RenderUpdate(bool clear, DWORD flags, DWORD alpha)
{
  { CRetakeLock<CExclusiveLock> lock(m_sharedSection);
    if (!m_pRenderer)
      return;

    if (m_presentstep == PRESENT_IDLE)
      CheckNextBuffer();

    if(m_presentstep == PRESENT_FLIP)
    {
      m_overlays.Flip();
      m_pRenderer->FlipPage(m_presentsource);
      m_presentstep = PRESENT_FRAME;
      m_presentevent.Set();
    }
  }

  CSharedLock lock(m_sharedSection);

  if( m_presentmethod == VS_INTERLACEMETHOD_RENDER_WEAVE
   || m_presentmethod == VS_INTERLACEMETHOD_RENDER_WEAVE_INVERTED)
    m_pRenderer->RenderUpdate(clear, flags | RENDER_FLAG_BOTH, alpha);
  else
    m_pRenderer->RenderUpdate(clear, flags | RENDER_FLAG_LAST, alpha);

  m_overlays.Render();

  UpdatePostRenderClock();
  UpdatePreFlipClock();

  m_presentstep = PRESENT_IDLE;
  m_presentevent.Set();

}

unsigned int CXBMCRenderManager::PreInit()
{
  CRetakeLock<CExclusiveLock> lock(m_sharedSection);

  m_presentcorr = 0.0;
  m_presenterr  = 0.0;
  m_errorindex  = 0;
  memset(m_errorbuff, 0, sizeof(m_errorbuff));
  m_prevwaitabserror = 0.0;
  m_renderframedur = 0.0;
  m_renderframepts = DVD_NOPTS_VALUE;
  m_renderframeplayspeed = 0;
  m_displayframedur = 0.0;
  m_displayframepts = DVD_NOPTS_VALUE;
  m_displayframeplayspeed = 0;
  m_displayframeestclock = DVD_NOPTS_VALUE;
  m_prevdisplayframeplayspeed = 0;
  m_prevdisplayframedur = 0.0;
  m_prevdisplayframeestclock = DVD_NOPTS_VALUE;
  m_displayrefreshdur = 0.0;
  m_refdisplayframepts = DVD_NOPTS_VALUE;
  m_refdisplayframeclock = DVD_NOPTS_VALUE;
  m_refdisplayframeplayspeed = 0;
  m_flipasync = true; 
  m_preflipclock = DVD_NOPTS_VALUE;
  m_postflipclock = DVD_NOPTS_VALUE;
  m_postrenderclock = DVD_NOPTS_VALUE;
  m_shortdisplaycount = 0;
  m_longdisplaycount = 0;

  m_bIsStarted = false;
  m_bPauseDrawing = false;
  if (!m_pRenderer)
  {
#if defined(HAS_GL)
    m_pRenderer = new CLinuxRendererGL();
#elif HAS_GLES == 2
    m_pRenderer = new CLinuxRendererGLES();
#elif defined(HAS_DX)
    m_pRenderer = new CWinRenderer();
#elif defined(HAS_SDL)
    m_pRenderer = new CLinuxRenderer();
#endif
  }

  return m_pRenderer->PreInit();
}

void CXBMCRenderManager::UnInit()
{
  CRetakeLock<CExclusiveLock> lock(m_sharedSection);

  m_bIsStarted = false;

  m_overlays.Flush();

  CLog::Log(LOGNOTICE, "------------------------- uninit");

  // free renderer resources.
  // TODO: we may also want to release the renderer here.
  if (m_pRenderer)
    m_pRenderer->UnInit();
}

void CXBMCRenderManager::SetReconfigured()
{
  m_bReconfigured = true;
  g_graphicsContext.AllowSetResolution(true);
}

void CXBMCRenderManager::SetupScreenshot()
{
  CSharedLock lock(m_sharedSection);
  if (m_pRenderer)
    m_pRenderer->SetupScreenshot();
}

CRenderCapture* CXBMCRenderManager::AllocRenderCapture()
{
  return new CRenderCapture;
}

void CXBMCRenderManager::ReleaseRenderCapture(CRenderCapture* capture)
{
  CSingleLock lock(m_captCritSect);

  RemoveCapture(capture);

  //because a CRenderCapture might have some gl things allocated, it can only be deleted from app thread
  if (g_application.IsCurrentThread())
  {
    delete capture;
  }
  else
  {
    capture->SetState(CAPTURESTATE_NEEDSDELETE);
    m_captures.push_back(capture);
  }

  if (!m_captures.empty())
    m_hasCaptures = true;
}

void CXBMCRenderManager::Capture(CRenderCapture* capture, unsigned int width, unsigned int height, int flags)
{
  CSingleLock lock(m_captCritSect);

  RemoveCapture(capture);

  capture->SetState(CAPTURESTATE_NEEDSRENDER);
  capture->SetUserState(CAPTURESTATE_WORKING);
  capture->SetWidth(width);
  capture->SetHeight(height);
  capture->SetFlags(flags);
  capture->GetEvent().Reset();

  if (g_application.IsCurrentThread())
  {
    if (flags & CAPTUREFLAG_IMMEDIATELY)
    {
      //render capture and read out immediately
      RenderCapture(capture);
      capture->SetUserState(capture->GetState());
      capture->GetEvent().Set();
    }

    if ((flags & CAPTUREFLAG_CONTINUOUS) || !(flags & CAPTUREFLAG_IMMEDIATELY))
    {
      //schedule this capture for a render and readout
      m_captures.push_back(capture);
    }
  }
  else
  {
    //schedule this capture for a render and readout
    m_captures.push_back(capture);
  }

  if (!m_captures.empty())
    m_hasCaptures = true;
}

void CXBMCRenderManager::ManageCaptures()
{
  //no captures, return here so we don't do an unnecessary lock
  if (!m_hasCaptures)
    return;

  CSingleLock lock(m_captCritSect);

  std::list<CRenderCapture*>::iterator it = m_captures.begin();
  while (it != m_captures.end())
  {
    CRenderCapture* capture = *it;

    if (capture->GetState() == CAPTURESTATE_NEEDSDELETE)
    {
      delete capture;
      it = m_captures.erase(it);
      continue;
    }

    if (capture->GetState() == CAPTURESTATE_NEEDSRENDER)
      RenderCapture(capture);
    else if (capture->GetState() == CAPTURESTATE_NEEDSREADOUT)
      capture->ReadOut();

    if (capture->GetState() == CAPTURESTATE_DONE || capture->GetState() == CAPTURESTATE_FAILED)
    {
      //tell the thread that the capture is done or has failed
      capture->SetUserState(capture->GetState());
      capture->GetEvent().Set();

      if (capture->GetFlags() & CAPTUREFLAG_CONTINUOUS)
      {
        capture->SetState(CAPTURESTATE_NEEDSRENDER);

        //if rendering this capture continuously, and readout is async, render a new capture immediately
        if (capture->IsAsync() && !(capture->GetFlags() & CAPTUREFLAG_IMMEDIATELY))
          RenderCapture(capture);

        it++;
      }
      else
      {
        it = m_captures.erase(it);
      }
    }
    else
    {
      it++;
    }
  }

  if (m_captures.empty())
    m_hasCaptures = false;
}

void CXBMCRenderManager::RenderCapture(CRenderCapture* capture)
{
  CSharedLock lock(m_sharedSection);
  if (!m_pRenderer || !m_pRenderer->RenderCapture(capture))
    capture->SetState(CAPTURESTATE_FAILED);
}

void CXBMCRenderManager::RemoveCapture(CRenderCapture* capture)
{
  //remove this CRenderCapture from the list
  std::list<CRenderCapture*>::iterator it;
  while ((it = find(m_captures.begin(), m_captures.end(), capture)) != m_captures.end())
    m_captures.erase(it);
}

void CXBMCRenderManager::FlipPage(volatile bool& bStop, double timestamp /* = 0LL*/, int source /*= -1*/, EFIELDSYNC sync /*= FS_NONE*/)
{
  if(timestamp - GetPresentTime() > MAXPRESENTDELAY)
    timestamp =  GetPresentTime() + MAXPRESENTDELAY;

  /* can't flip, untill timestamp */
  if(!g_graphicsContext.IsFullScreenVideo())
    WaitPresentTime(timestamp);

  /* make sure any queued frame was fully presented */
  double timeout = m_presenttime + 1.0;
  while(m_presentstep != PRESENT_IDLE && !bStop)
  {
    if(!m_presentevent.WaitMSec(100) && GetPresentTime() > timeout && !bStop)
    {
      CLog::Log(LOGWARNING, "CRenderManager::FlipPage - timeout waiting for previous frame");
      return;
    }
  };

  if(bStop)
    return;

  { CRetakeLock<CExclusiveLock> lock(m_sharedSection);
    if(!m_pRenderer) return;

    m_presenttime  = timestamp;
    m_presentfield = sync;
    m_presentstep  = PRESENT_FLIP;
    m_presentsource = source;
    m_presentmethod = g_settings.m_currentVideoSettings.m_InterlaceMethod;

    /* select render method for auto */
    if(m_presentmethod == VS_INTERLACEMETHOD_AUTO)
    {
      if(m_presentfield == FS_NONE)
        m_presentmethod = VS_INTERLACEMETHOD_NONE;
      else if(m_pRenderer->Supports(VS_INTERLACEMETHOD_RENDER_BOB))
        m_presentmethod = VS_INTERLACEMETHOD_RENDER_BOB;
      else
        m_presentmethod = VS_INTERLACEMETHOD_NONE;
    }

    /* default to odd field if we want to deinterlace and don't know better */
    if(m_presentfield == FS_NONE && m_presentmethod != VS_INTERLACEMETHOD_NONE)
      m_presentfield = FS_TOP;

    /* invert present field if we have one of those methods */
    if( m_presentmethod == VS_INTERLACEMETHOD_RENDER_BOB_INVERTED
     || m_presentmethod == VS_INTERLACEMETHOD_RENDER_WEAVE_INVERTED )
    {
      if( m_presentfield == FS_BOT )
        m_presentfield = FS_TOP;
      else
        m_presentfield = FS_BOT;
    }
  }

  g_application.NewFrame();
  /* wait untill render thread have flipped buffers */
  timeout = m_presenttime + 1.0;
  while(m_presentstep == PRESENT_FLIP && !bStop)
  {
    if(!m_presentevent.WaitMSec(100) && GetPresentTime() > timeout && !bStop)
    {
      CLog::Log(LOGWARNING, "CRenderManager::FlipPage - timeout waiting for flip to complete");
      return;
    }
  }
}

float CXBMCRenderManager::GetMaximumFPS()
{
  float fps;

  if (g_guiSettings.GetInt("videoscreen.vsync") != VSYNC_DISABLED)
  {
    fps = (float)g_VideoReferenceClock.GetRefreshRate();
    if (fps <= 0) fps = g_graphicsContext.GetFPS();
  }
  else
    fps = 1000.0f;

  return fps;
}

void CXBMCRenderManager::Present()
{
  { CRetakeLock<CExclusiveLock> lock(m_sharedSection);
    if (!m_pRenderer)
      return;

    if (m_presentstep == PRESENT_IDLE)
      CheckNextBuffer();

    if(m_presentstep == PRESENT_FLIP)
    {
      m_overlays.Flip();
      m_pRenderer->FlipPage(m_presentsource);
      m_presentstep = PRESENT_FRAME;
      m_presentevent.Set();
    }
  }

  CSharedLock lock(m_sharedSection);

  if     ( m_presentmethod == VS_INTERLACEMETHOD_RENDER_BOB
        || m_presentmethod == VS_INTERLACEMETHOD_RENDER_BOB_INVERTED)
    PresentBob();
  else if( m_presentmethod == VS_INTERLACEMETHOD_RENDER_WEAVE
        || m_presentmethod == VS_INTERLACEMETHOD_RENDER_WEAVE_INVERTED)
    PresentWeave();
  else if( m_presentmethod == VS_INTERLACEMETHOD_RENDER_BLEND )
    PresentBlend();
  else
    PresentSingle();

  m_presentevent.Set();

  m_overlays.Render();

  lock.Leave();

  UpdatePostRenderClock();
  /* wait for this present to be valid */
  if(g_graphicsContext.IsFullScreenVideo())
    WaitPresentTime(m_presenttime);
  UpdatePreFlipClock();

//  m_presentevent.Set();
}

/* simple present method */
void CXBMCRenderManager::PresentSingle()
{
  CSingleLock lock(g_graphicsContext);

  m_pRenderer->RenderUpdate(true, 0, 255);
  m_presentstep = PRESENT_IDLE;
}

/* new simpler method of handling interlaced material, *
 * we just render the two fields right after eachother */
void CXBMCRenderManager::PresentBob()
{
  CSingleLock lock(g_graphicsContext);

  if(m_presentstep == PRESENT_FRAME)
  {
    if( m_presentfield == FS_BOT)
      m_pRenderer->RenderUpdate(true, RENDER_FLAG_BOT, 255);
    else
      m_pRenderer->RenderUpdate(true, RENDER_FLAG_TOP, 255);
    m_presentstep = PRESENT_FRAME2;
    g_application.NewFrame();
  }
  else
  {
    if( m_presentfield == FS_TOP)
      m_pRenderer->RenderUpdate(true, RENDER_FLAG_BOT, 255);
    else
      m_pRenderer->RenderUpdate(true, RENDER_FLAG_TOP, 255);
    m_presentstep = PRESENT_IDLE;
  }
}

void CXBMCRenderManager::PresentBlend()
{
  CSingleLock lock(g_graphicsContext);

  if( m_presentfield == FS_BOT )
  {
    m_pRenderer->RenderUpdate(true, RENDER_FLAG_BOT | RENDER_FLAG_NOOSD, 255);
    m_pRenderer->RenderUpdate(false, RENDER_FLAG_TOP, 128);
  }
  else
  {
    m_pRenderer->RenderUpdate(true, RENDER_FLAG_TOP | RENDER_FLAG_NOOSD, 255);
    m_pRenderer->RenderUpdate(false, RENDER_FLAG_BOT, 128);
  }
  m_presentstep = PRESENT_IDLE;
}

/* renders the two fields as one, but doing fieldbased *
 * scaling then reinterlaceing resulting image         */
void CXBMCRenderManager::PresentWeave()
{
  CSingleLock lock(g_graphicsContext);

  m_pRenderer->RenderUpdate(true, RENDER_FLAG_BOTH, 255);
  m_presentstep = PRESENT_IDLE;
}

void CXBMCRenderManager::Recover()
{
//#if defined(HAS_GL) && !defined(TARGET_DARWIN)
//  CLog::Log(LOGERROR, "CXBMCRenderManager::Recover");
//  //glFlush(); // attempt to have gpu done with pixmap and vdpau
//  glFinish();
//#endif
//  UnInit();
}

void CXBMCRenderManager::UpdateResolution()
{
  if (m_bReconfigured)
  {
    CLog::Log(LOGNOTICE, "--------------- rendermanager update res");
    CRetakeLock<CExclusiveLock> lock(m_sharedSection);
    if (g_graphicsContext.IsFullScreenVideo() && g_graphicsContext.IsFullScreenRoot())
    {
      RESOLUTION res = GetResolution();
      g_graphicsContext.SetVideoResolution(res);
    }
    m_bReconfigured = false;
  }
}


//int CXBMCRenderManager::AddVideoPicture(DVDVideoPicture& pic, int source, double presenttime, EFIELDSYNC sync, CDVDClock *clock, bool &late)
int CXBMCRenderManager::AddVideoPicture(DVDVideoPicture& pic, int source, double pts, double presenttime, EFIELDSYNC sync, int playspeed, double framedur, bool vclockresync /* = false */)
{
  CSharedLock lock(m_sharedSection);
  if (!m_pRenderer)
    return -1;

#ifdef HAS_DX
  m_pRenderer->AddProcessor(&pic);
#endif
//  m_pClock = clock;

  YV12Image image;
  source = m_pRenderer->GetNextFreeBufferIndex();
  if (source < 0)
  {
    CLog::Log(LOGERROR, "CXBMCRenderManager::AddVideoPicture - error getting next buffer");
    return -1;
  }

  int index = m_pRenderer->GetImage(&image, source);

  if(index < 0)
    return index;

  if(pic.format == DVDVideoPicture::FMT_YUV420P)
  {
    CDVDCodecUtils::CopyPicture(&image, &pic);
  }
  else if(pic.format == DVDVideoPicture::FMT_NV12)
  {
    CDVDCodecUtils::CopyNV12Picture(&image, &pic);
  }
  else if(pic.format == DVDVideoPicture::FMT_YUY2
       || pic.format == DVDVideoPicture::FMT_UYVY)
  {
    CDVDCodecUtils::CopyYUV422PackedPicture(&image, &pic);
  }
#ifdef HAVE_LIBVDPAU
  else if(pic.format == DVDVideoPicture::FMT_VDPAU || pic.format == DVDVideoPicture::FMT_VDPAU_420)
  {
     m_pRenderer->AddProcessor(pic.vdpau);
     if (pic.vdpau)
        pic.vdpau->Present(index);
  }
//      if (pic.vdpau)
//       pic.vdpau->Present(index);
//    m_pRenderer->AddProcessor(pic.vdpau);
#endif
#ifdef HAVE_LIBOPENMAX
  else if(pic.format == DVDVideoPicture::FMT_OMXEGL)
    m_pRenderer->AddProcessor(pic.openMax, &pic);
#endif
#ifdef HAVE_VIDEOTOOLBOXDECODER
  else if(pic.format == DVDVideoPicture::FMT_CVBREF)
    m_pRenderer->AddProcessor(pic.vtb, &pic);
#endif
#ifdef HAVE_LIBVA
  else if(pic.format == DVDVideoPicture::FMT_VAAPI)
    m_pRenderer->AddProcessor(*pic.vaapi);
#endif

  // set pts and sync
  *image.pPresenttime = presenttime;
  *image.pPts = pts;
  *image.pPlaySpeed = playspeed;
  *image.pFrameDur = framedur;
  *image.pVClockResync = vclockresync;
  *image.pSync = sync;

  // upload texture
  m_pRenderer->Upload(index);

  m_pRenderer->ReleaseImage(index, false);

  // signal new frame to application
    CLog::Log(LOGDEBUG, "ASB: CRenderManager::AddVideoPicture about to g_application.NewFrame() pts: %f index: %i", pts, index);
  g_application.NewFrame();

  // signal lateness to player
//  late = m_late ? true : false;
//  m_late = false;

  return index;
}

int CXBMCRenderManager::WaitForBuffer(volatile bool& bStop)
{
  CSharedLock lock(m_sharedSection);
  if (!m_pRenderer)
    return -1;

  double timeout = GetPresentTime() + 0.05;
  while(!m_pRenderer->HasFreeBuffer() && !bStop)
  {
    lock.Leave();
    m_flipEvent.WaitMSec(50);
    if(GetPresentTime() > timeout && !bStop)
    {
//      m_pRenderer->LogBuffers();
      CLog::Log(LOGWARNING, "CRenderManager::WaitForBuffer - timeout waiting buffer");
      return -1;
    }
    lock.Enter();
  }
  return 1;
}

void CXBMCRenderManager::NotifyFlip()
{
  if (!m_pRenderer)
    return;

  // first update our pts tracking:
  // so assuming for now the following state:  
  //    async flipping (eg no gLfinish())
  //    && using vsync 
  //    && video reference clock 
  //    && not too high refresh rate eg vb duration > ~8ms
  // then
  //    if postflipclock - preflipclock >= 6ms && postflipclock > vb_after_preflipclock 
  //         assume displayclock == vb_after_postflipclock
  //    if postflipclock < vb_after_preflipclock && vb_after_preflipclock - postrenderclock > min(0.5 * vb_duration, 8ms)
  //         assume displayclock == vb_after_postflipclock
  //    else estimate_displayclock only as say vb_after_postflipclock 
  //         (to be used for estimating display pts and only if we have not updated displayclock recently etc)
  // TODO: async mode - will need to adjust the functions down to PresentRenderImpl() to pass this through 
  // TODO: ignore for now for simplicity non-fullscreen consideration as accuracy probably not so important...
  //       but what about non full screen root (windowed mode)?
  
  UpdatePostFlipClock();
  bool skiprenderinfocalc = false;
  if (m_preflipclock == DVD_NOPTS_VALUE || m_postflipclock == DVD_NOPTS_VALUE || m_postrenderclock == DVD_NOPTS_VALUE)
  {
    //CLog::Log(LOGERROR, "ASB: CRenderManager::NotifyFlip Problem with unexpected values for m_preflipclock: %f m_postflipclock: %f m_postrenderclock: %f", m_preflipclock, m_postflipclock, m_postrenderclock);
    skiprenderinfocalc = true;
    // Should we be flipping m_pRenderer in this case...probably not
    //return;
  }

  if (!skiprenderinfocalc)
  {
  CSharedLock lock(m_sharedDisplayInfoSection); 
  //copy current to prev values before updating
  double prevdisplayframeestclock = m_displayframeestclock;
  int prevdisplayframeplayspeed = m_displayframeplayspeed;
  double prevdisplayframedur = m_displayframedur;   

  double prevdisplayframeestclock2 = m_prevdisplayframeestclock;
  int prevdisplayframeplayspeed2 = m_prevdisplayframeplayspeed;
  double prevdisplayframedur2 = m_prevdisplayframedur;   

  lock.Leave();

  // get the vblank clock tick time following m_postflipclock value
  double clocktickafterpostflipclock = CDVDClock::GetNextAbsoluteClockTick(m_postflipclock * DVD_TIME_BASE) / DVD_TIME_BASE;
  double signal_to_view_delay = GetDisplaySignalToViewDelay(); 
  double displayrefreshdur;
  int fps = g_VideoReferenceClock.GetRefreshRate(&displayrefreshdur); //fps == 0 or less assume no vblank based reference clock

  CExclusiveLock lock1(m_sharedDisplayInfoSection); //now we can update the info variables.
      
  //m_flipasync = true; //TODO: get application to tell render manager if we are async or not
  m_displayframepts = m_renderframepts;
  m_displayframeplayspeed = m_renderframeplayspeed;
  m_displayframedur = m_renderframedur;  
  m_displayrefreshdur = displayrefreshdur;
  m_prevdisplayframeestclock = prevdisplayframeestclock;
  m_prevdisplayframeplayspeed = prevdisplayframeplayspeed;
  m_prevdisplayframedur = prevdisplayframedur;

  // now we need to establish values for m_displayframeestclock, and if 
  // possible m_refdisplayframeclock, m_refdisplayframeplayspeed, m_refdisplayframeplaypts 
  if (fps > 0 && m_displayrefreshdur > 0.001) //vblank based smooth video and plausible refresh duration
  {
     if (m_flipasync) 
     {
        // 6ms overrun across a vblank to signify it must have waited for a previous flip to complete
        double flipclockoverrundur = 0.006;
        // min of {8ms, half tick duration} safe allowance for outstanding render opengl 
        // commands to complete before a vblank
        double renderallowance = std::min(0.008, 0.5 * m_displayrefreshdur);

        // the vblank clock tick time following m_preflipclock value 
        double clocktickafterpreflipclock = clocktickafterpostflipclock;
        while (clocktickafterpreflipclock > m_preflipclock)
               clocktickafterpreflipclock -= m_displayrefreshdur;  //reduce to just before
        clocktickafterpreflipclock = clocktickafterpreflipclock + m_displayrefreshdur;  //finally take it forward one tick
        // standard estimate is that all is well and frame will be on front buffer at vblank after flip
        m_displayframeestclock = clocktickafterpostflipclock + signal_to_view_delay;
 
        if ( (m_postflipclock > clocktickafterpreflipclock && 
             clocktickafterpostflipclock - clocktickafterpreflipclock > flipclockoverrundur) ||
                (m_postflipclock < clocktickafterpreflipclock && 
                 clocktickafterpreflipclock - m_postrenderclock > renderallowance) )
        {
           m_refdisplayframeclock = m_displayframeestclock;
           m_refdisplayframepts = m_displayframepts;
           m_refdisplayframeplayspeed = m_renderframeplayspeed;
        }
     }
     else if (!m_flipasync)
     {
        // just assume the vblank before postflip clock is displayclock
        m_refdisplayframeclock = clocktickafterpostflipclock - m_displayrefreshdur + signal_to_view_delay;
        m_refdisplayframepts = m_displayframepts;
        m_refdisplayframeplayspeed = m_renderframeplayspeed;
        m_displayframeestclock = m_refdisplayframeclock;         
     }
  }
  else
  {
     // just estimate this from postflipclock + half display duration + signal to view delay
     m_displayframeestclock = m_postflipclock + (m_displayrefreshdur / 2) ;
  }

  //if we didn't change speed or duration, and at normal speed then estimate if we got long/short display frames
  if (prevdisplayframeplayspeed == m_displayframeplayspeed && 
      m_displayframeplayspeed == DVD_PLAYSPEED_NORMAL && prevdisplayframedur == m_displayframedur) 
  {  
     double displayclockelapsed = m_displayframeestclock - prevdisplayframeestclock;
     double displayclockelapsed2 = m_displayframeestclock - prevdisplayframeestclock2;
     if (displayclockelapsed > m_displayframedur + 0.5 * m_displayrefreshdur)
        m_longdisplaycount++;
     else if (displayclockelapsed < m_displayframedur - 0.5 * m_displayrefreshdur && 
              displayclockelapsed > 0.9 * m_displayrefreshdur)
        m_shortdisplaycount++;
     else if (prevdisplayframeestclock2 != DVD_NOPTS_VALUE &&
              prevdisplayframeplayspeed2 == m_displayframeplayspeed &&
              prevdisplayframedur2 == m_displayframedur &&
              displayclockelapsed2 > m_displayframedur * 2 * 0.95 && 
              displayclockelapsed2 < m_displayframedur * 2 * 1.05 &&
              displayclockelapsed <= 0.9 * m_displayrefreshdur)
        m_longdisplaycount--; //previous estimate would have incremented longdisplay count and is now 
                              //understood to be more likely to be wrong so reverse it
    CLog::Log(LOGDEBUG, "ASB: CRenderManager::NotifyFlip displayclockelapsed: %f m_displayframeestclock: %f prevdisplayframeestclock: %f clocktickafterpostflipclock: %f m_postflipclock: %f", displayclockelapsed, m_displayframeestclock, prevdisplayframeestclock, clocktickafterpostflipclock, m_postflipclock);
  }
  lock1.Leave();
}

  CRetakeLock<CExclusiveLock> lock2(m_sharedSection);

  m_pRenderer->NotifyFlip();
  m_flipEvent.Set();
}

void CXBMCRenderManager::UpdatePostRenderClock()
{
  m_postrenderclock = CDVDClock::GetAbsoluteClock(true) / DVD_TIME_BASE; 
} 

void CXBMCRenderManager::UpdatePreFlipClock()
{
  m_preflipclock = CDVDClock::GetAbsoluteClock(true) / DVD_TIME_BASE;
} 
 
void CXBMCRenderManager::UpdatePostFlipClock()
{
  m_postflipclock = CDVDClock::GetAbsoluteClock(true) / DVD_TIME_BASE;
}  

double CXBMCRenderManager::GetDisplaySignalToViewDelay()
{
  // the delay between when the display signal begins transmitting to when it 
  // is considered viewed/perceived.
  // TODO: function to get from advanced settings reflecting the physical display 
  //       device and video signal method and allowing multiples of vblank duration
  //       eg possibly half vb is good estimate for analog display, but with digital 1 vb + internal processing delay)

  // for now just go with half refresh duration
  return (m_displayrefreshdur / 2);
}

double CXBMCRenderManager::GetDisplayDelay()
{
  // estimate of clock time to take an output frame, render it then deliver it to be visible on display
  // This function allows the player to prepare relative clock-to-pts delta at initial resync

  // - assume 50ms for render + signal to view delay
  return 0.05 + GetDisplaySignalToViewDelay();
}

double CXBMCRenderManager::GetCurrentDisplayPts(int& playspeed)
{
  double samplepts;
  double sampleclock;
  CSharedLock lock(m_sharedDisplayInfoSection);
  playspeed = m_displayframeplayspeed;
  double clock = CDVDClock::GetAbsoluteClock(true) / DVD_TIME_BASE;
  if (m_displayframepts == DVD_NOPTS_VALUE)
     return DVD_NOPTS_VALUE; //tell caller we don't know

  //use estimates if available if no reference value OR playspeed has changed OR reference value is more than 2 seconds old
  if (m_refdisplayframeclock == DVD_NOPTS_VALUE || 
      playspeed != m_refdisplayframeplayspeed || clock - m_refdisplayframeclock > 2.0)
  {        
     samplepts = m_displayframepts;
     sampleclock = m_displayframeestclock;
  }
  else
  {
     samplepts = m_refdisplayframepts;
     sampleclock = m_refdisplayframeclock;
  }

  if ( (playspeed == DVD_PLAYSPEED_PAUSE && clock >= m_displayframeestclock) ||
       (clock >= m_displayframeestclock + m_displayrefreshdur) ) 
     return m_displayframepts;
  else 
     return samplepts - (playspeed / DVD_PLAYSPEED_NORMAL) * (sampleclock - clock);
}

// despite the name this function will also prepare state for render flip too
void CXBMCRenderManager::CheckNextBuffer()
{
//  if(!m_pRenderer || !m_pClock) return;
  if(!m_pRenderer) return;

  YV12Image image;
  int source = m_pRenderer->GetCurrentBufferIndex();
  if (source == -1)
    return;

  int index = m_pRenderer->GetImage(&image, source);

  // calculate render time
//  double iPlayingClock, iCurrentClock, iSleepTime, iPresentTime;
//  iPlayingClock = m_pClock->GetClock(iCurrentClock, false);
//  iSleepTime = *image.pPresenttime - iPlayingClock;
//  iPresentTime = iCurrentClock + iSleepTime;

//  if (iSleepTime <= 0)
//    m_late = true;

  double timestamp = *image.pPresenttime/DVD_TIME_BASE;
  if(timestamp - GetPresentTime() > MAXPRESENTDELAY)
    timestamp =  GetPresentTime() + MAXPRESENTDELAY;

  m_renderframepts = *image.pPts; //from image
  m_renderframeplayspeed = *image.pPlaySpeed; //from image
  m_renderframedur = *image.pFrameDur/DVD_TIME_BASE; //from image

  if(g_graphicsContext.IsFullScreenVideo()
      || timestamp <= GetPresentTime())
  {
    m_presenttime  = timestamp;
    bool reset = image.pVClockResync;
    m_presentfield = *image.pSync;
    m_presentstep  = PRESENT_FLIP;
    m_presentsource = -1;
    m_presentmethod = g_settings.m_currentVideoSettings.m_InterlaceMethod;

    /* select render method for auto */
    if(m_presentmethod == VS_INTERLACEMETHOD_AUTO)
    {
      if(m_presentfield == FS_NONE)
        m_presentmethod = VS_INTERLACEMETHOD_NONE;
      else if(m_pRenderer->Supports(VS_INTERLACEMETHOD_RENDER_BOB))
        m_presentmethod = VS_INTERLACEMETHOD_RENDER_BOB;
      else
        m_presentmethod = VS_INTERLACEMETHOD_NONE;
    }

    /* default to odd field if we want to deinterlace and don't know better */
    if(m_presentfield == FS_NONE && m_presentmethod != VS_INTERLACEMETHOD_NONE)
      m_presentfield = FS_TOP;

    /* invert present field if we have one of those methods */
    if( m_presentmethod == VS_INTERLACEMETHOD_RENDER_BOB_INVERTED
     || m_presentmethod == VS_INTERLACEMETHOD_RENDER_WEAVE_INVERTED )
    {
      if( m_presentfield == FS_BOT )
        m_presentfield = FS_TOP;
      else
        m_presentfield = FS_BOT;
    }
  }

  m_pRenderer->ReleaseImage(index, false);
}

void CXBMCRenderManager::ReleaseProcessor()
{
  CRetakeLock<CExclusiveLock> lock(m_sharedSection);
  if (!m_pRenderer)
    return;

  m_pRenderer->ReleaseProcessor();
}

bool CXBMCRenderManager::Drain()
{
  m_bDrain = true;
  if(!m_pRenderer)
    return true;

  int index = m_pRenderer->GetCurrentBufferIndex();
  if (index == -1)
    return true;

  g_application.NewFrame();
  return false;
}
