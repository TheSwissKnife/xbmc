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
#include "settings/AdvancedSettings.h"
#include "settings/GUISettings.h"
#include "settings/Settings.h"
#include "video/VideoReferenceClock.h"
#include "utils/MathUtils.h"
#include "utils/TimeUtils.h"
#include "DVDPlayer.h"
#include "DVDPlayerVideo.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "DVDCodecs/DVDCodecUtils.h"
#include "DVDCodecs/Video/DVDVideoPPFFmpeg.h"
#include "DVDCodecs/Video/DVDVideoCodecFFmpeg.h"
#include "DVDDemuxers/DVDDemux.h"
#include "DVDDemuxers/DVDDemuxUtils.h"
#include "../../Util.h"
#include "DVDOverlayRenderer.h"
#include "DVDPerformanceCounter.h"
#include "DVDCodecs/DVDCodecs.h"
#include "DVDCodecs/Overlay/DVDOverlayCodecCC.h"
#include "DVDCodecs/Overlay/DVDOverlaySSA.h"
#include <sstream>
#include <iomanip>
#include <numeric>
#include <iterator>
#include "utils/log.h"
#include "DVDCodecs/Video/VDPAU.h"

using namespace std;

class CPulldownCorrection
{
public:
  CPulldownCorrection()
  {
    m_duration = 0.0;
    m_accum    = 0;
    m_total    = 0;
    m_next     = m_pattern.end();
  }

  void init(double fps, int *begin, int *end)
  {
    std::copy(begin, end, std::back_inserter(m_pattern));
    m_duration = DVD_TIME_BASE / fps;
    m_accum    = 0;
    m_total    = std::accumulate(m_pattern.begin(), m_pattern.end(), 0);
    m_next     = m_pattern.begin();
  }

  double pts()
  {
    double input  = m_duration * std::distance(m_pattern.begin(), m_next);
    double output = m_duration * m_accum / m_total;
    return output - input;
  }

  double dur()
  {
    return m_duration * m_pattern.size() * *m_next / m_total;
  }

  void next()
  {
    m_accum += *m_next;
    if(++m_next == m_pattern.end())
    {
      m_next  = m_pattern.begin();
      m_accum = 0;
    }
  }

  bool enabled()
  {
    return m_pattern.size() > 0;
  }
private:
  double                     m_duration;
  int                        m_total;
  int                        m_accum;
  std::vector<int>           m_pattern;
  std::vector<int>::iterator m_next;
};


class CDVDMsgVideoCodecChange : public CDVDMsg
{
public:
  CDVDMsgVideoCodecChange(const CDVDStreamInfo &hints, CDVDVideoCodec* codec)
    : CDVDMsg(GENERAL_STREAMCHANGE)
    , m_codec(codec)
    , m_hints(hints)
  {}
 ~CDVDMsgVideoCodecChange()
  {
    delete m_codec;
  }
  CDVDVideoCodec* m_codec;
  CDVDStreamInfo  m_hints;
};


CDVDPlayerVideo::CDVDPlayerVideo( CDVDClock* pClock
                                , CDVDOverlayContainer* pOverlayContainer
                                , CDVDMessageQueue& parent)
: CThread()
, m_messageQueue("video")
, m_messageParent(parent)
{
  m_pClock = pClock;
  m_pOverlayContainer = pOverlayContainer;
  m_pTempOverlayPicture = NULL;
  m_pVideoCodec = NULL;
  m_pOverlayCodecCC = NULL;
  m_speed = DVD_PLAYSPEED_NORMAL;

  m_bRenderSubs = false;
  m_stalled = false;
  m_started = false;
  m_iVideoDelay = 0;
  m_iSubtitleDelay = 0;
  m_fForcedAspectRatio = 0;
  m_iNrOfPicturesNotToSkip = 0;
  m_messageQueue.SetMaxDataSize(40 * 1024 * 1024);
  m_messageQueue.SetMaxTimeSize(8.0);
  g_dvdPerformanceCounter.EnableVideoQueue(&m_messageQueue);

  m_iCurrentPts = DVD_NOPTS_VALUE;
  m_iDroppedFrames = 0;
  m_iDecoderDroppedFrames = 0;
  m_iDecoderPresentDroppedFrames = 0;
  m_iOutputDroppedFrames = 0;
  m_iPlayerDropRequests = 0;
  m_fFrameRate = 25;
  m_bAllowFullscreen = false;
  memset(&m_output, 0, sizeof(m_output));
}

CDVDPlayerVideo::~CDVDPlayerVideo()
{
  StopThread();
  g_dvdPerformanceCounter.DisableVideoQueue();
  g_VideoReferenceClock.StopThread();
}

double CDVDPlayerVideo::GetOutputDelay()
{
    double time = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET);
    if( m_fFrameRate )
      time = (time * DVD_TIME_BASE) / m_fFrameRate;
    else
      time = 0.0;

    if( m_speed != 0 )
      time = time * DVD_PLAYSPEED_NORMAL / abs(m_speed);

    return time;
}

bool CDVDPlayerVideo::OpenStream( CDVDStreamInfo &hint )
{
  CLog::Log(LOGNOTICE, "Creating video codec with codec id: %i", hint.codec);
  CDVDVideoCodec* codec = CDVDFactoryCodec::CreateVideoCodec( hint );
  if(!codec)
  {
    CLog::Log(LOGERROR, "Unsupported video codec");
    return false;
  }

  if(g_guiSettings.GetBool("videoplayer.usedisplayasclock") && g_VideoReferenceClock.ThreadHandle() == NULL)
  {
    g_VideoReferenceClock.Create();
    //we have to wait for the clock to start otherwise alsa can cause trouble
    if (!g_VideoReferenceClock.WaitStarted(2000))
      CLog::Log(LOGDEBUG, "g_VideoReferenceClock didn't start in time");
  }

  if(m_messageQueue.IsInited())
    m_messageQueue.Put(new CDVDMsgVideoCodecChange(hint, codec), 0);
  else
  {
    OpenStream(hint, codec);
    CLog::Log(LOGNOTICE, "Creating video thread");
    m_messageQueue.Init();
    Create();
  }
  return true;
}

void CDVDPlayerVideo::OpenStream(CDVDStreamInfo &hint, CDVDVideoCodec* codec)
{
  //reported fps is usually not completely correct
  if (hint.fpsrate && hint.fpsscale)
    m_fFrameRate = DVD_TIME_BASE / CDVDCodecUtils::NormalizeFrameduration((double)DVD_TIME_BASE * hint.fpsscale / hint.fpsrate);
  else
    m_fFrameRate = 25;

  m_bCalcFrameRate = g_guiSettings.GetBool("videoplayer.usedisplayasclock") ||
                      g_guiSettings.GetBool("videoplayer.adjustrefreshrate");
  ResetFrameRateCalc();

/*
  m_iDroppedRequest = 0;
  m_iLateFrames = 0;
*/
  m_autosync = 1;
  ResetDropInfo();

  if( m_fFrameRate > 100 || m_fFrameRate < 5 )
  {
    CLog::Log(LOGERROR, "CDVDPlayerVideo::OpenStream - Invalid framerate %d, using forced 25fps and just trust timestamps", (int)m_fFrameRate);
    m_fFrameRate = 25;
  }

  // use aspect in stream if available
  m_fForcedAspectRatio = hint.aspect;

  if (m_pVideoCodec)
    delete m_pVideoCodec;

  m_pVideoCodec = codec;
  m_hints   = hint;
  m_stalled = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET) == 0;
  m_started = false;
  m_codecname = m_pVideoCodec->GetName();
}

void CDVDPlayerVideo::CloseStream(bool bWaitForBuffers)
{
  // wait until buffers are empty
  if (bWaitForBuffers && m_speed > 0) m_messageQueue.WaitUntilEmpty();

  m_messageQueue.Abort();

  // wait for decode_video thread to end
  CLog::Log(LOGNOTICE, "waiting for video thread to exit");

  StopThread(); // will set this->m_bStop to true

  m_messageQueue.End();

  CLog::Log(LOGNOTICE, "deleting video codec");
  if (m_pVideoCodec)
  {
    m_pVideoCodec->Dispose();
    delete m_pVideoCodec;
    m_pVideoCodec = NULL;
  }

  if (m_pTempOverlayPicture)
  {
    CDVDCodecUtils::FreePicture(m_pTempOverlayPicture);
    m_pTempOverlayPicture = NULL;
  }

  //tell the clock we stopped playing video
  m_pClock->UpdateFramerate(0.0);
}

void CDVDPlayerVideo::OnStartup()
{
  CThread::SetName("CDVDPlayerVideo");
  m_iDroppedFrames = 0;
  m_iDecoderDroppedFrames = 0;
  m_iDecoderPresentDroppedFrames = 0;
  m_iOutputDroppedFrames = 0;
  m_iPlayerDropRequests = 0;

  m_crop.x1 = m_crop.x2 = 0.0f;
  m_crop.y1 = m_crop.y2 = 0.0f;

  m_iCurrentPts = DVD_NOPTS_VALUE;
  m_FlipTimeStamp = m_pClock->GetAbsoluteClock();

#ifdef HAS_VIDEO_PLAYBACK
  if(!m_output.inited)
  {
    g_renderManager.PreInit();
    m_output.inited = true;
  }
#endif
  g_dvdPerformanceCounter.EnableVideoDecodePerformance(ThreadHandle());
}

void CDVDPlayerVideo::Process()
{
  CLog::Log(LOGNOTICE, "running thread: video_thread");

  DVDVideoPicture picture;
  CPulldownCorrection pulldown;
  CDVDVideoPPFFmpeg mPostProcess("");
  CStdString sPostProcessType;

  memset(&picture, 0, sizeof(DVDVideoPicture));

  double pts = 0;
  double frametime = (double)DVD_TIME_BASE / m_fFrameRate;

  //int iDropped = 0; //frames dropped in a row
  bool bRequestDrop = false;
  bool flushpausedclock = false;

  m_videoStats.Start();

  while (!m_bStop)
  {
    int iQueueTimeOut = (int)(m_stalled ? frametime / 4 : frametime * 10) / 1000;
    int iPriority = (m_speed == DVD_PLAYSPEED_PAUSE && m_started) ? 1 : 0;

    CDVDMsg* pMsg;
    MsgQueueReturnCode ret = m_messageQueue.Get(&pMsg, iQueueTimeOut, iPriority);

    if (MSGQ_IS_ERROR(ret) || ret == MSGQ_ABORT)
    {
      CLog::Log(LOGERROR, "Got MSGQ_ABORT or MSGO_IS_ERROR return true");
      break;
    }
    else if (ret == MSGQ_TIMEOUT)
    {
      // if we only wanted priority messages, this isn't a stall
      if( iPriority )
        continue;

      //Okey, start rendering at stream fps now instead, we are likely in a stillframe
      if( !m_stalled )
      {
        if(m_started)
          CLog::Log(LOGINFO, "CDVDPlayerVideo - Stillframe detected, switching to forced %f fps", m_fFrameRate);
        m_stalled = true;
        pts+= frametime*4;
      }

      //Waiting timed out, output last picture
      if( picture.iFlags & DVP_FLAG_ALLOCATED )
      {
        //Remove interlaced flag before outputting
        //no need to output this as if it was interlaced
        picture.iFlags &= ~DVP_FLAG_INTERLACED;
//        picture.iFlags |= DVP_FLAG_NOSKIP;
        OutputPicture(&picture, pts);
        pts+= frametime;
      }

      continue;
    }

    if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      ((CDVDMsgGeneralSynchronize*)pMsg)->Wait( &m_bStop, SYNCSOURCE_VIDEO );
      CLog::Log(LOGDEBUG, "CDVDPlayerVideo - CDVDMsg::GENERAL_SYNCHRONIZE");
      pMsg->Release();

      /* we may be very much off correct pts here, but next picture may be a still*/
      /* make sure it isn't dropped */
      m_iNrOfPicturesNotToSkip = 5;
      continue;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESYNC))
    {
      CDVDMsgGeneralResync* pMsgGeneralResync = (CDVDMsgGeneralResync*)pMsg;

      if(pMsgGeneralResync->m_timestamp != DVD_NOPTS_VALUE)
        pts = pMsgGeneralResync->m_timestamp;

      double delay = m_FlipTimeStamp - m_pClock->GetAbsoluteClock();
      if( delay > frametime ) delay = frametime;
      else if( delay < 0 )    delay = 0;

      if(pMsgGeneralResync->m_clock)
      {
        CLog::Log(LOGDEBUG, "CDVDPlayerVideo - CDVDMsg::GENERAL_RESYNC(%f, 1)", pts);
        m_pClock->Discontinuity(pts - delay);
      }
      else
        CLog::Log(LOGDEBUG, "CDVDPlayerVideo - CDVDMsg::GENERAL_RESYNC(%f, 0)", pts);

      pMsgGeneralResync->Release();
      continue;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_DELAY))
    {
      if (m_speed != DVD_PLAYSPEED_PAUSE)
      {
        double timeout = static_cast<CDVDMsgDouble*>(pMsg)->m_value;

        CLog::Log(LOGDEBUG, "CDVDPlayerVideo - CDVDMsg::GENERAL_DELAY(%f)", timeout);

        timeout *= (double)DVD_PLAYSPEED_NORMAL / abs(m_speed);
        timeout += CDVDClock::GetAbsoluteClock();

        while(!m_bStop && CDVDClock::GetAbsoluteClock() < timeout)
          Sleep(1);
      }
    }
    else if (pMsg->IsType(CDVDMsg::VIDEO_SET_ASPECT))
    {
      CLog::Log(LOGDEBUG, "CDVDPlayerVideo - CDVDMsg::VIDEO_SET_ASPECT");
      m_fForcedAspectRatio = *((CDVDMsgDouble*)pMsg);
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESET))
    {
      if(m_pVideoCodec)
        m_pVideoCodec->Reset();
      m_packets.clear();
      m_started = false;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH)) // private message sent by (CDVDPlayerVideo::Flush())
    {
      if(m_pVideoCodec)
        m_pVideoCodec->Reset();
      m_packets.clear();

      m_pullupCorrection.Flush();
      //we need to recalculate the framerate
      //TODO: this needs to be set on a streamchange instead
      ResetFrameRateCalc();

      m_stalled = true;
      m_started = false;
    }
    else if (pMsg->IsType(CDVDMsg::VIDEO_NOSKIP))
    {
      // libmpeg2 is also returning incomplete frames after a dvd cell change
      // so the first few pictures are not the correct ones to display in some cases
      // just display those together with the correct one.
      // (setting it to 2 will skip some menu stills, 5 is working ok for me).
      m_iNrOfPicturesNotToSkip = 5;
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      m_speed = static_cast<CDVDMsgInt*>(pMsg)->m_value;
      if(m_speed == DVD_PLAYSPEED_PAUSE)
        m_iNrOfPicturesNotToSkip = 0;
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_STARTED))
    {
      if(m_started)
        m_messageParent.Put(new CDVDMsgInt(CDVDMsg::PLAYER_STARTED, DVDPLAYER_VIDEO));
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_STREAMCHANGE))
    {
      CDVDMsgVideoCodecChange* msg(static_cast<CDVDMsgVideoCodecChange*>(pMsg));
      OpenStream(msg->m_hints, msg->m_codec);
      msg->m_codec = NULL;
      picture.iFlags &= ~DVP_FLAG_ALLOCATED;
    }

    if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* pPacket = ((CDVDMsgDemuxerPacket*)pMsg)->GetPacket();
      bool bPacketDrop     = ((CDVDMsgDemuxerPacket*)pMsg)->GetPacketDrop();

      if (m_stalled)
      {
        CLog::Log(LOGINFO, "CDVDPlayerVideo - Stillframe left, switching to normal playback");
        m_stalled = false;

        //don't allow the first frames after a still to be dropped
        //sometimes we get multiple stills (long duration frames) after each other
        //in normal mpegs
        m_iNrOfPicturesNotToSkip = 5;
      }
/*
      else if( iDropped*frametime > DVD_MSEC_TO_TIME(100) && m_iNrOfPicturesNotToSkip == 0 )
      { // if we dropped too many pictures in a row, insert a forced picture
        m_iNrOfPicturesNotToSkip = 1;
      }
*/

/*
#ifdef PROFILE
      bRequestDrop = false;
#else
      if (m_messageQueue.GetDataSize() == 0
      ||  m_speed < 0)
      {
        bRequestDrop = false;
        m_iDroppedRequest = 0;
        m_iLateFrames     = 0;
      }
#endif
*/

      // calc drop here
      bRequestDrop = false;
      int iDropDirective = CalcDropRequirement();

      //temporary hint config until we get lateness management implemented
      int drophint = 0;
      if ((m_speed != DVD_PLAYSPEED_NORMAL) && (m_speed != DVD_PLAYSPEED_PAUSE))
         drophint |= VC_HINT_NOPOSTPROC;
      if (bPacketDrop)
        drophint |= VC_HINT_NOPRESENT;
      // if we just dropped in any way try to squeeze next picture out so that we are less likely to miss renderer slot
      if (m_bJustDropped)
        drophint |= VC_HINT_HURRYUP;
      m_bJustDropped = false; //reset
      if (iDropDirective & DC_SUBTLE)
        drophint |= VC_HINT_DROPSUBTLE;
      if (iDropDirective & DC_URGENT)
        drophint |= VC_HINT_DROPURGENT;
      m_pVideoCodec->SetDropHint(drophint);

      if (iDropDirective & DC_DECODER)
        bRequestDrop = true;
      // if player want's us to drop this packet, do so nomatter what
      if(bPacketDrop)
      {
        m_iPlayerDropRequests++;
        bRequestDrop = true;
      }

      // tell codec if next frame should be dropped
      // problem here, if one packet contains more than one frame
      // both frames will be dropped in that case instead of just the first
      // decoder still needs to provide an empty image structure, with correct flags
      m_pVideoCodec->SetDropState(bRequestDrop);

CLog::Log(LOGDEBUG, "ASB: CDVDPlayerVideo About to do m_pVideoCodec->Decode SetDropState=%i, bPacketDrop: %i, drophint: %i, pPacket->pts: %f pPacket->iSize: %i", (int)bRequestDrop, (int)bPacketDrop, drophint, pPacket->pts, pPacket->iSize);
      drophint = 0;

      // don't let codec converge count instantly reduce buffered message count for a stream 
      // - as that happens after a flush and then defeats the object of having the buffer
      int iConvergeCount = m_pVideoCodec->GetConvergeCount();

      int iDecoderState = m_pVideoCodec->Decode(pPacket->pData, pPacket->iSize, pPacket->dts, pPacket->pts);
      if (m_pVideoCodec->GetConvergeCount() > iConvergeCount)
         iConvergeCount = m_pVideoCodec->GetConvergeCount();

      // buffer packets so we can recover should decoder flush for some reason
      if(iConvergeCount > 0)
      {
        int popped = 0;
        // store up to 11 seconds worth of demux messages (10s is a common key frame interval)
        // and prune back up to 20 each time if we suddenly need less (to make more efficent when replaying)
        m_packets.push_back(DVDMessageListItem(pMsg, 0));
        while (m_packets.size() > iConvergeCount || m_packets.size() * frametime > DVD_SEC_TO_TIME(11))
        {
          if (m_packets.size() <= 2 || popped > 20)
             break;
          m_packets.pop_front();
          popped++;
        }
      }

      m_videoStats.AddSampleBytes(pPacket->iSize);
      // assume decoder dropped a picture if it didn't give us any
      // picture from a demux packet, this should be reasonable
      // for libavformat as a demuxer as it normally packetizes
      // pictures when they come from demuxer
      if (iDecoderState & (VC_NOTDECODERDROPPED | VC_DROPPED))
      {
         // decoder give complete dropping information
         if (iDecoderState & VC_DROPPED)
         {
            m_iDroppedFrames++;
            if (iDecoderState & (VC_DECODERBIFIELDDROP | VC_DECODERFRAMEDROP))
            {
               m_iDecoderDroppedFrames++;
            }
            else if (iDecoderState & VC_PRESENTDROP)
               m_iDecoderPresentDroppedFrames++;
            m_bJustDropped = true;
            m_pullupCorrection.Flush(); //dropped frames mess up the pattern, so just flush it
            //iDropped++;
         }
      }
      //else if(bRequestDrop && !bPacketDrop && (iDecoderState & VC_BUFFER) && !(iDecoderState & VC_PICTURE))
      else if(bRequestDrop && (iDecoderState & VC_BUFFER) && !(iDecoderState & VC_PICTURE))
      {
        m_iDecoderDroppedFrames++;
        m_iDroppedFrames++;
        m_bJustDropped = true;
        m_pullupCorrection.Flush(); //dropped frames mess up the pattern, so just flush it
        //iDropped++;
      }

      // always reset decoder drop request state after each packet - ie avoid any linger, 
      // so that it can only be re-enabled by lateness calc or by player drop request
      bRequestDrop = false;

      // loop while no error
      while (!m_bStop)
      {

        // if decoder was flushed, we need to seek back again to resume rendering
        if (iDecoderState & VC_FLUSHED)
        {
          CLog::Log(LOGDEBUG, "CDVDPlayerVideo - video decoder was flushed");

          // ASB quick hack
          if (!m_pClock->IsPaused())
          {
             m_pClock->Pause();
             CLog::Log(LOGDEBUG,"ASB: Process flush paused clock: %f", m_pClock->GetClock());
             flushpausedclock = true;
          }

          while(!m_packets.empty())
          {
            CDVDMsgDemuxerPacket* msg = (CDVDMsgDemuxerPacket*)m_packets.front().message->Acquire();
            m_packets.pop_front();

            // all packets except the last one should be dropped
            // if prio packets and current packet should be dropped, this is likely a new reset
            msg->m_drop = !m_packets.empty() || (iPriority > 0 && bPacketDrop);
            m_messageQueue.Put(msg, iPriority + 10);
          }

          m_pVideoCodec->Reset();
          m_packets.clear();
          break;
        }

        // if decoder had an error, tell it to reset to avoid more problems
        if (iDecoderState & VC_ERROR)
        {
          CLog::Log(LOGDEBUG, "CDVDPlayerVideo - video decoder returned error");
          break;
        }

        // check for a new picture
        if (iDecoderState & VC_PICTURE)
        {

          if (flushpausedclock)
          {
              if (m_pClock->IsPaused())
              {
                 CLog::Log(LOGDEBUG,"ASB: Process flush resuming clock: %f", m_pClock->GetClock());
                 m_pClock->Resume();
              }
              else
                 CLog::Log(LOGDEBUG,"ASB: Process flush clock resumed without our knowledge clock: %f", m_pClock->GetClock());
              flushpausedclock = false;
          }
          // try to retrieve the picture (should never fail!), unless there is a demuxer bug ofcours
          m_pVideoCodec->ClearPicture(&picture);
          if (m_pVideoCodec->GetPicture(&picture))
          {
            sPostProcessType.clear();

            picture.iGroupId = pPacket->iGroupId;

            if(picture.iDuration == 0.0)
              picture.iDuration = frametime;

            // set picture as dropped if player requested a packet drop for decoders that we don't trust?
            // TODO this needs some major improvement - tracking pts/dts etc to be more sure the drop correct packet
            if ((!(iDecoderState & (VC_NOTDECODERDROPPED | VC_DROPPED))) && bPacketDrop)
              picture.iFlags |= DVP_FLAG_DROPPED;

/*
            if (m_iNrOfPicturesNotToSkip > 0)
            {
              picture.iFlags |= DVP_FLAG_NOSKIP;
              m_iNrOfPicturesNotToSkip--;
            }
*/

            // validate picture timing, 
            // if both dts/pts invalid, use pts calulated from picture.iDuration
            // if pts invalid use dts, else use picture.pts as passed
            if (picture.dts == DVD_NOPTS_VALUE && picture.pts == DVD_NOPTS_VALUE)
              picture.pts = pts;
            else if (picture.pts == DVD_NOPTS_VALUE)
              picture.pts = picture.dts;

            /* use forced aspect if any */
            if( m_fForcedAspectRatio != 0.0f )
              picture.iDisplayWidth = (int) (picture.iDisplayHeight * m_fForcedAspectRatio);

            //Deinterlace if codec said format was interlaced or if we have selected we want to deinterlace
            //this video
            EINTERLACEMETHOD mInt = g_settings.m_currentVideoSettings.m_InterlaceMethod;
            if((mInt == VS_INTERLACEMETHOD_DEINTERLACE)
            || (mInt == VS_INTERLACEMETHOD_AUTO && (picture.iFlags & DVP_FLAG_INTERLACED)
                                                && !g_renderManager.Supports(VS_INTERLACEMETHOD_RENDER_BOB)))
            {
              if (!sPostProcessType.empty())
                sPostProcessType += ",";
              sPostProcessType += g_advancedSettings.m_videoPPFFmpegDeint;
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
              if (mPostProcess.Process(&picture))
                mPostProcess.GetPicture(&picture);
            }

            /* if frame has a pts (usually originiating from demux packet), use that */
            if(picture.pts != DVD_NOPTS_VALUE)
            {
              if(pulldown.enabled())
                picture.pts += pulldown.pts();

              pts = picture.pts;
            }

            if(pulldown.enabled())
            {
              picture.iDuration = pulldown.dur();
              pulldown.next();
            }

            if (picture.iRepeatPicture)
              picture.iDuration *= picture.iRepeatPicture + 1;

            // quick hack
            bool not_startedpausedclock = false;
            if(m_started == false)
            {
               if (!m_pClock->IsPaused())
               {
                  m_pClock->Pause();
                  CLog::Log(LOGDEBUG,"ASB: Process paused clock: %f", m_pClock->GetClock());
                  not_startedpausedclock = true;
               }
            }
            CLog::Log(LOGDEBUG,"ASB: OutputPicture Entering for pts: %f iFlags: %i iDuration: %f clock: %f", pts, picture.iFlags, picture.iDuration, m_pClock->GetClock());
#if 1
            if (iDropDirective & DC_OUTPUT)
               picture.iFlags |= DVP_FLAG_DROPPED;

            int iResult = OutputPicture(&picture, pts);
#elif 0
            // testing NV12 rendering functions
            DVDVideoPicture* pTempNV12Picture = CDVDCodecUtils::ConvertToNV12Picture(&picture);
            int iResult = OutputPicture(pTempNV12Picture, pts);
            CDVDCodecUtils::FreePicture(pTempNV12Picture);
#elif 0
            // testing YUY2 or UYVY rendering functions
            // WARNING: since this scales a full YV12 frame, weaving artifacts will show on interlaced content
            // even with the deinterlacer on
            DVDVideoPicture* pTempYUVPackedPicture = CDVDCodecUtils::ConvertToYUV422PackedPicture(&picture, DVDVideoPicture::FMT_UYVY);
            //DVDVideoPicture* pTempYUVPackedPicture = CDVDCodecUtils::ConvertToYUV422PackedPicture(&picture, DVDVideoPicture::FMT_YUY2);
            int iResult = OutputPicture(pTempYUVPackedPicture, pts);
            CDVDCodecUtils::FreePicture(pTempYUVPackedPicture);
#endif

            if(m_started == false)
            {
              if(not_startedpausedclock)
              {
        	if (m_pClock->IsPaused())
                {
                   CLog::Log(LOGDEBUG,"ASB: Process resuming clock: %f", m_pClock->GetClock());
                   m_pClock->Resume();
                }
                else
                  CLog::Log(LOGDEBUG,"ASB: Process flush clock resumed without our knowledge clock: %f", m_pClock->GetClock());
                 not_startedpausedclock = false;
              }
              m_codecname = m_pVideoCodec->GetName();
              m_started = true;
              m_messageParent.Put(new CDVDMsgInt(CDVDMsg::PLAYER_STARTED, DVDPLAYER_VIDEO));
            }
            CLog::Log(LOGDEBUG,"ASB: OutputPicture RETURNED iResult: %i clock: %f", iResult, m_pClock->GetClock());

            // guess next frame pts. iDuration is always valid
            if (m_speed != 0)
              pts += picture.iDuration * m_speed / abs(m_speed);

            if( iResult & EOS_ABORT )
            {
              //if we break here and we directly try to decode again wihout
              //flushing the video codec things break for some reason
              //i think the decoder (libmpeg2 atleast) still has a pointer
              //to the data, and when the packet is freed that will fail.
              iDecoderState = m_pVideoCodec->Decode(NULL, 0, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);
              break;
            }

            // for trusty decoder we assume that EOS_DROPPED is always for a drop we have not yet counted
            if ((iDecoderState & (VC_NOTDECODERDROPPED | VC_DROPPED)) && (iResult & EOS_DROPPED))
            {
              m_iDroppedFrames++;
              m_iOutputDroppedFrames++;
              m_bJustDropped = true;
              //iDropped++;
            } 
            //else if( (iResult & EOS_DROPPED) && !bPacketDrop )
            else if (iResult & EOS_DROPPED)
            {
              m_iDroppedFrames++;
              m_iOutputDroppedFrames++;
              m_bJustDropped = true;
              //iDropped++;
            }
            else
            {
              m_dropinfo.iLastOutputPictureTime = CurrentHostCounter();
              // we can consider now that we did not skip tihs picture and reduce the not to skip counter
              if (m_iNrOfPicturesNotToSkip > 0)
                m_iNrOfPicturesNotToSkip--;
              //iDropped = 0;
             }

            //bRequestDrop = (iResult & EOS_VERYLATE) == EOS_VERYLATE;
          }
          else
          {
            CLog::Log(LOGWARNING, "Decoder Error getting videoPicture.");
            m_pVideoCodec->Reset();
          }
        }

        /*
        if (iDecoderState & VC_USERDATA)
        {
          // found some userdata while decoding a frame
          // could be closed captioning
          DVDVideoUserData videoUserData;
          if (m_pVideoCodec->GetUserData(&videoUserData))
          {
            ProcessVideoUserData(&videoUserData, pts);
          }
        }
        */

        // if the decoder needs more data, we just break this loop
        // and try to get more data from the videoQueue
        if (iDecoderState & VC_BUFFER)
          break;

        // the decoder didn't need more data, try to extract from the remaining buffer
        // if it does not need any more data then we should ask it hurry regardless as that may clear some room
        drophint = VC_HINT_HURRYUP;
        if ((m_speed != DVD_PLAYSPEED_NORMAL) && (m_speed != DVD_PLAYSPEED_PAUSE))
           drophint |= VC_HINT_NOPOSTPROC;
        m_pVideoCodec->SetDropHint(drophint);
        drophint = 0;
        iDecoderState = m_pVideoCodec->Decode(NULL, 0, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);
        if (iDecoderState & (VC_NOTDECODERDROPPED | VC_DROPPED))
        {
           // decoder exposing complete dropping information
           if (iDecoderState & VC_DROPPED)
           {
              m_iDroppedFrames++;
              if (iDecoderState & (VC_DECODERBIFIELDDROP | VC_DECODERFRAMEDROP))
                 m_iDecoderDroppedFrames++;
              else if (iDecoderState & VC_PRESENTDROP)
                 m_iDecoderPresentDroppedFrames++;
              m_bJustDropped = true;
              m_pullupCorrection.Flush(); //dropped frames mess up the pattern, so just flush it
           }
        }
      }
    }

    // all data is used by the decoder, we can safely free it now
    pMsg->Release();
  }
}

int CDVDPlayerVideo::CalcDropRequirement()
{
  // take g_renderManger.GetDisplayPts(&playspeed) which returns an interpolated pts in 
  // of the video frame being displayed (or about to be displayed),
  // and the playspeed applied at the time it was presented
  // - use this information to update the lateness tracking structure m_dropinfo which should 
  //   try to spot lateness coming before it arrives and for streams that are too much for the system
  //   determine a suitable background dropping rate.
  // return info on if and how to drop the next frame/picture (decoder with low urgency, normal, high
  // urgency, or drop output)
  // Should be called before every decode that passes input (demux) so that we can track input rate 
  //  (note that interlaced stream may be 1 packet per field or 1 packet per 2 fields)!
  // m_dropinfo should be reset completely at stream open or change.
  // m_iDecoderDroppedFrames should be the count of frames/fields actually dropped in decoder (and not in post decode)
  // m_dropinfo.iLastOutputPictureTime should be updated to system time after any call to OutputPicture 
  // where drop is not requested
  
  int iCalcId = m_dropinfo.iCalcId; //calculation id reset only at stream change
  m_dropinfo.iCalcId++;
  int iDPlaySpeed = -999;  //init invalid playspeed

// temporary
  double fCurAbsClock;
  double fCurClock = m_pClock->GetClock(&fCurAbsClock);
  iDPlaySpeed = m_speed;
  if (m_iCurrentPts == DVD_NOPTS_VALUE || iDPlaySpeed == 0)
     return 0;
  double fDPts = m_iCurrentPts + ((fCurAbsClock - m_iCurrentPtsClock) * iDPlaySpeed / DVD_PLAYSPEED_NORMAL);
// end temporary
  //proper:
  //double fCurClock = m_pClock->GetClock();
  //double fDPts = g_renderManger.GetDisplayPts(&iDPlaySpeed);

  int64_t Now = CurrentHostCounter();
  int iDropRequestDistance = 0; //min distance in call iterations between drop requests (0 meaning ok to drop every time)
  double fDInterval = 0.0;
  double fFrameRate = m_fFrameRate;
  if (fFrameRate == 0.0)
     fFrameRate = 25; //just to be sure we don't divide by zero now
  if (g_VideoReferenceClock.GetRefreshRate(&fDInterval) <= 0)
     fDInterval = 1 / fFrameRate;
  fDInterval *= DVD_TIME_BASE; //refactor to dvd clock time;

  // try to give a {0,1} oscillation for optionally offsetting drop requests 
  // to avoid beating with non-dropable frames
  int iOsc = m_dropinfo.iOscillator; 
  //if we are very late for a long time then our dropping requests are not frequent enough so increase frequency a little
  m_dropinfo.iVeryLateCount = std::min(m_dropinfo.iVeryLateCount, 300);
  int iDropMore = m_dropinfo.iVeryLateCount / 50;
  int iDecoderDrops = m_iDecoderDroppedFrames; 

  // allow to go 5% past half a display frame late before considering as late, limited to 4ms min
  double fLatenessThreshold = std::max(DVD_MSEC_TO_TIME(4), (fDInterval / 2) * 1.05);
  // threshold drop ratio where for anything higher we need to consider dropping before being late
  double fDropRatioThreshold = 1.0 / 50; //1 in 50

  int curidx = m_dropinfo.iCurSampIdx; //about to be populated
  int iLastDecoderDropRequestCalcId = m_dropinfo.iLastDecoderDropRequestCalcId;
  int iLastOutputDropRequestCalcId = m_dropinfo.iLastOutputDropRequestCalcId;
  int iLastDecoderDropRequestDecoderDrops = m_dropinfo.iLastDecoderDropRequestDecoderDrops;

  m_dropinfo.iDropNextFrame = 0; //default not to drop

  bool bClockInterrupted = false; 
  // TODO set interrupted for maybe reset/resync and discontinuity later
  bClockInterrupted = m_pClock->IsPaused();
  // TODO also what about decode flush - should clear samples here too I guess

  // if clock speed now does not match playspeed at the presentation time then reset counters
  int iClockSpeed = m_pClock->GetSpeed();
  if (m_pClock->IsPaused())
     iClockSpeed = DVD_PLAYSPEED_PAUSE;

CLog::Log(LOGDEBUG,"ASB: CalcDropRequirement INIT iClockSpeed: %i iDPlaySpeed: %i fFrameRate: %f fCurClock: %f fDInterval: %f iCalcId: %i fDPts: %f", iClockSpeed, iDPlaySpeed, fFrameRate, fCurClock, fDInterval, iCalcId, fDPts);

  // if paused or in transient speed change just reset anything sensible and return 0
  if (iClockSpeed != iDPlaySpeed || iDPlaySpeed == DVD_PLAYSPEED_PAUSE || iClockSpeed == DVD_PLAYSPEED_PAUSE)
  {
     m_dropinfo.iVeryLateCount = 0;
     return 0;
  }

  // if playspeed or frame rate change etc reset sample buffers
  if (m_dropinfo.iPlaySpeed != iDPlaySpeed ||
      m_dropinfo.fFrameRate != fFrameRate || m_dropinfo.fDInterval != fDInterval || bClockInterrupted)
  {
CLog::Log(LOGDEBUG,"ASB: CalcDropRequirement CHANGE RESET iClockSpeed: %i iDPlaySpeed: %i fFrameRate: %f m_dropinfo.fFrameRate: %f m_dropinfo.fDInterval: %f fDInterval: %f", iClockSpeed, iDPlaySpeed, fFrameRate, m_dropinfo.fFrameRate, m_dropinfo.fDInterval, fDInterval);
     for (int i = 0; i < DROPINFO_SAMPBUFSIZE; i++)
     {
	m_dropinfo.fLatenessSamp[i] = 0.0;
	m_dropinfo.fClockSamp[i] = DVD_NOPTS_VALUE;
	m_dropinfo.iDecoderDropSamp[i] = 0;
	m_dropinfo.iCalcIdSamp[i] = 0;
     }
     m_dropinfo.iCurSampIdx = 0;
     m_dropinfo.fDropRatio = 0.0;           
     if (m_dropinfo.iPlaySpeed != iDPlaySpeed && 
         m_dropinfo.fFrameRate == fFrameRate && m_dropinfo.fDInterval == fDInterval)
     {
        // for slightly better accuracy after speed changes at normal, 2x, 4x restore last drop ratio
        if (iDPlaySpeed == DVD_PLAYSPEED_NORMAL)
            m_dropinfo.fDropRatio = m_dropinfo.fDropRatioLastNormal;
        else if (iDPlaySpeed == 2 * DVD_PLAYSPEED_NORMAL)
            m_dropinfo.fDropRatio = m_dropinfo.fDropRatioLast2X;
        else if (iDPlaySpeed == 4 * DVD_PLAYSPEED_NORMAL)
            m_dropinfo.fDropRatio = m_dropinfo.fDropRatioLast4X;
     }
     m_dropinfo.iPlaySpeed = iDPlaySpeed;
     m_dropinfo.fDInterval = fDInterval;
     m_dropinfo.fFrameRate = fFrameRate;
     m_dropinfo.iVeryLateCount = 0;
CLog::Log(LOGDEBUG,"ASB: CalcDropRequirement CHANGE RESET DONE iClockSpeed: %i iDPlaySpeed: %i fFrameRate: %f m_dropinfo.fFrameRate: %f m_dropinfo.fDInterval: %f fDInterval: %f", iClockSpeed, iDPlaySpeed, fFrameRate, m_dropinfo.fFrameRate, m_dropinfo.fDInterval, fDInterval);
  }

  if (iDPlaySpeed > 0) 
  {
     // store sample info     
     m_dropinfo.fLatenessSamp[curidx] = fCurClock - fDPts;
     m_dropinfo.fClockSamp[curidx] = fCurClock;
     m_dropinfo.iDecoderDropSamp[curidx] = iDecoderDrops;
     m_dropinfo.iCalcIdSamp[curidx] = iCalcId;
     m_dropinfo.iCurSampIdx = (curidx + 1) % DROPINFO_SAMPBUFSIZE;

     double fLateness = fCurClock - fDPts - fLatenessThreshold;     
CLog::Log(LOGDEBUG,"ASB: CalcDropRequirement iDPlaySpeed > 0 curidx: %i m_dropinfo.fLatenessSamp[curidx]: %f, fCurClock: %f m_dropinfo.fClockSamp[curidx]: %f m_dropinfo.iDecoderDropSamp[curidx]: %i fLateness: %f", curidx, m_dropinfo.fLatenessSamp[curidx], fCurClock, m_dropinfo.fClockSamp[curidx], m_dropinfo.iDecoderDropSamp[curidx], fLateness);

     if (Now - m_dropinfo.iLastOutputPictureTime > 150 * CurrentHostFrequency() / 1000 || m_iNrOfPicturesNotToSkip > 0)
     {
        //we aim to not drop at all when we have not output in say 150ms or we have been asked not to skip
CLog::Log(LOGDEBUG,"ASB: CalcDropRequirement CurrentHostCounter() CurrentHostCounter(): %"PRId64", m_dropinfo.iLastOutputPictureTime: %"PRId64"", CurrentHostCounter(), m_dropinfo.iLastOutputPictureTime);
        m_dropinfo.iDropNextFrame = 0;
     }
     else if (!m_bAllowDrop)
     {
CLog::Log(LOGDEBUG,"ASB: CalcDropRequirement (!m_bAllowDrop)");
        // drop on output at intervals in line with lateness
        m_dropinfo.iDropNextFrame = 0;
        if (fLateness > DVD_MSEC_TO_TIME(50))
           iDropRequestDistance = 4;
        if (fLateness > DVD_MSEC_TO_TIME(100))
           iDropRequestDistance = 3;
        else if (fLateness > DVD_MSEC_TO_TIME(150))
           iDropRequestDistance = 2;
        else if (fLateness > DVD_MSEC_TO_TIME(200))
           iDropRequestDistance = 1;
        if (iDropRequestDistance > 0 && iLastOutputDropRequestCalcId + iDropRequestDistance < iCalcId)
           m_dropinfo.iDropNextFrame = DC_OUTPUT;
     }
     // next do various degrees of very late checks, with appropriate dropping severity 
     else if (fLateness > DVD_MSEC_TO_TIME(500))
     {
        // drop urgently in decoder and on output too if we did not drop on output last time
        CLog::Log(LOGDEBUG,"ASB: CalcDropRequirement fLateness > DVD_MSEC_TO_TIME(500)");
        if (m_dropinfo.iSuccessiveOutputDropRequests == 1)
           m_dropinfo.iDropNextFrame = DC_DECODER | DC_URGENT;
        else
           m_dropinfo.iDropNextFrame = DC_DECODER | DC_URGENT | DC_OUTPUT;    
        m_dropinfo.fDropRatio = std::max(m_dropinfo.fDropRatio, 0.1);
        m_dropinfo.iVeryLateCount++;
     }
     else if (fLateness > DVD_MSEC_TO_TIME(250))
     {
        CLog::Log(LOGDEBUG,"ASB: CalcDropRequirement fLateness > DVD_MSEC_TO_TIME(250)");
        // decoder drop request skipping this every 4 or 5
        if (m_dropinfo.iSuccessiveDecoderDropRequests <= 3 + iOsc + iDropMore)
           m_dropinfo.iDropNextFrame = DC_DECODER; 
        else
           m_dropinfo.iDropNextFrame = 0;
        m_dropinfo.fDropRatio = std::max(m_dropinfo.fDropRatio, 0.08);
        m_dropinfo.iVeryLateCount++;
     }
     else if (fLateness > DVD_MSEC_TO_TIME(150))
     {
        // decoder drop request skipping this every 3 or 4
        if (m_dropinfo.iSuccessiveDecoderDropRequests <= 2 + iOsc + iDropMore)
           m_dropinfo.iDropNextFrame = DC_DECODER; 
        else
           m_dropinfo.iDropNextFrame = 0;
        m_dropinfo.fDropRatio = std::max(m_dropinfo.fDropRatio, 0.05);
        m_dropinfo.iVeryLateCount++;
     }
     else if (fLateness > DVD_MSEC_TO_TIME(100))
     {
        // decoder drop request skipping this every 2 or 3
        if (m_dropinfo.iSuccessiveDecoderDropRequests <= 1 + iOsc + iDropMore)
           m_dropinfo.iDropNextFrame = DC_DECODER; 
        else
           m_dropinfo.iDropNextFrame = 0;
        m_dropinfo.fDropRatio = std::max(m_dropinfo.fDropRatio, 0.04);
        m_dropinfo.iVeryLateCount++;
     }
     // else try to get an average over at least 250ms if constant dropping at more than (1 / 50) or 1 second otherwise
     // in order to determine the finer adjustments
     else if (iLastDecoderDropRequestCalcId > 0) //at least one previous sample
     {
        // now we want to try to calculate lateness averages over two sample periods
        // to have enough samples we should also have at reasonable number of samples in each set
        int i = curidx;
        bool bGotSamplesT = false;
        bool bGotSamplesTMinus1 = false;
        double fSampleSumT = 0.0;
        double fSampleSumTMinus1 = 0.0;
        int iSampleNumT = 0;
        int iSampleNumTMinus1 = 0;
        int iBoundaryIdx = -1; //index of earliest sample in set of T
        double fClockMidT = 0.0;
        double fClockMidTMinus1 = 0.0;
        int iDecoderDropsT = 0;
        int iDecoderDropsTMinus1 = 0;
        double fDecoderDropRatioT = 0.0;
        double fDecoderDropRatioTMinus1 = 0.0;
        double fLatenessAvgT = 0.0;
        double fLatenessAvgTMinus1 = 0.0;
        double fPrevClockSamp = DVD_NOPTS_VALUE;
        
        int iSampleDistance = 10;
        double fClockDistance = DVD_MSEC_TO_TIME(1000);
        if (m_dropinfo.fDropRatio > fDropRatioThreshold)
        {
           iSampleDistance = 5;
           fClockDistance = DVD_MSEC_TO_TIME(250);
        }

        while (!bGotSamplesTMinus1)
        {
//CLog::Log(LOGDEBUG,"ASB: CalcDropRequirement while (!bGotSamplesTMinus1) i: %i m_dropinfo.fClockSamp[i]: %f curidx: %i", i, m_dropinfo.fClockSamp[i], curidx);
           if (m_dropinfo.fClockSamp[i] == DVD_NOPTS_VALUE)
              break; // assume we have moved back to a slot with no sample
           if (fPrevClockSamp != DVD_NOPTS_VALUE && (fPrevClockSamp - m_dropinfo.fClockSamp[i] > DVD_MSEC_TO_TIME(500) || m_dropinfo.fClockSamp[i] > fPrevClockSamp))
           {
CLog::Log(LOGDEBUG,"ASB: CalcDropRequirement fPrevClockSamp: %f m_dropinfo.fClockSamp[i]: %f", fPrevClockSamp, m_dropinfo.fClockSamp[i]);
              break; // assume we have moved back to a sample that shows a discontinuity (eg seek or speed change) 
           }

           if (!bGotSamplesT) //accumulate samples for time T
           {
              iSampleNumT++;
              fSampleSumT += m_dropinfo.fLatenessSamp[i];
              if (fCurClock - m_dropinfo.fClockSamp[i] >= fClockDistance && iSampleNumT >= iSampleDistance)
              {
                 bGotSamplesT = true; 
                 iBoundaryIdx = i;
                 fClockMidT = (fCurClock - m_dropinfo.fClockSamp[iBoundaryIdx]) / 2 + m_dropinfo.fClockSamp[iBoundaryIdx];
                 iDecoderDropsT = iDecoderDrops - m_dropinfo.iDecoderDropSamp[i]; //drops during T
                 fDecoderDropRatioT = (double)iDecoderDropsT / (iCalcId - m_dropinfo.iCalcIdSamp[iBoundaryIdx]);
              }
           }
           else //accumulate samples for times for time T-1 
           {
              iSampleNumTMinus1++;
              fSampleSumTMinus1 += m_dropinfo.fLatenessSamp[i];
              if (m_dropinfo.fClockSamp[iBoundaryIdx] - m_dropinfo.fClockSamp[i] >= fClockDistance && iSampleNumTMinus1 >= iSampleDistance)
              {
                 bGotSamplesTMinus1 = true; 
                 fClockMidTMinus1 = (m_dropinfo.fClockSamp[iBoundaryIdx] - m_dropinfo.fClockSamp[i]) / 2 + m_dropinfo.fClockSamp[i];
                 iDecoderDropsTMinus1 = iDecoderDrops - iDecoderDropsT - m_dropinfo.iDecoderDropSamp[i]; //drops during TMinus1
                 fDecoderDropRatioTMinus1 = (double)iDecoderDropsTMinus1 / (m_dropinfo.iCalcIdSamp[iBoundaryIdx] - m_dropinfo.iCalcIdSamp[i]);
              }
           }

           fPrevClockSamp = m_dropinfo.fClockSamp[i];
           i = ((i - 1) + DROPINFO_SAMPBUFSIZE) % DROPINFO_SAMPBUFSIZE; //move index back one
           if (i == curidx)
              break; // failsafe to exit loop if we have wrapped
        }
        
        if (bGotSamplesT)            
           fLatenessAvgT = (fSampleSumT / iSampleNumT) - fLatenessThreshold;
        if (bGotSamplesTMinus1)
        {
           fLatenessAvgTMinus1 = (fSampleSumTMinus1 / iSampleNumTMinus1) - fLatenessThreshold;
           // now calculate drop ratio and update...
           // and always try to reduce current drop a little (so we attempt to force it down)
           double incr = 0.02 * std::max(0.0, (fLatenessAvgT - fLatenessAvgTMinus1) / (fClockMidT - fClockMidTMinus1));
           // if we are not getting later and we are not late then reduce m_dropinfo.fDropRatio with more rapidity
           if (fLatenessAvgT <= 0.0 && fLatenessAvgT <= fLatenessAvgTMinus1)
              m_dropinfo.fDropRatio = 0.99 * m_dropinfo.fDropRatio;
           else
              m_dropinfo.fDropRatio = std::max(0.0, (0.999 * m_dropinfo.fDropRatio) + incr);
           //m_dropinfo.fDropRatio = std::max(0.0, (fLatenessAvgT - fLatenessAvgTMinus1) / (fClockMidT - fClockMidTMinus1) + 
           //           0.95 * (fDecoderDropRatioT + fDecoderDropRatioTMinus1) / 2);
           m_dropinfo.fDropRatio = std::min(0.95, m_dropinfo.fDropRatio); //constrain to 0.95

           // store the new drop ratio in playspeed record if appropriate
           if (iDPlaySpeed == DVD_PLAYSPEED_NORMAL)
               m_dropinfo.fDropRatioLastNormal = m_dropinfo.fDropRatio;           
           else if (iDPlaySpeed == 2 * DVD_PLAYSPEED_NORMAL)
               m_dropinfo.fDropRatioLast2X = m_dropinfo.fDropRatio;           
           else if (iDPlaySpeed == 4 * DVD_PLAYSPEED_NORMAL)
               m_dropinfo.fDropRatioLast4X = m_dropinfo.fDropRatio;           
         
CLog::Log(LOGDEBUG,"ASB: CalcDropRequirement m_dropinfo.fDropRatio: %f fLatenessAvgTMinus1: %f fSampleSumTMinus1: %f iSampleNumTMinus1: %i fLatenessThreshold: %f fLatenessAvgT: %f fLatenessAvgTMinus1: %f fClockMidT: %f fClockMidTMinus1: %f fDecoderDropRatioT: %f fDecoderDropRatioTMinus1: %f", m_dropinfo.fDropRatio, fLatenessAvgTMinus1, fSampleSumTMinus1, iSampleNumTMinus1, fLatenessThreshold, fLatenessAvgT, fLatenessAvgTMinus1, fClockMidT, fClockMidTMinus1, fDecoderDropRatioT, fDecoderDropRatioTMinus1);
        }

        // now if drop ratio is low ignore it and if late proper choose a drop request interval based on lateness 
        if (bGotSamplesT && fLatenessAvgT > 0.0 && m_dropinfo.fDropRatio <= fDropRatioThreshold)
        {
           if (fLatenessAvgT > DVD_MSEC_TO_TIME(40))
              m_dropinfo.iVeryLateCount++;
           else if (fLatenessAvgT > 0.0)
              m_dropinfo.iVeryLateCount--;
           else
              m_dropinfo.iVeryLateCount = 0;

           if (fLatenessAvgT > DVD_MSEC_TO_TIME(50))
              iDropRequestDistance = 4;
           else if (fLatenessAvgT > fDInterval)
              iDropRequestDistance = 6;
           else 
              iDropRequestDistance = 8;
     
           //drop request at required distance or if it seems last drop request did not succeed and we are quite late
           if ((iLastDecoderDropRequestCalcId + iDropRequestDistance + iOsc < iCalcId) || 
                 (iLastDecoderDropRequestDecoderDrops == iDecoderDrops && fLatenessAvgT > fDInterval &&
                   iLastDecoderDropRequestCalcId + 5 < iCalcId))
           {
              // try to drop subtle (eg do post decode decoder drop when interlaced if supported in codec decode)
              // if less than a display interval
              if (fLatenessAvgT > fDInterval)                 
                 m_dropinfo.iDropNextFrame = DC_DECODER;
              else
                 m_dropinfo.iDropNextFrame = DC_DECODER | DC_SUBTLE;
           }
        }
        else if (bGotSamplesT && m_dropinfo.fDropRatio > fDropRatioThreshold)
        {
           if (fLatenessAvgT > DVD_MSEC_TO_TIME(40))
              m_dropinfo.iVeryLateCount++;
           else if (fLatenessAvgT > 0.0)
              m_dropinfo.iVeryLateCount--;
           else
              m_dropinfo.iVeryLateCount = 0;

           // start with a drop distance based on fDropRatio directly
           iDropRequestDistance = (int)(1.0 / m_dropinfo.fDropRatio) - 1;
           if (fLatenessAvgT > DVD_MSEC_TO_TIME(50))
              iDropRequestDistance = std::max(0, iDropRequestDistance - iDropMore);
              //iDropRequestDistance = std::max(0, (iDropRequestDistance / 4));
           else if (fLatenessAvgT > fDInterval)
           {
              iDropRequestDistance = std::max(1, std::min(iDropRequestDistance - iDropMore, iDropRequestDistance / 3));
              //iDropRequestDistance = std::max(1, (iDropRequestDistance / 3));
           }
           else if (fLatenessAvgT > 0.0)
              iDropRequestDistance = std::max(1, (iDropRequestDistance / 2));
           //iDropRequestDistance = std::max(1, iDropRequestDistance - iDropMore);

           bool bDropEarly = false;
           if ((fLatenessAvgT > -(fDInterval / 5)) ||
               (iDropRequestDistance <= 15 && fLatenessAvgT > -(fDInterval / 2)) ||
               (iDropRequestDistance <= 10 && fLatenessAvgT > -fDInterval) ||
               (iDropRequestDistance <= 5 && fLatenessAvgT > -fDInterval * 1.5) ||
               (iDropRequestDistance <= 3 && fLatenessAvgT > -fDInterval * 2) ||
               (iDropRequestDistance <= 2 && fLatenessAvgT > -fDInterval * 2.5))
              bDropEarly = true;

           if ((bDropEarly && iCalcId - iLastDecoderDropRequestCalcId - iDropRequestDistance > iOsc) || 
                 (iLastDecoderDropRequestDecoderDrops == iDecoderDrops && iLastDecoderDropRequestCalcId + 2 < iCalcId))
              m_dropinfo.iDropNextFrame = DC_DECODER;
        }
        else if (bGotSamplesT && fLatenessAvgT <= 0.0 )// we are not late
        {
              m_dropinfo.iVeryLateCount = 0;
        }
           // if we have not requested a drop so far and we are not late check the drift, update the dropPS and drop if we are close to being late and current dropPS dictates?
CLog::Log(LOGDEBUG,"ASB: CalcDropRequirement bGotSamplesT: %i bGotSamplesTMinus1: %i iLastDecoderDropRequestCalcId: %i fLatenessAvgT: %f fLatenessAvgTMinus1: %f iOsc: %i iCalcId: %i m_dropinfo.iDropNextFrame: %i iDropRequestDistance: %i iDropMore: %i m_dropinfo.fDropRatio: %f", (int)bGotSamplesT, (int)bGotSamplesTMinus1, iLastDecoderDropRequestCalcId, fLatenessAvgT, fLatenessAvgTMinus1, iOsc, iCalcId, m_dropinfo.iDropNextFrame, iDropRequestDistance, iDropMore, m_dropinfo.fDropRatio);
     }
  } 
  else if (iDPlaySpeed < 0) 
  { 
     // don't add sample if playing backwards and simply drop at fixed rate based on system clock
     // dvd player itself will seek to correct itself anyway
     // target: don't drop only if we last output a picture more than 100ms ago
     if (Now - m_dropinfo.iLastOutputPictureTime > 100 * CurrentHostFrequency() / 1000)
        m_dropinfo.iDropNextFrame = DC_DECODER;
     m_dropinfo.iVeryLateCount = 0;
  }

  // if we are requesting a decoder drop this time around, record our id for it and 
  // the current decoder drop count so that some call later we can see if this drop request worked
  if (m_dropinfo.iDropNextFrame & DC_DECODER)
  {
     // adjust the oscillator
     m_dropinfo.iLastDecoderDropRequestDecoderDrops = iDecoderDrops;
     m_dropinfo.iLastDecoderDropRequestCalcId = iCalcId;
     m_dropinfo.iOscillator = (m_dropinfo.iOscillator + 1) % 2;
  }
  if (m_dropinfo.iDropNextFrame & DC_OUTPUT)
  {
     // adjust the oscillator
     m_dropinfo.iLastOutputDropRequestCalcId = iCalcId;
  }
  // update the successive drop request value to expose simply
  if (m_dropinfo.iDropNextFrame & DC_DECODER)
     m_dropinfo.iSuccessiveDecoderDropRequests++;    
  else
     m_dropinfo.iSuccessiveDecoderDropRequests = 0;
  if (m_dropinfo.iDropNextFrame & DC_OUTPUT)
     m_dropinfo.iSuccessiveOutputDropRequests++;
  else
     m_dropinfo.iSuccessiveOutputDropRequests = 0;

  return m_dropinfo.iDropNextFrame;
}

void CDVDPlayerVideo::ResetDropInfo()
{
  m_dropinfo.fDropRatio = 0.0;
  m_dropinfo.fDropRatioLastNormal = 0.0;
  m_dropinfo.fDropRatioLast2X = 0.0;
  m_dropinfo.fDropRatioLast4X = 0.0;
  m_dropinfo.iCurSampIdx = 0;
  for (int i = 0; i < DROPINFO_SAMPBUFSIZE; i++)
  {
      m_dropinfo.fLatenessSamp[i] = 0.0;
      m_dropinfo.fClockSamp[i] = DVD_NOPTS_VALUE;
      m_dropinfo.iDecoderDropSamp[i] = 0;
      m_dropinfo.iCalcIdSamp[i] = 0;
  }
  m_dropinfo.fDInterval = 0.0;
  m_dropinfo.fFrameRate = 0.0;
  m_dropinfo.iPlaySpeed = DVD_PLAYSPEED_PAUSE;
  m_dropinfo.iLastDecoderDropRequestDecoderDrops = 0;
  m_dropinfo.iLastDecoderDropRequestCalcId = 0;
  m_dropinfo.iLastOutputDropRequestCalcId = 0;
  m_dropinfo.iCalcId = 1; //start at id==1
  m_dropinfo.iOscillator = 0;
  m_dropinfo.iLastOutputPictureTime = 0; //int64_t
  m_dropinfo.iDropNextFrame = 0; //bit mask DC_DECODER, DC_SUBTLE, DC_URGENT, DC_OUTPUT
  m_dropinfo.iVeryLateCount = 0;
  m_dropinfo.iSuccessiveDecoderDropRequests = 0;
  m_dropinfo.iSuccessiveOutputDropRequests = 0;
}

void CDVDPlayerVideo::OnExit()
{
  g_dvdPerformanceCounter.DisableVideoDecodePerformance();

  if (m_pOverlayCodecCC)
  {
    m_pOverlayCodecCC->Dispose();
    m_pOverlayCodecCC = NULL;
  }

  CLog::Log(LOGNOTICE, "thread end: video_thread");
}

void CDVDPlayerVideo::ProcessVideoUserData(DVDVideoUserData* pVideoUserData, double pts)
{
  // check userdata type
  BYTE* data = pVideoUserData->data;
  int size = pVideoUserData->size;

  if (size >= 2)
  {
    if (data[0] == 'C' && data[1] == 'C')
    {
      data += 2;
      size -= 2;

      // closed captioning
      if (!m_pOverlayCodecCC)
      {
        m_pOverlayCodecCC = new CDVDOverlayCodecCC();
        CDVDCodecOptions options;
        CDVDStreamInfo info;
        if (!m_pOverlayCodecCC->Open(info, options))
        {
          delete m_pOverlayCodecCC;
          m_pOverlayCodecCC = NULL;
        }
      }

      if (m_pOverlayCodecCC)
      {
        m_pOverlayCodecCC->Decode(data, size, DVD_NOPTS_VALUE, DVD_NOPTS_VALUE);

        CDVDOverlay* overlay;
        while((overlay = m_pOverlayCodecCC->GetOverlay()) != NULL)
        {
          overlay->iGroupId = 0;
          overlay->iPTSStartTime += pts;
          if(overlay->iPTSStopTime != 0.0)
            overlay->iPTSStopTime += pts;

          m_pOverlayContainer->Add(overlay);
          overlay->Release();
        }
      }
    }
  }
}

bool CDVDPlayerVideo::InitializedOutputDevice()
{
#ifdef HAS_VIDEO_PLAYBACK
  return g_renderManager.IsStarted();
#else
  return false;
#endif
}

void CDVDPlayerVideo::SetSpeed(int speed)
{
  if(m_messageQueue.IsInited())
    m_messageQueue.Put( new CDVDMsgInt(CDVDMsg::PLAYER_SETSPEED, speed), 1 );
  else
    m_speed = speed;
}

void CDVDPlayerVideo::StepFrame()
{
  m_iNrOfPicturesNotToSkip++;
}

void CDVDPlayerVideo::Flush()
{
  /* flush using message as this get's called from dvdplayer thread */
  /* and any demux packet that has been taken out of queue need to */
  /* be disposed of before we flush */
  m_messageQueue.Flush();
  m_messageQueue.Put(new CDVDMsg(CDVDMsg::GENERAL_FLUSH), 1);
}

#ifdef HAS_VIDEO_PLAYBACK
void CDVDPlayerVideo::ProcessOverlays(DVDVideoPicture* pSource, YV12Image* pDest, double pts)
{
  // remove any overlays that are out of time
  m_pOverlayContainer->CleanUp(pts - m_iSubtitleDelay);

  enum EOverlay
  { OVERLAY_AUTO // select mode auto
  , OVERLAY_GPU  // render osd using gpu
  , OVERLAY_VID  // render osd directly on video memory
  , OVERLAY_BUF  // render osd on buffer
  } render = OVERLAY_AUTO;

  if(render == OVERLAY_AUTO)
  {
    render = OVERLAY_GPU;

#ifdef _LINUX
    // for now use cpu for ssa overlays as it currently allocates and
    // frees textures for each frame this causes a hugh memory leak
    // on some mesa intel drivers
    if(m_pOverlayContainer->ContainsOverlayType(DVDOVERLAY_TYPE_SSA) && pSource->format == DVDVideoPicture::FMT_YUV420P)
      render = OVERLAY_VID;
#endif

    if(render == OVERLAY_VID)
    {
      if( m_pOverlayContainer->ContainsOverlayType(DVDOVERLAY_TYPE_SPU)
       || m_pOverlayContainer->ContainsOverlayType(DVDOVERLAY_TYPE_IMAGE)
       || m_pOverlayContainer->ContainsOverlayType(DVDOVERLAY_TYPE_SSA) )
        render = OVERLAY_BUF;
    }
  }

  if(pSource->format == DVDVideoPicture::FMT_YUV420P)
  {
    if(render == OVERLAY_BUF)
    {
      // rendering spu overlay types directly on video memory costs a lot of processing power.
      // thus we allocate a temp picture, copy the original to it (needed because the same picture can be used more than once).
      // then do all the rendering on that temp picture and finaly copy it to video memory.
      // In almost all cases this is 5 or more times faster!.

      if(m_pTempOverlayPicture && ( m_pTempOverlayPicture->iWidth  != pSource->iWidth
                                 || m_pTempOverlayPicture->iHeight != pSource->iHeight))
      {
        CDVDCodecUtils::FreePicture(m_pTempOverlayPicture);
        m_pTempOverlayPicture = NULL;
      }

      if(!m_pTempOverlayPicture)
        m_pTempOverlayPicture = CDVDCodecUtils::AllocatePicture(pSource->iWidth, pSource->iHeight);
      if(!m_pTempOverlayPicture)
        return;

      CDVDCodecUtils::CopyPicture(m_pTempOverlayPicture, pSource);
    }
    else
    {
      AutoCrop(pSource);
      CDVDCodecUtils::CopyPicture(pDest, pSource);
    }
  }

  m_pOverlayContainer->Lock();

  VecOverlays* pVecOverlays = m_pOverlayContainer->GetOverlays();
  VecOverlaysIter it = pVecOverlays->begin();

  //Check all overlays and render those that should be rendered, based on time and forced
  //Both forced and subs should check timeing, pts == 0 in the stillframe case
  while (it != pVecOverlays->end())
  {
    CDVDOverlay* pOverlay = *it++;
    if(!pOverlay->bForced && !m_bRenderSubs)
      continue;

    if(pOverlay->iGroupId != pSource->iGroupId)
      continue;

    double pts2 = pOverlay->bForced ? pts : pts - m_iSubtitleDelay;

    if((pOverlay->iPTSStartTime <= pts2 && (pOverlay->iPTSStopTime > pts2 || pOverlay->iPTSStopTime == 0LL)) || pts == 0)
    {
      if (render == OVERLAY_GPU)
        g_renderManager.AddOverlay(pOverlay, pts2);

      if(pSource->format == DVDVideoPicture::FMT_YUV420P)
      {
        if     (render == OVERLAY_BUF)
          CDVDOverlayRenderer::Render(m_pTempOverlayPicture, pOverlay, pts2);
        else if(render == OVERLAY_VID)
          CDVDOverlayRenderer::Render(pDest, pOverlay, pts2);
      }

    }
  }

  m_pOverlayContainer->Unlock();

  if(pSource->format == DVDVideoPicture::FMT_YUV420P)
  {
    if(render == OVERLAY_BUF)
    {
      AutoCrop(m_pTempOverlayPicture);
      CDVDCodecUtils::CopyPicture(pDest, m_pTempOverlayPicture);
    }
  }
  else if(pSource->format == DVDVideoPicture::FMT_NV12)
  {
    AutoCrop(pSource);
    CDVDCodecUtils::CopyNV12Picture(pDest, pSource);
  }
  else if(pSource->format == DVDVideoPicture::FMT_YUY2 || pSource->format == DVDVideoPicture::FMT_UYVY)
  {
    AutoCrop(pSource);
    CDVDCodecUtils::CopyYUV422PackedPicture(pDest, pSource);
  }
#ifdef HAS_DX
  else if(pSource->format == DVDVideoPicture::FMT_DXVA)
    g_renderManager.AddProcessor(pSource->proc, pSource->proc_id);
#endif
#ifdef HAVE_LIBVDPAU
  else if(pSource->format == DVDVideoPicture::FMT_VDPAU || pSource->format == DVDVideoPicture::FMT_VDPAU_420)
  {
    if (pSource->vdpau)
{
CLog::Log(LOGDEBUG,"ASB: ProcessOverlays about to pSource->vdpau->Present() clock: %f", m_pClock->GetClock());
      pSource->vdpau->Present();
}
    g_renderManager.AddProcessor(pSource->vdpau);
  }
#endif
#ifdef HAVE_LIBOPENMAX
  else if(pSource->format == DVDVideoPicture::FMT_OMXEGL)
    g_renderManager.AddProcessor(pSource->openMax, pSource);
#endif
#ifdef HAVE_VIDEOTOOLBOXDECODER
  else if(pSource->format == DVDVideoPicture::FMT_CVBREF)
    g_renderManager.AddProcessor(pSource->vtb, pSource);
#endif
#ifdef HAVE_LIBVA
  else if(pSource->format == DVDVideoPicture::FMT_VAAPI)
    g_renderManager.AddProcessor(*pSource->vaapi);
#endif
}
#endif

int CDVDPlayerVideo::OutputPicture(DVDVideoPicture* pPicture, double pts)
{
#ifdef HAS_VIDEO_PLAYBACK
  /* check so that our format or aspect has changed. if it has, reconfigure renderer */
  if (!g_renderManager.IsConfigured()
   || m_output.width != pPicture->iWidth
   || m_output.height != pPicture->iHeight
   || m_output.dwidth != pPicture->iDisplayWidth
   || m_output.dheight != pPicture->iDisplayHeight
   || m_output.framerate != m_fFrameRate
   || m_output.color_format != (unsigned int)pPicture->format
   || ( m_output.color_matrix != pPicture->color_matrix && pPicture->color_matrix != 0 ) // don't reconfigure on unspecified
   || m_output.color_range != pPicture->color_range)
  {
    CLog::Log(LOGNOTICE, " fps: %f, pwidth: %i, pheight: %i, dwidth: %i, dheight: %i",
      m_fFrameRate, pPicture->iWidth, pPicture->iHeight, pPicture->iDisplayWidth, pPicture->iDisplayHeight);
    unsigned flags = 0;
    if(pPicture->color_range == 1)
      flags |= CONF_FLAGS_YUV_FULLRANGE;

    switch(pPicture->color_matrix)
    {
      case 7: // SMPTE 240M (1987)
        flags |= CONF_FLAGS_YUVCOEF_240M;
        break;
      case 6: // SMPTE 170M
      case 5: // ITU-R BT.470-2
      case 4: // FCC
        flags |= CONF_FLAGS_YUVCOEF_BT601;
        break;
      case 1: // ITU-R Rec.709 (1990) -- BT.709
        flags |= CONF_FLAGS_YUVCOEF_BT709;
        break;
      case 3: // RESERVED
      case 2: // UNSPECIFIED
      default:
        if(pPicture->iWidth > 1024 || pPicture->iHeight >= 600)
          flags |= CONF_FLAGS_YUVCOEF_BT709;
        else
          flags |= CONF_FLAGS_YUVCOEF_BT601;
        break;
    }

    CStdString formatstr;

    switch(pPicture->format)
    {
      case DVDVideoPicture::FMT_YUV420P:
        flags |= CONF_FLAGS_FORMAT_YV12;
        formatstr = "YV12";
        break;
      case DVDVideoPicture::FMT_NV12:
        flags |= CONF_FLAGS_FORMAT_NV12;
        formatstr = "NV12";
        break;
      case DVDVideoPicture::FMT_UYVY:
        flags |= CONF_FLAGS_FORMAT_UYVY;
        formatstr = "UYVY";
        break;
      case DVDVideoPicture::FMT_YUY2:
        flags |= CONF_FLAGS_FORMAT_YUY2;
        formatstr = "YUY2";
        break;
      case DVDVideoPicture::FMT_VDPAU:
        flags |= CONF_FLAGS_FORMAT_VDPAU;
        formatstr = "VDPAU";
        break;
      case DVDVideoPicture::FMT_VDPAU_420:
        flags |= CONF_FLAGS_FORMAT_VDPAU_420;
        formatstr = "VDPAU_420";
        break;
      case DVDVideoPicture::FMT_DXVA:
        flags |= CONF_FLAGS_FORMAT_DXVA;
        formatstr = "DXVA";
        break;
      case DVDVideoPicture::FMT_VAAPI:
        flags |= CONF_FLAGS_FORMAT_VAAPI;
        formatstr = "VAAPI";
        break;
      case DVDVideoPicture::FMT_OMXEGL:
        flags |= CONF_FLAGS_FORMAT_OMXEGL;
        break;
      case DVDVideoPicture::FMT_CVBREF:
        flags |= CONF_FLAGS_FORMAT_CVBREF;
        formatstr = "BGRA";
        break;
    }

    if(m_bAllowFullscreen)
    {
      flags |= CONF_FLAGS_FULLSCREEN;
      m_bAllowFullscreen = false; // only allow on first configure
    }

    CLog::Log(LOGDEBUG,"%s - change configuration. %dx%d. framerate: %4.2f. format: %s",__FUNCTION__,pPicture->iWidth, pPicture->iHeight,m_fFrameRate, formatstr.c_str());
    if(!g_renderManager.Configure(pPicture->iWidth, pPicture->iHeight, pPicture->iDisplayWidth, pPicture->iDisplayHeight, m_fFrameRate, flags))
    {
      CLog::Log(LOGERROR, "%s - failed to configure renderer", __FUNCTION__);
      return EOS_ABORT;
    }

    m_output.width = pPicture->iWidth;
    m_output.height = pPicture->iHeight;
    m_output.dwidth = pPicture->iDisplayWidth;
    m_output.dheight = pPicture->iDisplayHeight;
    m_output.framerate = m_fFrameRate;
    m_output.color_format = pPicture->format;
    m_output.color_matrix = pPicture->color_matrix;
    m_output.color_range = pPicture->color_range;
   
    // TODO: get the more sophisticated resync implemented that audio player and dvd player main is
    //       integrated with
    // 1. need to ensure this does not wait when a reconfigure is called that does not warrant a resync
    // 2. need to set the clock just behind our pts (at least 2 frames say)
    // 3. need to get audio to wait and prepare its position especially if sync mode is "audio clock"
    if (!g_VideoReferenceClock.WaitStable(3000))
      CLog::Log(LOGWARNING, "%s g_VideoReferenceClock didn't reach stability, continuing anyway", __FUNCTION__);
    CLog::Log(LOGDEBUG,"ASB: OutputPicture done reconfigure section, pts: %f", pts);
    //m_pClock->Discontinuity(pts - 2*frametime);
  }

  double maxfps  = 60.0;
  //bool   limited = false;
  int    result  = 0;

  if (!g_renderManager.IsStarted()) {
    CLog::Log(LOGERROR, "%s - renderer not started", __FUNCTION__);
    return EOS_ABORT;
  }
  maxfps = g_renderManager.GetMaximumFPS();

/*
  // check if our output will limit speed
  if(m_fFrameRate * abs(m_speed) / DVD_PLAYSPEED_NORMAL > maxfps*0.9)
    limited = true;
*/

  //correct any pattern in the timestamps
  m_pullupCorrection.Add(pts);
  pts += m_pullupCorrection.GetCorrection();

  //try to calculate the framerate
  CalcFrameRate();

  // signal to clock what our framerate is, it may want to adjust it's
  // speed to better match with our video renderer's output speed
  double interval;
  int refreshrate = m_pClock->UpdateFramerate(m_fFrameRate, &interval);
  if (refreshrate > 0) //refreshrate of -1 means the videoreferenceclock is not running
  {//when using the videoreferenceclock, a frame is always presented half a vblank interval too late
    //pts -= DVD_TIME_BASE * interval;
  }

  //User set delay
  pts += m_iVideoDelay;

  // calculate the time we need to delay this picture before displaying
  double iSleepTime, iClockSleep, iFrameSleep, iCurrentClock, iFrameDuration;

  //iCurrentClock = m_pClock->GetAbsoluteClock(); // snapshot current clock
  double gc = m_pClock->GetClock(&iCurrentClock);
  //iClockSleep = pts - m_pClock->GetClock();  //sleep calculated by pts to clock comparison
  iClockSleep = pts - gc;
  iFrameSleep = m_FlipTimeStamp - iCurrentClock; // sleep calculated by duration of frame
  iFrameDuration = pPicture->iDuration;

  // correct sleep times based on speed
  if(m_speed)
  {
    iClockSleep = iClockSleep * DVD_PLAYSPEED_NORMAL / m_speed;
    iFrameSleep = iFrameSleep * DVD_PLAYSPEED_NORMAL / abs(m_speed);
    iFrameDuration = iFrameDuration * DVD_PLAYSPEED_NORMAL / abs(m_speed);
  }
  else
  {
    iClockSleep = 0;
    iFrameSleep = 0;
  }

  // dropping to a very low framerate is not correct (it should not happen at all)
  iClockSleep = min(iClockSleep, DVD_MSEC_TO_TIME(500));
  iFrameSleep = min(iFrameSleep, DVD_MSEC_TO_TIME(500));

  if( m_stalled )
    iSleepTime = iFrameSleep;
  else
    iSleepTime = iFrameSleep + (iClockSleep - iFrameSleep) / m_autosync;

#ifdef PROFILE /* during profiling, try to play as fast as possible */
  iSleepTime = 0;
#endif

  // present the current pts of this frame to user, and include the actual
  // presentation delay, to allow him to adjust for it
  if( m_stalled )
    m_iCurrentPts = DVD_NOPTS_VALUE;
  else
    m_iCurrentPts = pts - max(0.0, iSleepTime);
  m_iCurrentPtsClock = iCurrentClock; //interpolated time

  // timestamp when we think next picture should be displayed based on current duration
  m_FlipTimeStamp  = iCurrentClock;
  m_FlipTimeStamp += max(0.0, iSleepTime);
  m_FlipTimeStamp += iFrameDuration;

  if( (pPicture->iFlags & DVP_FLAG_DROPPED) )
    return result | EOS_DROPPED;

  // set fieldsync if picture is interlaced
  EFIELDSYNC mDisplayField = FS_NONE;
  if( pPicture->iFlags & DVP_FLAG_INTERLACED )
  {
    if( pPicture->iFlags & DVP_FLAG_TOP_FIELD_FIRST )
      mDisplayField = FS_TOP;
    else
      mDisplayField = FS_BOT;
  }

CLog::Log(LOGDEBUG,"ASB: OutputPicture about to g_renderManager.WaitVdpauFlip clock: %f", m_pClock->GetClock());
//move further down as returning EOS_DROPPED prevents pts pattern watching if too early
// TODO: make this less vdpau centric in here (wrap into g_renderManager.WaitHardwareFlip()?)
  if (pPicture->format == DVDVideoPicture::FMT_VDPAU || pPicture->format == DVDVideoPicture::FMT_VDPAU_420)
  {
    if (!g_renderManager.WaitVdpauFlip(500))
    {
      return EOS_DROPPED;
    }
  }
CLog::Log(LOGDEBUG,"ASB: OutputPicture g_renderManager.WaitVdpauFlip finished clock: %f", m_pClock->GetClock());
//if (pts > 260000.0)
//  return EOS_DROPPED;


  // copy picture to overlay
  YV12Image image;

  int index = g_renderManager.GetImage(&image);

  // video device might not be done yet
  while (index < 0 && !CThread::m_bStop &&
         CDVDClock::GetAbsoluteClock() < iCurrentClock + iSleepTime + DVD_MSEC_TO_TIME(500) )
  {
    Sleep(1);
    index = g_renderManager.GetImage(&image);
  }

  if (index < 0)
    return EOS_DROPPED;

  ProcessOverlays(pPicture, &image, pts);

CLog::Log(LOGDEBUG,"ASB: OutputPicture ProcessOverlays finished clock: %f", m_pClock->GetClock());
  // tell the renderer that we've finished with the image (so it can do any
  // post processing before FlipPage() is called.)
  g_renderManager.ReleaseImage(index);

/*
double tickgc, tickac;
tickgc = m_pClock->GetClockTick(&tickac);
CLog::Log(LOGDEBUG,"ASB: OutputPicture iCurrentClock + iSleepTime: %f, tickac + pts - tickgc: %f, iCurrentClock: %f tickgc: %f tickac: %f pts: %f", iCurrentClock + iSleepTime, tickac + pts - tickgc, iCurrentClock, tickgc, tickac, pts);
*/
  if (pPicture->format == DVDVideoPicture::FMT_VDPAU || pPicture->format == DVDVideoPicture::FMT_VDPAU_420)
  {
    g_renderManager.FlipPage(CThread::m_bStop, (iCurrentClock + iSleepTime) / DVD_TIME_BASE, index, mDisplayField, true);
  }
  else
    g_renderManager.FlipPage(CThread::m_bStop, (iCurrentClock + iSleepTime) / DVD_TIME_BASE, index, mDisplayField);

  return result;
#else
  // no video renderer, let's mark it as dropped
  return EOS_DROPPED;
#endif
}

void CDVDPlayerVideo::AutoCrop(DVDVideoPicture *pPicture)
{
  if ((pPicture->format == DVDVideoPicture::FMT_YUV420P) ||
     (pPicture->format == DVDVideoPicture::FMT_NV12) ||
     (pPicture->format == DVDVideoPicture::FMT_YUY2) ||
     (pPicture->format == DVDVideoPicture::FMT_UYVY) ||
     (pPicture->format == DVDVideoPicture::FMT_VDPAU_420))
  {
    RECT crop;

    if (g_settings.m_currentVideoSettings.m_Crop)
      AutoCrop(pPicture, crop);
    else
    { // reset to defaults
      crop.left   = 0;
      crop.right  = 0;
      crop.top    = 0;
      crop.bottom = 0;
    }

    m_crop.x1 += ((float)crop.left   - m_crop.x1) * 0.1;
    m_crop.x2 += ((float)crop.right  - m_crop.x2) * 0.1;
    m_crop.y1 += ((float)crop.top    - m_crop.y1) * 0.1;
    m_crop.y2 += ((float)crop.bottom - m_crop.y2) * 0.1;

    crop.left   = MathUtils::round_int(m_crop.x1);
    crop.right  = MathUtils::round_int(m_crop.x2);
    crop.top    = MathUtils::round_int(m_crop.y1);
    crop.bottom = MathUtils::round_int(m_crop.y2);

    //compare with hysteresis
# define HYST(n, o) ((n) > (o) || (n) + 1 < (o))
    if(HYST(g_settings.m_currentVideoSettings.m_CropLeft  , crop.left)
    || HYST(g_settings.m_currentVideoSettings.m_CropRight , crop.right)
    || HYST(g_settings.m_currentVideoSettings.m_CropTop   , crop.top)
    || HYST(g_settings.m_currentVideoSettings.m_CropBottom, crop.bottom))
    {
      g_settings.m_currentVideoSettings.m_CropLeft   = crop.left;
      g_settings.m_currentVideoSettings.m_CropRight  = crop.right;
      g_settings.m_currentVideoSettings.m_CropTop    = crop.top;
      g_settings.m_currentVideoSettings.m_CropBottom = crop.bottom;
      g_renderManager.SetViewMode(g_settings.m_currentVideoSettings.m_ViewMode);
    }
# undef HYST
  }
}

void CDVDPlayerVideo::AutoCrop(DVDVideoPicture *pPicture, RECT &crop)
{
  crop.left   = g_settings.m_currentVideoSettings.m_CropLeft;
  crop.right  = g_settings.m_currentVideoSettings.m_CropRight;
  crop.top    = g_settings.m_currentVideoSettings.m_CropTop;
  crop.bottom = g_settings.m_currentVideoSettings.m_CropBottom;

  int black  = 16; // what is black in the image
  int level  = 8;  // how high above this should we detect
  int multi  = 4;  // what multiple of last line should failing line be to accept
  BYTE *s;
  int last, detect, black2;

  // top and bottom levels
  black2 = black * pPicture->iWidth;
  detect = level * pPicture->iWidth + black2;

  //YV12 and NV12 have planar Y plane
  //YUY2 and UYVY have Y packed with U and V
  int xspacing = 1;
  int xstart   = 0;
  if (pPicture->format == DVDVideoPicture::FMT_YUY2)
    xspacing = 2;
  else if (pPicture->format == DVDVideoPicture::FMT_UYVY)
  {
    xspacing = 2;
    xstart   = 1;
  }

  // Crop top
  s      = pPicture->data[0];
  last   = black2;
  for (unsigned int y = 0; y < pPicture->iHeight/2; y++)
  {
    int total = 0;
    for (unsigned int x = xstart; x < pPicture->iWidth * xspacing; x += xspacing)
      total += s[x];
    s += pPicture->iLineSize[0];

    if (total > detect)
    {
      if (total - black2 > (last - black2) * multi)
        crop.top = y;
      break;
    }
    last = total;
  }

  // Crop bottom
  s    = pPicture->data[0] + (pPicture->iHeight-1) * pPicture->iLineSize[0];
  last = black2;
  for (unsigned int y = (int)pPicture->iHeight; y > pPicture->iHeight/2; y--)
  {
    int total = 0;
    for (unsigned int x = xstart; x < pPicture->iWidth * xspacing; x += xspacing)
      total += s[x];
    s -= pPicture->iLineSize[0];

    if (total > detect)
    {
      if (total - black2 > (last - black2) * multi)
        crop.bottom = pPicture->iHeight - y;
      break;
    }
    last = total;
  }

  // left and right levels
  black2 = black * pPicture->iHeight;
  detect = level * pPicture->iHeight + black2;


  // Crop left
  s    = pPicture->data[0];
  last = black2;
  for (unsigned int x = xstart; x < pPicture->iWidth/2*xspacing; x += xspacing)
  {
    int total = 0;
    for (unsigned int y = 0; y < pPicture->iHeight; y++)
      total += s[y * pPicture->iLineSize[0]];
    s++;
    if (total > detect)
    {
      if (total - black2 > (last - black2) * multi)
        crop.left = x / xspacing;
      break;
    }
    last = total;
  }

  // Crop right
  s    = pPicture->data[0] + (pPicture->iWidth-1);
  last = black2;
  for (unsigned int x = (int)pPicture->iWidth*xspacing-1; x > pPicture->iWidth/2*xspacing; x -= xspacing)
  {
    int total = 0;
    for (unsigned int y = 0; y < pPicture->iHeight; y++)
      total += s[y * pPicture->iLineSize[0]];
    s--;

    if (total > detect)
    {
      if (total - black2 > (last - black2) * multi)
        crop.right = pPicture->iWidth - (x / xspacing);
      break;
    }
    last = total;
  }

  // We always crop equally on each side to get zoom
  // effect intead of moving the image. Aslong as the
  // max crop isn't much larger than the min crop
  // use that.
  int min, max;

  min = std::min(crop.left, crop.right);
  max = std::max(crop.left, crop.right);
  if(10 * (max - min) / pPicture->iWidth < 1)
    crop.left = crop.right = max;
  else
    crop.left = crop.right = min;

  min = std::min(crop.top, crop.bottom);
  max = std::max(crop.top, crop.bottom);
  if(10 * (max - min) / pPicture->iHeight < 1)
    crop.top = crop.bottom = max;
  else
    crop.top = crop.bottom = min;
}

std::string CDVDPlayerVideo::GetPlayerInfo()
{
  std::ostringstream s;
  s << "fr:"     << fixed << setprecision(3) << m_fFrameRate;
  s << " vq:"   << setw(2) << min(99,m_messageQueue.GetLevel()) << "%";
  s << " Mb/s:" << fixed << setprecision(2) << (double)GetVideoBitrate() / (1024.0*1024.0);
  s << " dr:" << m_iDroppedFrames;
  s << "(" << m_iDecoderDroppedFrames;
  s << "," << m_iDecoderPresentDroppedFrames;
  s << "," << m_iOutputDroppedFrames;
  s << ")";
  s << " rdr:" << m_iPlayerDropRequests;

  int pc = m_pullupCorrection.GetPatternLength();
  if (pc > 0)
    s << ", pc:" << pc;
  else
    s << ", pc:-";

  s << ", dc:"   << m_codecname;

  return s.str();
}

int CDVDPlayerVideo::GetVideoBitrate()
{
  return (int)m_videoStats.GetBitrate();
}

void CDVDPlayerVideo::ResetFrameRateCalc()
{
  m_fStableFrameRate = 0.0;
  m_iFrameRateCount  = 0;
  m_bAllowDrop       = !m_bCalcFrameRate && g_settings.m_currentVideoSettings.m_ScalingMethod != VS_SCALINGMETHOD_AUTO;
  m_iFrameRateLength = 1;
  m_iFrameRateErr    = 0;
}

#define MAXFRAMERATEDIFF   0.01
#define MAXFRAMESERR    1000

void CDVDPlayerVideo::CalcFrameRate()
{
  if (m_iFrameRateLength >= 128)
    return; //we're done calculating

  //only calculate the framerate if sync playback to display is on, adjust refreshrate is on,
  //or scaling method is set to auto
  if (!m_bCalcFrameRate && g_settings.m_currentVideoSettings.m_ScalingMethod != VS_SCALINGMETHOD_AUTO)
  {
    ResetFrameRateCalc();
    return;
  }

  if (!m_pullupCorrection.HasFullBuffer())
    return; //we can only calculate the frameduration if m_pullupCorrection has a full buffer

  //see if m_pullupCorrection was able to detect a pattern in the timestamps
  //and is able to calculate the correct frame duration from it
  double frameduration = m_pullupCorrection.GetFrameDuration();

  if (frameduration == DVD_NOPTS_VALUE)
  {
      CLog::Log(LOGDEBUG,"ASB: CalcFrameRate frameduration == DVD_NOPTS_VALUE");
    //reset the stored framerates if no good framerate was detected
    m_fStableFrameRate = 0.0;
    m_iFrameRateCount = 0;
    m_iFrameRateErr++;

    if (m_iFrameRateErr == MAXFRAMESERR && m_iFrameRateLength == 1)
    {
      CLog::Log(LOGDEBUG,"%s counted %i frames without being able to calculate the framerate, giving up", __FUNCTION__, m_iFrameRateErr);
      m_bAllowDrop = true;
      m_iFrameRateLength = 128;
    }
    return;
  }

  double framerate = DVD_TIME_BASE / frameduration;

  //store the current calculated framerate if we don't have any yet
  if (m_iFrameRateCount == 0)
  {
    m_fStableFrameRate = framerate;
    m_iFrameRateCount++;
  }
  //check if the current detected framerate matches with the stored ones
  else if (fabs(m_fStableFrameRate / m_iFrameRateCount - framerate) <= MAXFRAMERATEDIFF)
  {
    m_fStableFrameRate += framerate; //store the calculated framerate
    m_iFrameRateCount++;

    //if we've measured m_iFrameRateLength seconds of framerates,
    if (m_iFrameRateCount >= MathUtils::round_int(framerate) * m_iFrameRateLength)
    {
      //store the calculated framerate if it differs too much from m_fFrameRate
      if (fabs(m_fFrameRate - (m_fStableFrameRate / m_iFrameRateCount)) > MAXFRAMERATEDIFF)
      {
        CLog::Log(LOGDEBUG,"%s framerate was:%f calculated:%f", __FUNCTION__, m_fFrameRate, m_fStableFrameRate / m_iFrameRateCount);
        m_fFrameRate = m_fStableFrameRate / m_iFrameRateCount;
      }

      //reset the stored framerates
      m_fStableFrameRate = 0.0;
      m_iFrameRateCount = 0;
      m_iFrameRateLength *= 2; //double the length we should measure framerates

      //we're allowed to drop frames because we calculated a good framerate
      m_bAllowDrop = true;
    }
  }
  else //the calculated framerate didn't match, reset the stored ones
  {
      CLog::Log(LOGDEBUG,"ASB: CalcFrameRate framerate didn't match m_fStableFrameRate: %f m_iFrameRateCount: %i framerate: %f", m_fStableFrameRate, m_iFrameRateCount, framerate);
    m_fStableFrameRate = 0.0;
    m_iFrameRateCount = 0;
  }
}
