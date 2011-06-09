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

/* to use the same as player */
#include "../dvdplayer/DVDClock.h"

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
    m_count = ExitCriticalSection(m_owned);
    m_lock  = new T(section);
    if(immidiate)
    {
      RestoreCriticalSection(m_owned, m_count);
      m_count = 0;
    }
  }
  ~CRetakeLock()
  {
    delete m_lock;
    RestoreCriticalSection(m_owned, m_count);
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
  m_vdpauflip = PRESENT_IDLE;
}

CXBMCRenderManager::~CXBMCRenderManager()
{
  delete m_pRenderer;
  m_pRenderer = NULL;
}

/* These is based on CurrentHostCounter() */
double CXBMCRenderManager::GetPresentTime()
{
  return CDVDClock::GetAbsoluteClock() / DVD_TIME_BASE;
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

void CXBMCRenderManager::WaitPresentTime(double presenttime)
{

  double frametime; // video reference clock tick increment (each display refresh will move our clock forward by this amount in seconds)
  int fps = g_VideoReferenceClock.GetRefreshRate(&frametime);
  if(fps <= 0)
  {
    /* smooth video not enabled */
    CDVDClock::WaitAbsoluteClock(presenttime * DVD_TIME_BASE);
    return;
  }

  double presentcorr = m_presentcorr;

/* add back in when we have sorted out lateness algorithm
  // when asked to reset our wait position state by player do so
  if (m_waitPosReset)
  {
    CLog::Log(LOGDEBUG, "ASB: CXBMCRenderManager::WaitPresentTime WAITRESET");
    memset(m_errorbuff, 0, sizeof(m_errorbuff));
    presentcorr = 0.0;
  }
*/

  double targetpos = 0.5; //fraction of frame ahead of targetwaitclock that the abs dvd clock should ideally be after our wait has completed

  // assume the caller is trying to wait for the vblank prior to the one the frame should finally display on, and to avoid any possible indeterminate behaviour around the
  // the absolute clock value matching at that time we will target half an interval before it (so that the appropriate tick will take it distinctly past)
  double targetwaitclock = presenttime - (targetpos * frametime);

  targetwaitclock += presentcorr * frametime;  // adjust by our accumulated correction
  int64_t waitedtime;
  double iclock;
  double clock = CDVDClock::WaitAbsoluteClock(targetwaitclock * DVD_TIME_BASE, &iclock, &waitedtime) / DVD_TIME_BASE;
  double error     = ( clock - targetwaitclock ) / frametime - targetpos;  //error is number(fraction) of frames out we are from where we were trying to correct to
  double abserror     = ( clock - presenttime ) / frametime; //abserror is number(fraction) of frames out we are from where we were requested to present
  m_presenterr     = error;

// just experiments for future lateness algorithm
/*
  m_waitclock = clock;
  m_lastpresenttime  = presenttime;
  m_frametime  = frametime;
  if (waitedtime == 0 || abserror > targetpos)
  {
     if (waitedtime == 0)
     {
       m_waitlateness = iclock / DVD_TIME_BASE - clock;
     }
     else
     {
       m_waitlateness = (abserror - targetpos) * frametime;
     }
  }
  else
       m_waitlateness = 0.0;
*/

  // we should be careful not too overshoot if we are getting close to a wrap boundary to avoid
  // swinging back and forward unnecessarily

  // if wrapped abserror value changes by more 10% of frame from last then reset presentcorr and avgs
  // we should wrap error combine with m_presentcorr and if abs value greater 90% targetpos but less than 105% move presentcorr by 1% avg error
  // else move by 5% - reset avg
  // if presentcorr approx 0 and error-wrapped approx abs(0.5) then steer by 10% in that direction to get it quickly away from beat spot
  // if presentcorr + error-wrapped > 0.55 (we are required to make too large a forward correction) then reset presentcorr and avgs
  // if presentcorr + error-wrapped < -0.55 targetpos then wrap presentcorr to 0.5 targetpos to avoid any issues

  error = wrap(error, 0.0 - targetpos, 1.0 - targetpos);
  abserror = wrap(abserror, 0.0 - targetpos, 1.0 - targetpos);
  bool reset = false;
  if (m_lastabserror)
    CLog::Log(LOGDEBUG, "ASB: CXBMCRenderManager::WaitPresentTime abserror: %f error: %f m_lastabserror: %f presentcorr: %f", abserror, error, m_lastabserror, presentcorr);
  else
    CLog::Log(LOGDEBUG, "ASB: CXBMCRenderManager::WaitPresentTime abserror: %f error: %f m_lastabserror: NULL presentcorr: %f", abserror, error, presentcorr);
  if ( (m_lastabserror != NULL && (fabs(abserror - m_lastabserror) > 0.1)) || (presentcorr > 0.55) || (fabs(presentcorr + error) > 0.6))
  {
     //unstable fluctuations in position (implying no point in trying to target), or targetting too far forward a correction,
     //or simply too large the current error target...then reset presentcorr and error buffer
     //note: allowing the small forward correction overshoot helps to avoid hanging around the 0.5 beat zone for long enough to reduce the chances of
     //targetting forward then backward with high frequency.
     presentcorr = 0.0;
     m_lastabserror = NULL; //avoid next iteration using the lastabserror value
     reset = true;
  }
  else if (presentcorr < -0.55)
  {
     //targetting to far backward, force it to next vblank at next iteration by wrapping presentcorr straight to 0.5 and reset error buffer
     //this avoids travelling through the troublesome beat zone
     presentcorr = 0.5;
     m_lastabserror = NULL; //avoid next iteration using the lastabserror value
     reset = true;
  }
  else if (fabs(presentcorr) < 0.05 && fabs(error) > 0.45)
  {
     //strongly target in error direction to move clear of beat zone quickly initially
     presentcorr += 0.1 * error;
  }
  else if (fabs(presentcorr + error) > 0.45)
  {
     //we have drifted close to the beat zone so now we try to drift carefully so that we only wrap when surely required
     // this is done with 1% adjustments of recent error average
     //save error in the buffer
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
     reset = true; //adjustment is large enough to warrant clearing averages
     presentcorr = presentcorr + (error * 0.05);
  }

  if (reset)
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
  state.Format("sync:%+3d%% avg:%3d%% error:%2d%%"
              ,     MathUtils::round_int(m_presentcorr * 100)
              ,     MathUtils::round_int(avgerror      * 100)
              , abs(MathUtils::round_int(m_presenterr  * 100)));
  return state;
}

bool CXBMCRenderManager::Configure(unsigned int width, unsigned int height, unsigned int d_width, unsigned int d_height, float fps, unsigned flags)
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
    m_bReconfigured = true;
    m_presentstep = PRESENT_IDLE;
    m_presentevent.Set();
    m_vdpauflip = PRESENT_IDLE;
    m_flipEvent.Reset();
  }

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

    if(m_presentstep == PRESENT_IDLE)
       SetVdpauFlipValues();

    if(m_presentstep == PRESENT_FLIP)
    {
      m_overlays.Flip();
      m_pRenderer->FlipPage(m_presentsource);
      m_presentstep = PRESENT_FRAME;
      m_presentevent.Set();
      ResetVdpauFlip();
    }
  }

  CSharedLock lock(m_sharedSection);

  if( m_presentmethod == VS_INTERLACEMETHOD_RENDER_WEAVE
   || m_presentmethod == VS_INTERLACEMETHOD_RENDER_WEAVE_INVERTED)
    m_pRenderer->RenderUpdate(clear, flags | RENDER_FLAG_BOTH, alpha);
  else
    m_pRenderer->RenderUpdate(clear, flags | RENDER_FLAG_LAST, alpha);

  m_overlays.Render();

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

  // free renderer resources.
  // TODO: we may also want to release the renderer here.
  if (m_pRenderer)
    m_pRenderer->UnInit();
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

void CXBMCRenderManager::FlipPage(volatile bool& bStop, double timestamp /* = 0LL*/, int source /*= -1*/, EFIELDSYNC sync /*= FS_NONE*/, bool bIsVdpau /*= false*/)
{
  if (bIsVdpau)
  {
    FlipVdpau(timestamp, sync);
    return;
  }

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

    if(m_presentstep == PRESENT_IDLE)
      SetVdpauFlipValues();

    if(m_presentstep == PRESENT_FLIP)
    {
      m_overlays.Flip();
      m_pRenderer->FlipPage(m_presentsource);
      m_presentstep = PRESENT_FRAME;
      m_presentevent.Set();
      ResetVdpauFlip();
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

  m_overlays.Render();
  lock.Leave();

  /* wait for this present to be valid */
  if(g_graphicsContext.IsFullScreenVideo())
    WaitPresentTime(m_presenttime);

//  ResetVdpauFlip();

  m_presentevent.Set();
}

/* simple present method */
void CXBMCRenderManager::PresentSingle()
{
//  CSingleLock lock(g_graphicsContext);

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
#ifdef HAS_GL
  glFlush(); // attempt to have gpu done with pixmap and vdpau
#endif
}

void CXBMCRenderManager::UpdateResolution()
{
  if (m_bReconfigured)
  {
    CRetakeLock<CExclusiveLock> lock(m_sharedSection);
    if (g_graphicsContext.IsFullScreenVideo() && g_graphicsContext.IsFullScreenRoot())
    {
      RESOLUTION res = GetResolution();
      g_graphicsContext.SetVideoResolution(res);
    }
    m_bReconfigured = false;
  }
}

bool CXBMCRenderManager::WaitVdpauFlip(unsigned int timeout)
{
  CSingleLock lock(m_flipSection);
  if (m_vdpauflip == PRESENT_IDLE)
    return true;
  lock.Leave();

  if (!m_flipEvent.WaitMSec(timeout))
    return false;

  m_vdpauflip = PRESENT_IDLE;

  if(!g_graphicsContext.IsFullScreenVideo())
    WaitPresentTime(m_presenttime);

  return true;
}

void CXBMCRenderManager::ResetVdpauFlip()
{
  m_flipEvent.Set();
}

bool CXBMCRenderManager::FlipVdpau(double pts, EFIELDSYNC sync)
{
  CSingleLock lock(m_flipSection);
  m_vdpauflip = PRESENT_FLIP;
  m_iPts = pts;
  m_Sync = sync;
  m_flipEvent.Reset();
  lock.Leave();

  g_application.NewFrame();
  return true;
}

void CXBMCRenderManager::SetVdpauFlipValues()
{
  CSingleLock lock(m_flipSection);
  if (m_vdpauflip != PRESENT_FLIP)
  {
    return;
  }
  m_vdpauflip = PRESENT_FRAME;
  lock.Leave();

  m_presenttime = m_iPts;

  m_presentfield = m_Sync;

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

