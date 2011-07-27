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
}

CDVDPlayerVideoOutput::~CDVDPlayerVideoOutput()
{
  StopThread();
}

void CDVDPlayerVideoOutput::Start()
{
  Create();
}

void CDVDPlayerVideoOutput::Reset()
{
  if (m_recover)
    StopThread();

  CSingleLock lock(m_criticalSection);
  while (!m_toOutputMessage.empty())
    m_toOutputMessage.pop();
  while (!m_fromOutputMessage.empty())
      m_fromOutputMessage.pop();

  if (m_recover)
    Start();
}

void CDVDPlayerVideoOutput::Dispose()
{
  m_bStop = true;
  m_toMsgSignal.Set();
  StopThread();
  m_recover = true;
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
  CSingleLock lock(m_criticalSection);
  m_toOutputMessage.push(msg);
  lock.Leave();

  m_toMsgSignal.Set();
}

int CDVDPlayerVideoOutput::GetMessageSize()
{
  CSingleLock lock(m_criticalSection);
  return m_toOutputMessage.size();
}

bool CDVDPlayerVideoOutput::GetMessage(FromOutputMessage &msg, bool bWait)
{
  bool bReturn = false;

  while (!m_bStop)
  {
    if (bWait && !m_fromMsgSignal.WaitMSec(300))
    {
      CLog::Log(LOGWARNING, "CDVDPlayerVideoOutput::GetMessage - timed out");
    }

    { CSingleLock lock(m_criticalSection);
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
  CSingleLock lock(m_criticalSection);
  m_pts = pts;
}

double CDVDPlayerVideoOutput::GetPts()
{
  CSingleLock lock(m_criticalSection);
  return m_pts;
}

void CDVDPlayerVideoOutput::Process()
{
  bool flushed = false;

  CSingleLock lock(m_criticalSection);
  lock.Leave();

  while (!m_bStop)
  {
    lock.Enter();
    if (!m_toOutputMessage.empty())
    {
      if (m_recover)
      {
        if (RefreshGlxContext())
          m_recover = false;
      }

      FromOutputMessage fromMsg;
      ToOutputMessage toMsg = m_toOutputMessage.front();
      m_toOutputMessage.pop();

      bool gotPic;
      if (toMsg.bLastPic)
      {
        m_picture.iFlags &= ~DVP_FLAG_INTERLACED;
        m_picture.iFlags |= DVP_FLAG_NOSKIP;
        gotPic = false;
      }
      else
        gotPic = GetPicture(toMsg, fromMsg);
      lock.Leave();

      if (gotPic)
      {
        if (!flushed)
        {
          fromMsg.iResult = m_pVideoPlayer->OutputPicture(&m_picture,m_pts);

          if (fromMsg.iResult & EOS_FLUSH)
          {
            flushed = true;
          }
        }
        else
          fromMsg.iResult = EOS_FLUSH;

        // guess next frame pts. iDuration is always valid
        if (toMsg.iSpeed != 0)
          m_pts += m_picture.iDuration * toMsg.iSpeed / abs(toMsg.iSpeed);

        lock.Enter();
        m_fromOutputMessage.push(fromMsg);
        lock.Leave();
        m_fromMsgSignal.Set();
      }
    }
    else
    {
      lock.Leave();
      if (!m_toMsgSignal.WaitMSec(500))
        CLog::Log(LOGNOTICE,"CDVDPlayerVideoOutput::Process - timeout waiting for message");
    }
  }
  DestroyGlxContext();
}



bool CDVDPlayerVideoOutput::GetPicture(ToOutputMessage toMsg, FromOutputMessage &fromMsg)
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

    m_picture.iGroupId = toMsg.iGroupId;

    if(m_picture.iDuration == 0.0)
      m_picture.iDuration = toMsg.fFrameTime;

    if(toMsg.bPacketDrop)
      m_picture.iFlags |= DVP_FLAG_DROPPED;

    if (toMsg.bNotToSkip)
    {
      m_picture.iFlags |= DVP_FLAG_NOSKIP;
    }

    // validate picture timing,
    // if both dts/pts invalid, use pts calulated from picture.iDuration
    // if pts invalid use dts, else use picture.pts as passed
    if (m_picture.dts == DVD_NOPTS_VALUE && picture.pts == DVD_NOPTS_VALUE)
      m_picture.pts = m_pts;
    else if (m_picture.pts == DVD_NOPTS_VALUE)
      m_picture.pts = m_picture.dts;

    /* use forced aspect if any */
    if( toMsg.fForcedAspectRatio != 0.0f )
      picture.iDisplayWidth = (int) (picture.iDisplayHeight * toMsg.fForcedAspectRatio);

    //Deinterlace if codec said format was interlaced or if we have selected we want to deinterlace
    //this video
    if(!(toMsg.iFilters & CDVDVideoCodec::FILTER_DEINTERLACE_ANY))
    {
      if((toMsg.mInt == VS_INTERLACEMETHOD_DEINTERLACE)
      || (toMsg.mInt == VS_INTERLACEMETHOD_AUTO && (picture.iFlags & DVP_FLAG_INTERLACED)
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
      m_pts = m_picture.pts;
    }

    if (m_picture.iRepeatPicture)
      m_picture.iDuration *= m_picture.iRepeatPicture + 1;

    bReturn = true;
  }
  else
  {
    CLog::Log(LOGWARNING, "CDVDPlayerVideoOutput::GetPicture - error getting videoPicture.");
    m_recover = true;
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
