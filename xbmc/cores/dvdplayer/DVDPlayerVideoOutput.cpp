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
  m_outputprevpic = false;
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
    return;
  }

  while (!m_toOutputMessage.empty())
     m_toOutputMessage.pop();
  while (!m_fromOutputMessage.empty())
     m_fromOutputMessage.pop();

  memset(&m_picture, 0, sizeof(DVDVideoPicture));
  if (m_recover)
  {
    lock.Leave();
    StopThread();
    Start();
  }
}

void CDVDPlayerVideoOutput::Dispose()
{
  m_bStop = true;
  m_toMsgSignal.Set();
  StopThread();
  m_recover = true;
  m_configuring = false;
  m_outputprevpic = false;
}

void CDVDPlayerVideoOutput::OnStartup()
{
  CLog::Log(LOGNOTICE, "CDVDPlayerVideoOutput::OnStartup: Output Thread created");
}

void CDVDPlayerVideoOutput::OnExit()
{
  CLog::Log(LOGNOTICE, "CDVDPlayerVideoOutput::OnExit: Output Thread terminated");
}

void CDVDPlayerVideoOutput::SendMessage(ToOutputMessage &msg)
{
  CSingleLock lock(m_msgSection);
  m_toOutputMessage.push(msg);
  lock.Leave();

  m_toMsgSignal.Set();
}

int CDVDPlayerVideoOutput::GetMessageSize()
{
  CSingleLock lock(m_msgSection);
  return m_toOutputMessage.size();
}

bool CDVDPlayerVideoOutput::GetMessage(FromOutputMessage &msg, bool bWait)
{
  bool bReturn = false;

  while (!m_bStop)
  {
    if (bWait && !m_fromMsgSignal.WaitMSec(500))
    {
      CLog::Log(LOGWARNING, "CDVDPlayerVideoOutput::GetMessage - timed out");
      // try to stop this getting stuck forever
      return false;
    }

    { CSingleLock lock(m_msgSection);
      if (!m_fromOutputMessage.empty())
      {
        msg = m_fromOutputMessage.front();
        m_fromOutputMessage.pop();
        bReturn = true;
        break;
      }
    }

    if (!bWait)
      break;
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
  bool started = false;

  while (!m_bStop)
  {
    if (m_outputprevpic && !m_configuring)
    {
      cLock.Enter();
      if (m_recover)
      {
        if (RefreshGlxContext())
          m_recover = false;
      }
      cLock.Leave();

      FromOutputMessage fromMsg;
      double pts = GetPts();
      //output with speed of zero to force render asap
      int speed = 0;
      if (started)
         speed = m_speed;
      fromMsg.iResult = m_pVideoPlayer->OutputPicture(&m_picture,pts,speed);
      started = true;
      if (fromMsg.iResult & (EOS_DROPPED | EOS_ABORT))
      {
         mLock.Enter();
         m_fromOutputMessage.push(fromMsg);
         mLock.Leave();
         m_fromMsgSignal.Set();
      }
      else
      {
         // signal new frame to application
         g_application.NewFrame();
      }
      m_outputprevpic = false;
    } 
    else if (!m_toOutputMessage.empty() && !m_configuring)
    {
      cLock.Enter();
      if (m_recover)
      {
        if (RefreshGlxContext())
          m_recover = false;
      }
      cLock.Leave();

      mLock.Enter();
      ToOutputMessage toMsg = m_toOutputMessage.front();
      m_toOutputMessage.pop();
      mLock.Leave();

      bool newPic = false;
      bool lastPic = false;
      if (toMsg.bLastPic)
      {
        if (started && (m_picture.iFlags & DVP_FLAG_ALLOCATED))
           lastPic = true;
      }
      else
        newPic = GetPicture(toMsg);

      FromOutputMessage fromMsg;
      fromMsg.iResult = 0;
      if (newPic || lastPic)
      {
        m_speed = toMsg.iSpeed;
        // only configure output after we got a new picture from decoder
        if (newPic && m_pVideoPlayer->CheckRenderConfig(&m_picture))
        {
          fromMsg.iResult = EOS_CONFIGURE;
          m_configuring = true;
          m_outputprevpic = true;
        }
        else
        {
          // call ProcessOverlays here
          // TODO: add speed parameter and maybe delay
          double pts = GetPts();
          m_pVideoPlayer->ProcessOverlays(&m_picture,pts);
 
          if (newPic)
          {
             //TODO: add delay parameter maybe?
             int speed = 0;
             if (started)
                speed = m_speed;
             fromMsg.iResult = m_pVideoPlayer->OutputPicture(&m_picture,pts,speed);
             started = true;
          }
        }

        if (fromMsg.iResult & (EOS_CONFIGURE | EOS_DROPPED | EOS_ABORT))
        {
CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::Process m_fromOutputMessage.push fromMsg.iResult: %i", fromMsg.iResult);
           mLock.Enter();
           m_fromOutputMessage.push(fromMsg);
           mLock.Leave();
           m_fromMsgSignal.Set();
        }
        else
        {
          // signal new frame to application
          g_application.NewFrame();
        }

        // guess next frame pts. iDuration is always valid
        // required for pics with no pts value
        if (!m_outputprevpic && m_speed != 0)
           SetPts(GetPts() + m_picture.iDuration * m_speed / abs(m_speed));
      }
    }
    else
    {
      // waiting for a VC_PICTURE message or a finished configuring state
      if (!m_toMsgSignal.WaitMSec(100) && started && !m_configuring)
        CLog::Log(LOGNOTICE,"CDVDPlayerVideoOutput::Process - timeout waiting for message");
    }
  }
  DestroyGlxContext();
}

bool CDVDPlayerVideoOutput::GetPicture(ToOutputMessage toMsg)
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

    if(m_picture.iDuration == 0.0)
      m_picture.iDuration = toMsg.fFrameTime;

    if(toMsg.bDrop)
      m_picture.iFlags |= DVP_FLAG_DROPPED;

    // validate picture timing,
    // if both dts/pts invalid, use pts calulated from picture.iDuration
    // if pts invalid use dts, else use picture.pts as passed
    if (m_picture.dts == DVD_NOPTS_VALUE && picture.pts == DVD_NOPTS_VALUE)
      m_picture.pts = GetPts();
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
CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::GetPicture picture.pts: %f", m_picture.pts);
      SetPts(m_picture.pts);
    }

    if (m_picture.iRepeatPicture)
      m_picture.iDuration *= m_picture.iRepeatPicture + 1;

    bReturn = true;
  }
  else
  {
    CLog::Log(LOGWARNING, "CDVDPlayerVideoOutput::GetPicture - error getting videoPicture.");
    CSingleLock lock(m_criticalSection);
    m_recover = true;
    lock.Leave();
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
