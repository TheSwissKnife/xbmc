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

#include "DVDVideoCodec.h"
#include "DVDResource.h"
#include "DllAvCodec.h"
#include "DllAvFormat.h"
#include "DllAvUtil.h"
#include "DllSwScale.h"
#include "DllAvFilter.h"
#include "threads/Thread.h"

#define INPUT_HISTBUFNUM 50 //number of input fields/frames to keep track of in order to monitor delaed output of decoder

class CVDPAU;
class CCriticalSection;

class CDVDVideoCodecFFmpeg : public CDVDVideoCodec
{
public:
  class IHardwareDecoder : public IDVDResourceCounted<IHardwareDecoder>
  {
    public:
             IHardwareDecoder() {}
    virtual ~IHardwareDecoder() {};
    virtual bool Open      (AVCodecContext* avctx, const enum PixelFormat) = 0;
    virtual int  Decode    (AVCodecContext* avctx, AVFrame* frame) = 0;
    virtual int  Decode    (AVCodecContext* avctx, AVFrame* frame, bool bDrain) {return Decode(avctx, frame);};
    virtual bool GetPicture(AVCodecContext* avctx, AVFrame* frame, DVDVideoPicture* picture) = 0;
    virtual int  Check     (AVCodecContext* avctx) = 0;
    virtual void Reset     () {}
    virtual const std::string Name() = 0;
    virtual CCriticalSection* Section() { return NULL; }

    // signal to vdpau (mixer) whether we run normal speed or not
    // so it can switch off deinterlacing
    virtual bool AllowFrameDropping() {return false;};
    virtual void SetDropState(bool bDrop) {return;};
    virtual bool FreeResources(bool test = false) {return false;};
  };

  CDVDVideoCodecFFmpeg();
  virtual ~CDVDVideoCodecFFmpeg();
  virtual bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options);
  virtual void Dispose();
  virtual int Decode(BYTE* pData, int iSize, double dts, double pts);
  virtual void Reset();
  bool GetPictureCommon(DVDVideoPicture* pDvdVideoPicture);
  virtual bool GetPicture(DVDVideoPicture* pDvdVideoPicture);
  virtual bool AllowDecoderDrop();
  virtual bool HintDropUrgent();
  virtual bool HintDropSubtle();
  virtual bool HintNoPostProc();
  virtual bool HintNoPresent();
  virtual bool HintHurryUp();
  virtual void ResetHintNoPresent();
  virtual bool SubtleHardwareDropping();
  virtual void ResetDropState();
  virtual void SetNextDropState(bool bDrop);
  virtual void SetDropMethod(bool bDrop, bool bInputPacket = true);
  virtual bool SetDecoderHint(int iDropHint);
  virtual void SetDropState(bool bDrop);
  virtual unsigned int SetFilters(unsigned int filters);
  virtual const char* GetName() { return m_name.c_str(); }; // m_name is never changed after open
  virtual unsigned GetConvergeCount();
  virtual bool WaitGetPicture();
  virtual bool HwFreeResources(bool test = false);

  bool               IsHardwareAllowed()                     { return !m_bSoftware; }
  IHardwareDecoder * GetHardware()                           { return m_pHardware; };
  void               SetHardware(IHardwareDecoder* hardware) 
  {
    m_pHardware = hardware;
    m_name += "-";
    m_name += m_pHardware->Name();
  }

protected:
  static enum PixelFormat GetFormat(struct AVCodecContext * avctx, const PixelFormat * fmt);

  int  FilterOpen(const CStdString& filters);
  void FilterClose();
  int  FilterProcess(AVFrame* frame);

  void GetVideoAspect(AVCodecContext* CodecContext, unsigned int& iWidth, unsigned int& iHeight);
  void RecordPacketInfoInHist(int iInputPktNum, int64_t pts_opaque, bool bDropRequested, bool bDecoderDropRequested);
  void SetFrameFlagsFromHist(int iInputPktNum);

  void ResetState();
  AVFrame* m_pFrame;
  AVCodecContext* m_pCodecContext;

  AVPicture* m_pConvertFrame;
  CStdString       m_filters;
  CStdString       m_filters_next;
  AVFilterGraph*   m_pFilterGraph;
  AVFilterContext* m_pFilterIn;
  AVFilterContext* m_pFilterOut;
  AVFilterLink*    m_pFilterLink;

  int m_iPictureWidth;
  int m_iPictureHeight;

  int m_iScreenWidth;
  int m_iScreenHeight;

  DllAvCodec m_dllAvCodec;
  DllAvUtil  m_dllAvUtil;
  DllSwScale m_dllSwScale;
  DllAvFilter m_dllAvFilter;

  std::string m_name;
  bool              m_bSoftware;
  IHardwareDecoder *m_pHardware;
  int m_iLastKeyframe;
  int m_iDecoderInputPktNumber; //current decoder input packet number
  int m_iDecoderOutputFrameNumber; //current successfully output decoder frame number
  int m_iDecoderDrop; //number of decoder drops 
  int m_iDecoderHint; //current decoder hint value
  int m_iDecoderDropRequest; //decoder drop request count since reset
  int m_iInterlacedEnabledFrameNumber; //frame number when interlacing mode was enabled
  int m_iDecoderLastOutputPktNumber; //input packet number when last frame came out of decoder
  int m_iMaxDecoderIODelay; //current max decoder delay (in input pkts) between input packet and its associated output frame
  bool m_bDropRequested; //current drop request state
  bool m_bDecoderDropRequested; //current decoder drop request state
  bool m_bHardwareDropRequested; //current pHardware drop request state
  bool m_bDropNextRequested; //set drop state next time flag 
  bool m_bDecodingStable; //decoder flow is now stable flag
  bool m_bExpectingDecodedFrame; //the decoder is expected to output a frame on next call
  bool m_bInterlacedMode; //decoder is in interlaced frame mode
  bool m_bFieldInputMode; //decoder input is field based 
  int m_iFrameFlags; //flags to be applied to the frame just decoded (from hints at decoder input time)
  struct input_hist {
     int64_t pts_opaque; //frame pts opaque form
     bool drop; //decoder drop request flag
     int flags; //flags that should be applied to the frame when coming out of the decoder
     bool field_no_out; //input is thought to be a field that will have no corresponding output frame
     int input_pos; //absolute input position
  } m_input_hist[INPUT_HISTBUFNUM]; //buffer history of decoder input frames

  double m_dts;
  bool   m_started;
  CEvent m_picSignal;
};
