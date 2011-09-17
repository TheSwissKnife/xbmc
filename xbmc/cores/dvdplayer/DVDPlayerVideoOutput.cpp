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
#include "utils/TimeUtils.h"
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
  Dispose();
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

  {  CSingleLock lock(m_criticalSection);
    while (!m_toOutputMessage.empty())
       m_toOutputMessage.pop();
    while (!m_fromOutputMessage.empty())
       m_fromOutputMessage.pop();

    memset(&m_picture, 0, sizeof(DVDVideoPicture));
    m_state = VO_STATE_WAITINGPLAYERSTART;
  }

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

bool CDVDPlayerVideoOutput::ResyncClockToVideo(double pts, int playerSpeed, bool bFlushed /* = false */)
{
   if (pts != DVD_NOPTS_VALUE)
   {
      // milliseconds of time required for decoder thread to buffer data adequately and 
      // to allow audtio thread the chance to adjust probably less than 100ms is not a good idea in any case
      int prepTime; 
      if (playerSpeed == DVD_PLAYSPEED_PAUSE)
         prepTime = 0;
      else if (bFlushed)
         prepTime = 300;
      else
         prepTime = 150;

      double clockPrime = 0.0;
      int sleepMs = 0;
      double displayDelay; 
      double displayTickDelay; 
      double displaySignalToViewDelay;

      //TODO: calculate the prepTime based on GetDisplayDelay(), GetDisplaySignalToViewDelay(), and buffer decode prep time estimate
      // - GetDisplayDelay() will tell us how long minimum in absolute clock time it should 
      //   take to get a picture from output to visible on display
      // - buffer decode prep estimate should be say 100ms for non-flush and 300ms for flush
      // - we should set the clock to give close to exact display pts to clock match using GetDisplaySignalToViewDelay()
      //   which tells us how much clock time after target tick is required to make the pic visible

      // for prepTime of 0 we just set the clock directly and return, otherwise we need to let other clock subscribers
      // that we are controllling the clock and let then know if they can tweak it for some period within some bounds
      // - during that time we must wait around until that option has passed (leaving at least say 50ms to output)  
      if (playerSpeed != DVD_PLAYSPEED_PAUSE)
      {
         displayDelay = g_renderManager.GetDisplayDelay() * DVD_TIME_BASE;
         displaySignalToViewDelay = g_renderManager.GetDisplaySignalToViewDelay() * DVD_TIME_BASE;
         // g_renderManager.GetDisplayDelay() is total delay and we want to know the distance to clock tick
         displayTickDelay = displayDelay - displaySignalToViewDelay;
         double tickInterval = m_pClock->GetTickInterval();

         // target delay in dvd clock time by summing the decode prep time and the display delay
         double targetDelay = ((double)prepTime * DVD_TIME_BASE / 1000) + displayTickDelay;
         // round up to a clock tick multiple to make the actual adjust
         int ticks = (int)((targetDelay / tickInterval) + 1);
         // this is the adjustment in absolute clock terms that would allow to get ready for the tick
         double ticktime = (double)ticks * tickInterval; 
         // add in signal to view delay so that we will be sync'd at view time rather than swap to front buffer tick
         clockPrime = ticktime + displaySignalToViewDelay; //add in so that we will be sync'd at view time rather than swap to front buffer tick
         
         // determine a suitable sleep in millisecs after which we can check the clock after audio may have adjusted
         sleepMs = (int)(((clockPrime - displayDelay) / DVD_TIME_BASE * 1000) - 20);
         sleepMs = std::min(1000, sleepMs);
         sleepMs = std::max(5, sleepMs);
   
         //maxTickAdjustOffer is number of ticks forward other subscribers can move the clock
         //adjustOfferExpirySys is system time when the offer expires
         //controlDurSys is how long we wish to be the clock controller (clock will remove control after this elapsed sys time
         int64_t controlDurSys = 5 * CurrentHostFrequency(); //5 seconds
         int maxTickAdjustOffer = 2;
         int64_t adjustOfferExpirySys = (sleepMs - 5) * CurrentHostFrequency() / 1000; 

         m_pClock->SetVideoIsController(controlDurSys, maxTickAdjustOffer, adjustOfferExpirySys);
CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::ResyncClockToVideo doing SYNC SetVideoIsController for pts: %f with prepTime: %i, displayDelay: %f displaySignalToViewDelay: %f tickInterval: %f clockPrime: %f sleepMs: %i", pts, prepTime, displayDelay, displaySignalToViewDelay, tickInterval, clockPrime, sleepMs);
      }

CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::ResyncClockToVideo pre SetCurrentTickClock GetClock: %f", m_pClock->GetClock(false));
      m_pClock->SetCurrentTickClock(pts - (playerSpeed / DVD_PLAYSPEED_NORMAL) * clockPrime);
CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::ResyncClockToVideo done SetCurrentTickClock GetClock: %f", m_pClock->GetClock(false));
      

      if (playerSpeed != DVD_PLAYSPEED_PAUSE)
      {
         //TODO: perhaps do sleep in small steps to monitor the clock progress
         Sleep(sleepMs);
CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::ResyncClockToVideo done Sleep GetClock: %f", m_pClock->GetClock(false));
      }
      return true;
   }
   else
      return false;
}

void CDVDPlayerVideoOutput::Process()
{
  bool bTimeoutTryPic = false;
  int playerSpeed = DVD_PLAYSPEED_NORMAL;
  int prevOutputSpeed = DVD_PLAYSPEED_NORMAL;
  bool bExpectMsgDelay = false;
  double videoDelay = m_pVideoPlayer->GetDelay();
  // make overlay delay be the video delay combined with the subtitle delay (so that the subtitle delay is relative to video)
  double overlayDelay = videoDelay + m_pVideoPlayer->GetSubtitleDelay();
  double overlayInterval;
  double msgDelayInterval;
  double frametime = (double)DVD_TIME_BASE / m_pVideoPlayer->GetFrameRate();
  double pts = DVD_NOPTS_VALUE;
  double prevOutputPts = DVD_NOPTS_VALUE;
  double clock = m_pClock->GetAbsoluteClock(true);
  double prevOutputClock = clock; //init as if we just output
  double timeoutStartClock = clock; 
  SetRendererConfiguring(false);
  m_state = VO_STATE_WAITINGPLAYERSTART;

  while (!m_bStop)
  {
    // always init that we are not outputting or dropping at start of each loop
    bool bOutPic = false;
    bool bPicDrop = false;
    bool bOutputOverlay = false;
    bool bResync = false; //clock resync flag to tell render manager to discard any previous clock corrections

    if (m_state == VO_STATE_RENDERERCONFIGURING)
    {
      // we just wait loop waiting for event so that we can check 
      m_toMsgSignal.WaitMSec(200);
      if (RendererConfiguring())
      {
         clock = m_pClock->GetAbsoluteClock(true);
         if (clock - prevOutputClock > DVD_MSEC_TO_TIME(5000))
         {
             CLog::Log(LOGWARNING,"CDVDPlayerVideoOutput::Process - timeout waiting for player to inform us renderer is configured");
         }
         continue;
      }
      m_state = VO_STATE_WAITINGPLAYERSTART; //move from renderer configuring to waiting player start state
      if (m_picture.iFlags & DVP_FLAG_ALLOCATED)
         bOutPic = true; //we should try to output the pic that initiated the renderer configure
    }
  
    // from here on in this loop iteration we should be having state change only via message passing
    // and we should start in a state from list
    // {VO_STATE_WAITINGPLAYERSTART, VO_STATE_OVERLAYONLY, VO_STATE_QUIESCING, VO_STATE_QUIESCED, VO_STATE_NORMALOUTPUT}

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

      playerSpeed = toMsg.iSpeed;

      // initial adjust of state logic
      // NOTE: come out of VO_STATE_WAITINGPLAYERSTART when toMsg.bPlayerStarted and playerSpeed not pause 
      //       but move back only when not toMsg.bPlayerStarted 

      if (m_state == VO_STATE_WAITINGPLAYERSTART && 
              toMsg.bPlayerStarted && playerSpeed != DVD_PLAYSPEED_PAUSE &&
              (msgCmd == VOCMD_NEWPIC || msgCmd == VOCMD_PROCESSOVERLAYONLY))
        m_state = VO_STATE_SYNCCLOCKFLUSH; //seek, start, flush (longer delay required): move to (temporary state) sync-clock-flush

      else if (m_state != VO_STATE_WAITINGPLAYERSTART && !(toMsg.bPlayerStarted))
        m_state = VO_STATE_WAITINGPLAYERSTART; //move back to waiting player start state

      else if (m_state != VO_STATE_WAITINGPLAYERSTART && 
               prevOutputSpeed == DVD_PLAYSPEED_PAUSE && playerSpeed != DVD_PLAYSPEED_PAUSE && 
              (msgCmd == VOCMD_NEWPIC || msgCmd == VOCMD_PROCESSOVERLAYONLY)) 
         m_state = VO_STATE_SYNCCLOCK; //un-pause (shorter delay required): move to (temporary state) sync-clock

      else if (m_state != VO_STATE_NORMALOUTPUT && msgCmd == VOCMD_NEWPIC && 
               toMsg.bPlayerStarted && playerSpeed != DVD_PLAYSPEED_PAUSE)
         m_state = VO_STATE_NORMALOUTPUT; //move to normal state 

      if (msgCmd == VOCMD_FINISHSTREAM)
      { 
          m_state = VO_STATE_QUIESCED; //move to quiesced state directly
          //tell player we have quiesced (by assumption that the player won't send any more cmds after this one)
          SendPlayerMessage(EOS_QUIESCED);
          continue;
      }
      else if (msgCmd == VOCMD_DEALLOCPIC)
      {
          // no state change required we just do as commanded and continue
          m_picture.iFlags &= ~DVP_FLAG_ALLOCATED;
          continue;
      }
      else if (msgCmd == VOCMD_SPEEDCHANGE)
      {
          // only real use at present is to sync clock to a better approximation than what dvd player might have done
          // - but during overlay only mode it could also be used to perhaps handle overlay timing better (TODO)
CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::Process Got msgCmd == VOCMD_SPEEDCHANGE playerSpeed: %i", playerSpeed);
          if (playerSpeed == DVD_PLAYSPEED_PAUSE && prevOutputPts != DVD_NOPTS_VALUE)
          {
             // pretend we output at this speed as likely no pic msg will be given to us at this speed
             // - this allows us to detect un-pause later
             prevOutputSpeed = DVD_PLAYSPEED_PAUSE;
             m_pClock->Discontinuity(prevOutputPts + videoDelay);  //get the clock re-positioned approximately
          }
          continue;
      }
      else if (msgCmd == VOCMD_EXPECTDELAY)
      {
          // just allows us to adjust our expected delay intervals
          bExpectMsgDelay = true;
          msgDelayInterval = interval;
          continue;
      }
      else if (msgCmd == VOCMD_PROCESSOVERLAYONLY)
      {
          if ((m_state == VO_STATE_SYNCCLOCK) || (m_state == VO_STATE_SYNCCLOCKFLUSH))
          {
             if (playerSpeed == DVD_PLAYSPEED_PAUSE && prevOutputPts != DVD_NOPTS_VALUE)
                m_pClock->Discontinuity(prevOutputPts + videoDelay);  //get the clock re-positioned approximately
             else if (pts != DVD_NOPTS_VALUE)
                m_pClock->Discontinuity(pts + videoDelay - (playerSpeed / DVD_PLAYSPEED_NORMAL) * DVD_MSEC_TO_TIME(50));  //get the clock re-positioned approximately
          }
          if (!(m_picture.iFlags & DVP_FLAG_ALLOCATED))
          {
             // bad condition so return to waiting for start state (until we have a better idea)
             //TODO: tell player if we could not start doing overlays?
             CLog::Log(LOGERROR, "CDVDPlayerVideoOutput::Process picture not allocated and overlay-only mode requested");
             m_state = VO_STATE_WAITINGPLAYERSTART; 
          }
          else
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
        if (bOutPic && ((m_state == VO_STATE_SYNCCLOCK) || (m_state == VO_STATE_SYNCCLOCKFLUSH)))
        {
           bool bFlushed = m_state == VO_STATE_SYNCCLOCKFLUSH ? true : false;
           ResyncClockToVideo(pts + videoDelay, playerSpeed, bFlushed);
           bResync = true;
           m_state = VO_STATE_NORMALOUTPUT; //assume we move now to normal state
        }
      }
    }
   
    
    // only configure renderer after we got a new picture from decoder
    if (bOutPic && m_pVideoPlayer->CheckRenderConfig(&m_picture))
    {
      // we are in state {VO_STATE_NORMALOUTPUT or VO_STATE_WAITINGPLAYERSTART} and we have a picture to output 
      // but its format requires a renderer configure, so we now move state accordingly and no longer wait for messsages
      // until video player updates the configuring state directly
      m_state = VO_STATE_RENDERERCONFIGURING; //move to renderer configuring state
      SetRendererConfiguring();
      SetPts(pts); //allow other threads to know our pts
      SendPlayerMessage(EOS_CONFIGURE);
      continue;
    }

    if (m_state == VO_STATE_OVERLAYONLY) //see if it is time to try to process an overlay
    {
      clock = m_pClock->GetAbsoluteClock(true);
      if (clock - prevOutputClock >= overlayInterval)
         bOutputOverlay = true;
    }

    if (bOutPic || bOutputOverlay) //we have something to output
    {
      // configure gl context
      if (Recovering())
      {
        if (RefreshGlxContext())
          SetRecovering(false);
      }
      // we want to tell OutputPicture our previous output speed so that it can use the previous speed for
      // calculation of presentation time
      prevOutputSpeed = playerSpeed;  //update our previous playspeed only when actually outputting
      int outPicResult = 0;
      SetPts(pts); //allow other threads to know our pts

      bool bOutputEarly = false;
      if (m_state == VO_STATE_WAITINGPLAYERSTART)
         bOutputEarly = true;

      // call ProcessOverlays here even if no new Pic
      // TODO: we should adjust ProcessOverlays to accept our speed value, and output-early state for consistency?
      int outOverlayResult = m_pVideoPlayer->ProcessOverlays(&m_picture, pts, overlayDelay);

      if (bOutPic)
      {
CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideoOutput::Process pts: %f playerSpeed: %i prevOutputSpeed: %i bOutputEarly: %i", pts, playerSpeed, prevOutputSpeed, (int)bOutputEarly);
         outPicResult = m_pVideoPlayer->OutputPicture(&m_picture, pts + videoDelay, playerSpeed, 
                                                       prevOutputSpeed, bOutputEarly, bResync);
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

      prevOutputClock = m_pClock->GetAbsoluteClock(true);
      // if we have a result to send back to player do so
      if (outPicResult)
         SendPlayerMessage(outPicResult);

      // guess next frame pts
      // required for future pics with no pts value
      prevOutputPts = pts;
      if (bOutPic && playerSpeed != DVD_PLAYSPEED_PAUSE && pts != DVD_NOPTS_VALUE)
         pts = pts + frametime * playerSpeed / abs(playerSpeed);
      else if (bOutputOverlay && pts != DVD_NOPTS_VALUE)
          pts += clock - prevOutputClock;

    } //end output section
    
    if (m_state != VO_STATE_WAITINGPLAYERSTART && playerSpeed == DVD_PLAYSPEED_PAUSE && ToMessageQIsEmpty())
       m_state = VO_STATE_QUIESCED; //now quiesced

    { //message waiting section
      if (bTimeoutTryPic)
         timeoutStartClock = m_pClock->GetAbsoluteClock(true);  //reset timeout start 
      else
         timeoutStartClock = prevOutputClock;
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
        clock = m_pClock->GetAbsoluteClock(true);
        if (clock - timeoutStartClock > DVD_MSEC_TO_TIME(5000))
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
        clock = m_pClock->GetAbsoluteClock(true);
        if (clock - timeoutStartClock > DVD_MSEC_TO_TIME(2000))
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
