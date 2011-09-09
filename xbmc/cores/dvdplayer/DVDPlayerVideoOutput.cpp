/*
 *      Copyright (C) 2005-2011 Team XBMC
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
#include "settings/AdvancedSettings.h"
#include "settings/GUISettings.h"
#include "settings/Settings.h"
#include "utils/log.h"
#include "DVDPlayerVideoOutput.h"
#include "DVDCodecs/Video/DVDVideoCodecFFmpeg.h"
#include "DVDCodecs/Video/DVDVideoPPFFmpeg.h"
#include "cores/VideoRenderers/RenderManager.h"
#include "windowing/WindowingFactory.h"
#include "Application.h"

CDVDPlayerVideoOutput::CDVDPlayerVideoOutput(CDVDPlayerVideo *videoplayer)
: CThread("Video Output Thread")
{
  m_pVideoPlayer = videoplayer;
  m_pts = 0;
  m_glWindow = 0;
  m_glContext = 0;
  m_pixmap = 0;
  m_glPixmap = 0;
  m_recover = true;
  m_configuring = false;
  memset(&m_picture, 0, sizeof(DVDVideoPicture));
}

CDVDPlayerVideoOutput::~CDVDPlayerVideoOutput()
{
  StopThread();
}

void CDVDPlayerVideoOutput::Start()
{
  Create();
}

void CDVDPlayerVideoOutput::Reset(bool resetConfigure /* = false */)
{
  CSingleLock lock(m_criticalSection);

  if (resetConfigure)
  {
    m_configuring = false;
    m_toMsgSignal.Set();
    return;
  }

  if (m_recover)
  {
    lock.Leave();
    StopThread();
  }

  while (!m_toOutputMessage.empty())
     m_toOutputMessage.pop();
  while (!m_fromOutputMessage.empty())
     m_fromOutputMessage.pop();

  memset(&m_picture, 0, sizeof(DVDVideoPicture));

  if (m_recover)
    Start();
}

void CDVDPlayerVideoOutput::Dispose()
{
  m_bStop = true;
  m_toMsgSignal.Set();
  StopThread();
  m_recover = true;
  m_configuring = false;
}

void CDVDPlayerVideoOutput::OnStartup()
{
  CLog::Log(LOGNOTICE, "CDVDPlayerVideoOutput::OnStartup: Output Thread created");
}

void CDVDPlayerVideoOutput::OnExit()
{
  CLog::Log(LOGNOTICE, "CDVDPlayerVideoOutput::OnExit: Output Thread terminated");
}

bool CDVDPlayerVideoOutput::SendMessage(ToOutputMessage &msg, int timeout /* = 0 */)
{
  CStopWatch timer;
  if (timeout)
     timer.StartZero();
  while (timeout > 0 && m_bStop)
  {
     Sleep(1);
     timeout -= timer.GetElapsedMilliseconds();
  }

  if (m_bStop)
     return false;

  CSingleLock lock(m_msgSection);
  m_toOutputMessage.push(msg);
  lock.Leave();

  m_toMsgSignal.Set();
  return true;
}

int CDVDPlayerVideoOutput::GetMessageSize()
{
  CSingleLock lock(m_msgSection);
  return m_toOutputMessage.size();
}

bool CDVDPlayerVideoOutput::GetMessage(FromOutputMessage &msg, int timeout /* = 0 */)
{
  bool bReturn = false;

  CStopWatch timer;
  if (timeout > 0)
     timer.StartZero();
  while (!m_bStop)
  {
    { CSingleLock lock(m_msgSection);
      if (!m_fromOutputMessage.empty())
      {
        msg = m_fromOutputMessage.front();
        m_fromOutputMessage.pop();
        bReturn = true;
        break;
      }
    }
    if (timeout > 0 && !m_fromMsgSignal.WaitMSec(timeout))
    {
      //CLog::Log(LOGWARNING, "CDVDPlayerVideoOutput::GetMessage - timed out");
      return false;
    }

    if (timeout <= 0)
      break;
    timeout -= timer.GetElapsedMilliseconds();
  }

  return bReturn;
}

void CDVDPlayerVideoOutput::SetCodec(CDVDVideoCodec *codec)
{
  m_pVideoCodec = codec;
}

void CDVDPlayerVideoOutput::SetPts(double pts)
{
  CSingleLock lock(m_msgSection);
  m_pts = pts;
}

double CDVDPlayerVideoOutput::GetPts()
{
  CSingleLock lock(m_msgSection);
  return m_pts;
}

void CDVDPlayerVideoOutput::Process()
{
  CSingleLock mLock(m_msgSection);
  mLock.Leave();
  CSingleLock cLock(m_criticalSection);
  cLock.Leave();
  bool bOutputPrevPic = false;
  bool bTimeoutTryPic = false;
  bool bPlayerStarted = false;
  bool bMsg = false;
  bool bOverlaysOnly = false;
  bool bPlayerFinishStream = false;
  double overlayDelay = m_pVideoPlayer->GetSubtitleDelay();
  double videoDelay = m_pVideoPlayer->GetDelay();
  double overlayInterval;
  int outputSpeed = 0;
  double frametime = (double)DVD_TIME_BASE / m_pVideoPlayer->GetFrameRate();
  double pts = DVD_NOPTS_VALUE;
  double clock = CDVDClock::GetAbsoluteClock(true);
  double lastOutputClock = clock; //init as if we just output

  while (!m_bStop)
  {
    mLock.Enter();
    bMsg = !m_toOutputMessage.empty();
    mLock.Leave();
    if (!m_configuring && (bMsg || bOutputPrevPic || bTimeoutTryPic || bOverlaysOnly))
    {
      cLock.Enter();
      if (m_recover)
      {
        if (RefreshGlxContext())
          m_recover = false;
      }
      cLock.Leave();

      bool bNewPic = false;
      bool bPicDrop = false;
      double interval;
      int msgCmd = 0; 
      int playerSpeed = 0;
      if (!bOutputPrevPic && bMsg) // then process the message
      {
        mLock.Enter();
        ToOutputMessage toMsg = m_toOutputMessage.front();
        m_toOutputMessage.pop();
        msgCmd = toMsg.iCmd;
        bPicDrop = toMsg.bDrop;
        interval = toMsg.fInterval;
        playerSpeed = toMsg.iSpeed;
        bPlayerStarted = toMsg.bPlayerStarted;
        mLock.Leave();
        // assume old speed during player pause speed, and if only change if player has started
        // so that we before start we can first frame displayed quickly (user can more quickly see start frame)
        if (playerSpeed != 0 && bPlayerStarted) //assume old speed during pause speed
           outputSpeed = playerSpeed;
      }

      if (bOutputPrevPic && (!(m_picture.iFlags & DVP_FLAG_ALLOCATED)))
      {
          // ignore bOutputPrevPic state if unexpectedly we have no allocted pic that we can output
          bOutputPrevPic = false;
          continue;
      }
      else if (msgCmd == VOCMD_FINISHSTREAM)
      { 
          bPlayerFinishStream = true;
          FromOutputMessage fromMsg;
          fromMsg.iResult = EOS_QUIESCED;
          mLock.Enter();
          m_fromOutputMessage.push(fromMsg);
          mLock.Leave();
          m_fromMsgSignal.Set();
          continue;
      }
      else if (msgCmd == VOCMD_DEALLOCPIC)
      {
          m_picture.iFlags &= ~DVP_FLAG_ALLOCATED;
          continue;
      }
      else if (msgCmd == VOCMD_PROCESSOVERLAYONLY)
      {
          // ignore process-overlay only msg if we have no last pic that we output
          //TODO: tell player if we could not start doing overlays?
          if (!bPlayerStarted || !(m_picture.iFlags & DVP_FLAG_ALLOCATED))
             bOverlaysOnly = false;
          else
          {
             bOverlaysOnly = true;
             if (interval == 0.0)
                overlayInterval = DVD_MSEC_TO_TIME(100);
             else
                overlayInterval = interval;
          }
          bPlayerFinishStream = false;
      }
      else if (msgCmd == VOCMD_NEWPIC || bTimeoutTryPic)
      {  // we got a msg informing of a pic available or we timed-out waiting 
         // and will try to get the pic anyway (assuming previous speed and no drop)
        bNewPic = GetPicture(pts, frametime, bPicDrop);
        if (bNewPic)
        {
           bOverlaysOnly = false;
           bPlayerFinishStream = false;
        }
      }

      bTimeoutTryPic = false; //reset
      bool bOutputOverlays = false;
      clock = CDVDClock::GetAbsoluteClock(true);
      if (bOverlaysOnly)
      {
         if (clock - lastOutputClock >= overlayInterval)
            bOutputOverlays = true;
      }

      if (bNewPic || bOutputOverlays || bOutputPrevPic) //we have something to output
      {
        int iResult = 0;
        SetPts(pts); //allow other threads to know our pts

        // only configure output after we got a new picture from decoder
        if (bNewPic && m_pVideoPlayer->CheckRenderConfig(&m_picture))
        {
          iResult = EOS_CONFIGURE;
          m_configuring = true;
          bOutputPrevPic = true;
        }

        if (!m_configuring)
        {
          // call ProcessOverlays here even if no new Pic
          m_pVideoPlayer->ProcessOverlays(&m_picture, pts, overlayDelay);
 
          if (bNewPic || bOutputPrevPic)
          {
             iResult = m_pVideoPlayer->OutputPicture(&m_picture, pts, videoDelay, outputSpeed);
             if (!(iResult & (EOS_DROPPED | EOS_ABORT)) && (!bPlayerStarted))
             {
                iResult |= EOS_STARTED; //tell player we have started outputting
             }
             if (bOutputPrevPic)
                bOutputPrevPic = false;
          }
        }

        if (iResult)
        {
           FromOutputMessage fromMsg;
           fromMsg.iResult = iResult;
           mLock.Enter();
           m_fromOutputMessage.push(fromMsg);
           mLock.Leave();
           m_fromMsgSignal.Set();
        }
        if (!(iResult & (EOS_CONFIGURE | EOS_DROPPED | EOS_ABORT)))
        {
          // signal new frame to application
          g_application.NewFrame();
        }

        // guess next frame pts
        // required for future pics with no pts value
        if (bNewPic && outputSpeed != 0 && pts != DVD_NOPTS_VALUE)
           pts = pts + frametime * outputSpeed / abs(outputSpeed);
        else if (bOutputOverlays && pts != DVD_NOPTS_VALUE)
            pts += clock - lastOutputClock;

        lastOutputClock = CDVDClock::GetAbsoluteClock(true);

      } //end output section

    }
    else //waiting section
    {

      bTimeoutTryPic = false;
      // we want to wait only say 5ms to loop back around to see if overlay output is due 
      // (improve later if we need smoother overlays)
      if (bOverlaysOnly)
         m_toMsgSignal.WaitMSec(5);
      else if (bPlayerFinishStream) //wait but no need to log timeouts as we expect to
        m_toMsgSignal.WaitMSec(100);
      else if (m_configuring || !bPlayerStarted)
      {
        // log timeout only after 5 seconds and set bTimeoutTryPic
        m_toMsgSignal.WaitMSec(200);
        clock = CDVDClock::GetAbsoluteClock(true);
        if (clock - lastOutputClock > DVD_MSEC_TO_TIME(5000))
        {
          CLog::Log(LOGWARNING,"CDVDPlayerVideoOutput::Process - timeout waiting for message (configuring: %i bPlayerStarted: %i), forcing trypic", (int)m_configuring, (int)bPlayerStarted);
          bTimeoutTryPic = true;
        }
      }
      else 
      {
        // log timeout after 100ms and set bTimeoutTryPic after 1s
        m_toMsgSignal.WaitMSec(100);
        CLog::Log(LOGNOTICE,"CDVDPlayerVideoOutput::Process - timeout waiting for message");
        clock = CDVDClock::GetAbsoluteClock(true);
        if (clock - lastOutputClock > DVD_MSEC_TO_TIME(1000))
        {
          CLog::Log(LOGNOTICE,"CDVDPlayerVideoOutput::Process forcing trypic");
          bTimeoutTryPic = true;
        }
      }
    } //end else waiting section
  }
  DestroyGlxContext();
}

// pts input as estimated pts for this pic and output as the corrected pts (which should also be in m_picture.pts)
// frametime is output as the frame duration 
bool CDVDPlayerVideoOutput::GetPicture(double& pts, double& frametime, bool drop /* = false*/)
{
  bool bReturn = false;

  DVDVideoPicture picture;
  CDVDVideoPPFFmpeg mPostProcess("");
  CStdString sPostProcessType;

  // try to retrieve the picture (should never fail!), unless there is a demuxer bug ofcours
  m_pVideoCodec->ClearPicture(&m_picture);
  if (m_pVideoCodec->GetPicture(&m_picture))
  {
    sPostProcessType.clear();

    if (drop)
       m_picture.iFlags |= DVP_FLAG_DROPPED;

    //TODO: store untouched pts and dts values in ring buffer of say 20 entries to allow decoder flush to make a better job of starting from correct place

    // validate picture timing,
    // if both pts invalid, use pts calculated from previous pts and iDuration
    // if still invalid use dts, else use picture.pts as passed
    //if (m_picture.dts == DVD_NOPTS_VALUE && picture.pts == DVD_NOPTS_VALUE)
    if (m_picture.pts == DVD_NOPTS_VALUE)
      m_picture.pts = pts;
    else if (m_picture.pts == DVD_NOPTS_VALUE)
      m_picture.pts = m_picture.dts;

    //Deinterlace if codec said format was interlaced or if we have selected we want to deinterlace
    //this video
    EINTERLACEMETHOD mInt = g_settings.m_currentVideoSettings.m_InterlaceMethod;
    unsigned int mFilters = m_pVideoCodec->GetFilters();
    if(!(mFilters & CDVDVideoCodec::FILTER_DEINTERLACE_ANY))
    {
      if((mInt == VS_INTERLACEMETHOD_DEINTERLACE)
      || (mInt == VS_INTERLACEMETHOD_AUTO && (picture.iFlags & DVP_FLAG_INTERLACED)
                                          && !g_renderManager.Supports(VS_INTERLACEMETHOD_RENDER_BOB)))
      {
        if (!sPostProcessType.empty())
          sPostProcessType += ",";
        sPostProcessType += g_advancedSettings.m_videoPPFFmpegDeint;
      }
    }

    if (g_settings.m_currentVideoSettings.m_PostProcess)
    {
      if (!sPostProcessType.empty())
        sPostProcessType += ",";
      // This is what mplayer uses for its "high-quality filter combination"
      sPostProcessType += g_advancedSettings.m_videoPPFFmpegPostProc;
    }

    if (!sPostProcessType.empty())
    {
      mPostProcess.SetType(sPostProcessType);
      if (mPostProcess.Process(&m_picture))
        mPostProcess.GetPicture(&m_picture);
    }

    /* if frame has a pts (usually originiating from demux packet), use that */
    if(m_picture.pts != DVD_NOPTS_VALUE)
    {
      //SetPts(m_picture.pts);  
      m_picture.pts = m_pVideoPlayer->GetCorrectedPicturePts(m_picture.pts, frametime);
      pts = m_picture.pts;
    }
    else
      frametime = (double)DVD_TIME_BASE / m_pVideoPlayer->GetFrameRate();

//TODO: consider whether we should always update iDuration to frametime regardless
// - perhaps should always set it once we think we have a good calculation on frametime?
    bool bDurationCalculated = false;
    if(m_picture.iDuration == 0.0)
    {
      bDurationCalculated = true;
      m_picture.iDuration = frametime;
    }

    if (!bDurationCalculated && m_picture.iRepeatPicture)
      m_picture.iDuration *= m_picture.iRepeatPicture + 1;

    frametime = m_picture.iDuration; //make them consistent at least
    bReturn = true;
  }
  else
  {
    CLog::Log(LOGWARNING, "CDVDPlayerVideoOutput::GetPicture - error getting videoPicture.");
    frametime = (double)DVD_TIME_BASE / m_pVideoPlayer->GetFrameRate();
//    CSingleLock lock(m_criticalSection);
//    m_recover = true;
//    lock.Leave();
    bReturn = false;
  }

  return bReturn;
}

bool CDVDPlayerVideoOutput::RefreshGlxContext()
{
  Display*     dpy;
  GLXContext   glContext;

  dpy = g_Windowing.GetDisplay();
  glContext = g_Windowing.GetGlxContext();

  // Get our window attribs.
  XWindowAttributes wndattribs;
  XGetWindowAttributes(dpy, DefaultRootWindow(dpy), &wndattribs); // returns a status but I don't know what success is

  m_pixmap = XCreatePixmap(dpy,
                           DefaultRootWindow(dpy),
                           192,
                           108,
                           wndattribs.depth);
  if (!m_pixmap)
  {
    CLog::Log(LOGERROR, "CDVDPlayerVideoOutput::RefreshGlxContext - Unable to create XPixmap");
    return false;
  }

  // create gl pixmap
  int num=0;
  int fbConfigIndex = 0;

  int doubleVisAttributes[] = {
    GLX_RENDER_TYPE, GLX_RGBA_BIT,
    GLX_RED_SIZE, 8,
    GLX_GREEN_SIZE, 8,
    GLX_BLUE_SIZE, 8,
    GLX_ALPHA_SIZE, 8,
    GLX_DEPTH_SIZE, 8,
    GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
    GLX_BIND_TO_TEXTURE_RGBA_EXT, True,
    GLX_DOUBLEBUFFER, False,
    GLX_Y_INVERTED_EXT, True,
    GLX_X_RENDERABLE, True,
    None
  };

  int pixmapAttribs[] = {
    GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
    GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGBA_EXT,
    None
  };

  GLXFBConfig *fbConfigs;
  fbConfigs = glXChooseFBConfig(dpy, DefaultScreen(dpy), doubleVisAttributes, &num);
  if (fbConfigs==NULL)
  {
    CLog::Log(LOGERROR, "CDVDPlayerVideoOutput::RefreshGlxContext - No compatible framebuffers found");
    return false;
  }
  fbConfigIndex = 0;

  m_glPixmap = glXCreatePixmap(dpy, fbConfigs[fbConfigIndex], m_pixmap, pixmapAttribs);

  if (!m_glPixmap)
  {
    CLog::Log(LOGINFO, "CDVDPlayerVideoOutput::RefreshGlxContext - Could not create Pixmap");
    XFree(fbConfigs);
    return false;
  }

  XVisualInfo *visInfo;
  visInfo = glXGetVisualFromFBConfig(dpy, fbConfigs[fbConfigIndex]);
  if (!visInfo)
  {
    CLog::Log(LOGINFO, "CDVDPlayerVideoOutput::RefreshGlxContext - Could not obtain X Visual Info for pixmap");
    XFree(fbConfigs);
    return false;
  }
  XFree(fbConfigs);

  m_glContext = glXCreateContext(dpy, visInfo, glContext, True);
  XFree(visInfo);

  if (!glXMakeCurrent(dpy, m_glPixmap, m_glContext))
  {
    CLog::Log(LOGINFO, "CDVDPlayerVideoOutput::RefreshGlxContext - Could not make Pixmap current");
    return false;
  }

  CLog::Log(LOGNOTICE, "CDVDPlayerVideoOutput::RefreshGlxContext - refreshed context");
  return true;
}

bool CDVDPlayerVideoOutput::DestroyGlxContext()
{
  g_renderManager.ReleaseProcessor();

  Display *dpy = g_Windowing.GetDisplay();
  if (m_glContext)
  {
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, m_glContext);
  }
  m_glContext = 0;

  if (m_glPixmap)
    glXDestroyPixmap(dpy, m_glPixmap);
  m_glPixmap = 0;

  if (m_pixmap)
    XFreePixmap(dpy, m_pixmap);
  m_pixmap = 0;

  return true;
}
