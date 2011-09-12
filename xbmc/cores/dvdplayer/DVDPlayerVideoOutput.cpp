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

CDVDPlayerVideoOutput::CDVDPlayerVideoOutput(CDVDPlayerVideo *videoplayer, CDVDClock* pClock)
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
  m_pClock = pClock;
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
  //CSingleLock lock(m_criticalSection);

  if (resetConfigure)
  {
    //m_configuring = false;
    SetRendererConfiguring(false);
    m_toMsgSignal.Set(); //wake up sleep signal event
    return;
  }

  bool bRecover = Recovering();
  if (bRecover)
  {
    StopThread();
  }

  while (!m_toOutputMessage.empty())
     m_toOutputMessage.pop();
  while (!m_fromOutputMessage.empty())
     m_fromOutputMessage.pop();

  memset(&m_picture, 0, sizeof(DVDVideoPicture));

  if (bRecover)
    Start();
}

bool CDVDPlayerVideoOutput::RendererConfiguring()
{
  CSingleLock lock(m_criticalSection);
  return m_configuring;
}

void CDVDPlayerVideoOutput::SetRendererConfiguring(bool configuring /* = true */)
{
  CSingleLock lock(m_criticalSection);
  m_configuring = configuring;
}

bool CDVDPlayerVideoOutput::Recovering()
{
  CSingleLock lock(m_criticalSection);
  return m_recover;
}

void CDVDPlayerVideoOutput::SetRecovering(bool recover /* true */)
{
  CSingleLock lock(m_criticalSection);
  m_recover = recover;
}

void CDVDPlayerVideoOutput::Dispose()
{
  m_bStop = true;
  m_toMsgSignal.Set();
  StopThread();
  m_recover = true; //no need for lock as thread is not started
  //m_configuring = false;
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

int CDVDPlayerVideoOutput::GetToMessageSize()
{
  CSingleLock lock(m_msgSection);
  return m_toOutputMessage.size();
}

bool CDVDPlayerVideoOutput::ToMessageQIsEmpty()
{
  CSingleLock lock(m_msgSection);
  return m_toOutputMessage.empty();
}

ToOutputMessage CDVDPlayerVideoOutput::GetToMessage()
{
  CSingleLock lock(m_msgSection);
  ToOutputMessage toMsg;
  if (!m_toOutputMessage.empty())
  {
    toMsg = m_toOutputMessage.front();
    m_toOutputMessage.pop();
  }
  return toMsg;
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

void CDVDPlayerVideoOutput::SendPlayerMessage(int result)
{
  FromOutputMessage fromMsg;
  fromMsg.iResult = result;
  CSingleLock mLock(m_msgSection);
  m_fromOutputMessage.push(fromMsg);
  mLock.Leave();
  m_fromMsgSignal.Set();
}

/*
VO_STATE_RECOVER = 1,           //recovering
VO_STATE_RENDERERCONFIGURING,         //waiting for renderer to configure
//VO_STATE_CLOCKSYSNC,        //syncing clock (does this deserve a state...it must make video the master, sync the clock, then after say 10 seconds resign control)
VO_STATE_OVERLAYONLY,       //just processng overlays
VO_STATE_QUIESCING,         //moving to state of QUIESCED 
VO_STATE_QUIESCED,          //state of not expecting regular pic msgs to process and prev fully processed
VO_STATE_NORMALOUTPUT       //processing pics and overlays as normal
*/

void CDVDPlayerVideoOutput::Process()
{
  bool bTimeoutTryPic = false;
  int playerSpeed = DVD_PLAYSPEED_NORMAL;
  bool bExpectMsgDelay = false;
  double overlayDelay = m_pVideoPlayer->GetSubtitleDelay();
  double videoDelay = m_pVideoPlayer->GetDelay();
  double overlayInterval;
  double msgDelayInterval;
  int outputSpeed = DVD_PLAYSPEED_PAUSE;
  int prevOutputSpeed = DVD_PLAYSPEED_PAUSE;
  double frametime = (double)DVD_TIME_BASE / m_pVideoPlayer->GetFrameRate();
  double pts = DVD_NOPTS_VALUE;
  double clock = CDVDClock::GetAbsoluteClock(true);
  double lastOutputClock = clock; //init as if we just output
  SetRendererConfiguring(false);
  m_state = VO_STATE_RECOVER;  //start in recover state

  while (!m_bStop)
  {
    // always init that we are not outputting or dropping at start of each loop
    bool bOutPic = false;
    bool bPicDrop = false;
    bool bOutputOverlay = false;

    if (m_state == VO_STATE_RECOVER)
    {
      if (RefreshGlxContext())
      {
        SetRecovering(false);
        m_state = VO_STATE_WAITINGPLAYERSTART; //move from recover to waiting player start state
      }
      else
      {
        Sleep(5);
        continue; //check recover again until we can move to init state
      }
    }
    else if (m_state == VO_STATE_RENDERERCONFIGURING)
    {
      // we just wait loop waiting for event so that we can check 
      m_toMsgSignal.WaitMSec(200);
      if (RendererConfiguring())
      {
         clock = CDVDClock::GetAbsoluteClock(true);
         if (clock - lastOutputClock > DVD_MSEC_TO_TIME(5000))
         {
             CLog::Log(LOGWARNING,"CDVDPlayerVideoOutput::Process - timeout waiting for player to inform us renderer is configured");
         }
         continue;
      }
      m_state = VO_STATE_WAITINGPLAYERSTART; //move from renderer configuring to waiting player start state
      if (m_picture.iFlags & DVP_FLAG_ALLOCATED)
         bOutPic = true; //we should try to output the pic that initiated the renderer configure
    }
  
    // from here on in this iteration we should be having state change only via message passing

    bool bHaveMsg = !(ToMessageQIsEmpty());
    // provided we are not to be outputting previous pic then 
    // process the next message if there is one or try to force a picture if timeout condition 
    if (!bOutPic && (bHaveMsg || bTimeoutTryPic))
    {
      if (bHaveMsg)
         bExpectMsgDelay = false; //reset

      VOCMD_TYPE msgCmd = VOCMD_NOCMD; 
      double interval;
      ToOutputMessage toMsg = GetToMessage();
      msgCmd = toMsg.iCmd;
      bPicDrop = toMsg.bDrop;
      interval = toMsg.fInterval;

      // we want to tell OutputPicture our previous output speed so that it can use the previous speed for
      // calculation of presentation time
      prevOutputSpeed = outputSpeed; 
      playerSpeed = toMsg.iSpeed;

      // from playerSpeed and state choose a preferable output speed for OutputPicture 
      // - when waiting for player to start also set output speed to paused (ie 0) 
      //   so we can get first frame displayed quickly (user can more quickly see start frame)
      if (m_state != VO_STATE_WAITINGPLAYERSTART)
         outputSpeed = playerSpeed;
      else if (m_state == VO_STATE_WAITINGPLAYERSTART)
         outputSpeed = DVD_PLAYSPEED_PAUSE;

      // adjust state for player started state
      if (m_state != VO_STATE_NORMALOUTPUT && msgCmd == VOCMD_NEWPIC && toMsg.bPlayerStarted)
        m_state = VO_STATE_NORMALOUTPUT; //move to normal output state
      else if (m_state != VO_STATE_WAITINGPLAYERSTART && !(toMsg.bPlayerStarted))
        m_state = VO_STATE_WAITINGPLAYERSTART; //move back to waiting player start state
      else if (m_state != VO_STATE_WAITINGPLAYERSTART && playerSpeed == DVD_PLAYSPEED_PAUSE && ToMessageQIsEmpty())
         m_state = VO_STATE_QUIESCING; //move to quiescing state (player has paused)

      if (msgCmd == VOCMD_FINISHSTREAM)
      { 
          m_state = VO_STATE_QUIESCED; //move to quiesced state
          SendPlayerMessage(EOS_QUIESCED); //tell player we have quiesced (by assumption that player won't send any more cmds after this one)
          continue;
      }
      else if (msgCmd == VOCMD_FLUSHSTREAM)
      { 
          //TODO: just make aware that it might be longer than usual for pic? or perhaps we also need to know which pts 
          continue;
      }
      else if (msgCmd == VOCMD_DEALLOCPIC)
      {
          m_picture.iFlags &= ~DVP_FLAG_ALLOCATED;
          continue;
      }
      else if (msgCmd == VOCMD_SPEEDCHANGE)
      {
          continue;
      }
      else if (msgCmd == VOCMD_EXPECTDELAY)
      {
          bExpectMsgDelay = true;
          msgDelayInterval = interval;
          continue;
      }
      else if (msgCmd == VOCMD_PROCESSOVERLAYONLY)
      {
          // ignore process-overlay only msg if we have no last pic that we output
          //TODO: tell player if we could not start doing overlays?
          if (m_state != VO_STATE_WAITINGPLAYERSTART && (m_picture.iFlags & DVP_FLAG_ALLOCATED))
          {
             m_state = VO_STATE_OVERLAYONLY;  //move to overlay-only state
             if (interval == 0.0)
                overlayInterval = DVD_MSEC_TO_TIME(100); //fail-safe default
             else
                overlayInterval = interval;
          }
      }
      else if (msgCmd == VOCMD_NEWPIC || bTimeoutTryPic)
      {  // we got a msg informing of a pic available or we timed-out waiting 
         // and will try to get the pic anyway (assuming previous speed and no drop)
        bOutPic = GetPicture(pts, frametime, bPicDrop);
      }
      bTimeoutTryPic = false; //reset
    }

    // only configure renderer after we got a new picture from decoder
    if (bOutPic && m_pVideoPlayer->CheckRenderConfig(&m_picture))
    {
      m_state = VO_STATE_RENDERERCONFIGURING; //move to renderer configuring state
      SetRendererConfiguring();
      SetPts(pts); //allow other threads to know our pts
      SendPlayerMessage(EOS_CONFIGURE);
      continue;
    }
    else if (m_state == VO_STATE_OVERLAYONLY) //see if it is time to try to process an overlay
    {
      clock = CDVDClock::GetAbsoluteClock(true);
      if (clock - lastOutputClock >= overlayInterval)
         bOutputOverlay = true;
    }

    if (bOutPic || bOutputOverlay) //we have something to output
    {
      int outPicResult = 0;
      SetPts(pts); //allow other threads to know our pts

      // call ProcessOverlays here even if no new Pic
      int outOverlayResult = m_pVideoPlayer->ProcessOverlays(&m_picture, pts, overlayDelay);

      if (bOutPic)
      {
         outPicResult = m_pVideoPlayer->OutputPicture(&m_picture, pts, videoDelay, outputSpeed, prevOutputSpeed);
         if (!(outPicResult & (EOS_DROPPED | EOS_ABORT)) && (m_state == VO_STATE_WAITINGPLAYERSTART))
         {
            outPicResult |= EOS_STARTED; //tell player we have started outputting to inform it that it can now fully start
         }
      }
      //provided we did not get a bad status from output of pic and outp of overlay then signal new frame to application
      if (!(outPicResult & (EOS_CONFIGURE | EOS_DROPPED | EOS_ABORT)) || (outOverlayResult != -1) )
      {
        // signal new frame to application
        g_application.NewFrame();
      }

      lastOutputClock = CDVDClock::GetAbsoluteClock(true);
      // if we have a result to send back to player do so
      if (outPicResult)
         SendPlayerMessage(outPicResult);

      // guess next frame pts
      // required for future pics with no pts value
      if (bOutPic && playerSpeed != DVD_PLAYSPEED_PAUSE && pts != DVD_NOPTS_VALUE)
         pts = pts + frametime * playerSpeed / abs(playerSpeed);
      else if (bOutputOverlay && pts != DVD_NOPTS_VALUE)
          pts += clock - lastOutputClock;

    } //end output section
    
    // set quiesced if we were quiescing and have no msgs and player is paused (now that we have done any ouptut)
    if (m_state == VO_STATE_QUIESCING && playerSpeed == DVD_PLAYSPEED_PAUSE && ToMessageQIsEmpty())
       m_state = VO_STATE_QUIESCED; //now quiesced

    { //message waiting section
      bTimeoutTryPic = false;
      // determine standard wait
      int wait = 100;
      if (bExpectMsgDelay)
         wait = msgDelayInterval * 1000 / DVD_TIME_BASE + 10;

      // for overlay only state we want to wait only say 5ms to loop back around to see if overlay output is due 
      // (improve later if we need smoother overlays)
      if (m_state == VO_STATE_OVERLAYONLY)
         m_toMsgSignal.WaitMSec(5);
      else if (m_state == VO_STATE_QUIESCED) //wait but no need to log timeouts as we expect to be waiting
        m_toMsgSignal.WaitMSec(wait);
      else if (m_state == VO_STATE_WAITINGPLAYERSTART)
      {
        // log timeout only after 5 seconds and set bTimeoutTryPic
        m_toMsgSignal.WaitMSec(wait);
        clock = CDVDClock::GetAbsoluteClock(true);
        if (clock - lastOutputClock > DVD_MSEC_TO_TIME(5000))
        {
          CLog::Log(LOGWARNING,"CDVDPlayerVideoOutput::Process - timeout waiting for message (player not started), forcing trypic");
          bTimeoutTryPic = true;
        }
      }
      else //normal state
      {
        // log timeout and set bTimeoutTryPic after 2s
        if (!m_toMsgSignal.WaitMSec(wait))
           CLog::Log(LOGNOTICE,"CDVDPlayerVideoOutput::Process - timeout waiting for message (player speed: %i)", playerSpeed);
        clock = CDVDClock::GetAbsoluteClock(true);
        if (clock - lastOutputClock > DVD_MSEC_TO_TIME(2000))
        {
          CLog::Log(LOGNOTICE,"CDVDPlayerVideoOutput::Process forcing trypic");
          bTimeoutTryPic = true;
        }
      }
    } //end message waiting section
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

CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::GetPicture pts: %f", m_picture.pts);
    // validate picture timing,
    // if both pts invalid, use pts calculated from previous pts and iDuration
    // if still invalid use dts, else use picture.pts as passed
    //if (m_picture.dts == DVD_NOPTS_VALUE && m_picture.pts == DVD_NOPTS_VALUE)
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
