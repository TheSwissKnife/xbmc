#pragma once

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

#include "threads/Thread.h"
#include "DVDCodecs/Video/DVDVideoCodec.h"
#include "DVDClock.h"
#include "DVDStreamInfo.h"
#include "utils/BitstreamStats.h"
#include "DVDPlayerVideo.h"
#include "threads/CriticalSection.h"
#include <queue>
#include "settings/VideoSettings.h"
#include <GL/glx.h>

struct ToOutputMessage
{
  ToOutputMessage()
  {
    fFrameTime = 0;
    bDrop = false;
    bNotToSkip = false;
    bLastPic = false;
    iSpeed = 0;
  };
  double fFrameTime;
  bool bDrop;
  bool bNotToSkip;
  bool bLastPic;
  int iSpeed;
};

struct FromOutputMessage
{
  FromOutputMessage()
  {
    iResult = 0;
  };
  int iResult;
};

class CDVDPlayerVideoOutput : public CThread
{
public:
  CDVDPlayerVideoOutput(CDVDPlayerVideo *videoplayer);
  virtual ~CDVDPlayerVideoOutput();

  void Start();
  void Reset(bool resetConfigure = false);
  void Dispose();
  void SendMessage(ToOutputMessage &msg);
  bool GetMessage(FromOutputMessage &msg, bool bWait);
  int GetMessageSize();
  void SetCodec(CDVDVideoCodec *codec);
  void SetPts(double pts);
  double GetPts();
protected:
  void OnStartup();
  void OnExit();
  void Process();
  bool GetPicture(ToOutputMessage toMsg);
  bool RefreshGlxContext();
  bool DestroyGlxContext();

  double m_pts;
  int m_speed;
  CDVDVideoCodec* m_pVideoCodec;
  DVDVideoPicture m_picture;
  std::queue<ToOutputMessage> m_toOutputMessage;
  std::queue<FromOutputMessage> m_fromOutputMessage;
  CEvent m_toMsgSignal, m_fromMsgSignal;
  CCriticalSection m_criticalSection;
  CCriticalSection m_msgSection;
  CDVDPlayerVideo *m_pVideoPlayer;
  GLXContext m_glContext;
  GLXWindow m_glWindow;
  Pixmap    m_pixmap;
  GLXPixmap m_glPixmap;
  bool m_recover;
  bool m_configuring;
  bool m_outputprevpic;
};
