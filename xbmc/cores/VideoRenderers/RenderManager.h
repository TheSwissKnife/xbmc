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

#include <list>

#if defined (HAS_GL)
  #include "LinuxRendererGL.h"
#elif HAS_GLES == 2
  #include "LinuxRendererGLES.h"
#elif defined(HAS_DX)
  #include "WinRenderer.h"
#elif defined(HAS_SDL)
  #include "LinuxRenderer.h"
#endif

#include "threads/SharedSection.h"
#include "threads/Thread.h"
#include "settings/VideoSettings.h"
#include "OverlayRenderer.h"
#include "../dvdplayer/DVDClock.h"

class CRenderCapture;

namespace DXVA { class CProcessor; }
namespace VAAPI { class CSurfaceHolder; }
class CVDPAU;
struct DVDVideoPicture;

#define ERRORBUFFSIZE 30
#define NUM_DISPLAYINFOBUF 4

class CXBMCRenderManager
{
public:
  CXBMCRenderManager();
  ~CXBMCRenderManager();

  // Functions called from the GUI
  void GetVideoRect(CRect &source, CRect &dest) { CSharedLock lock(m_sharedSection); if (m_pRenderer) m_pRenderer->GetVideoRect(source, dest); };
  float GetAspectRatio() { CSharedLock lock(m_sharedSection); if (m_pRenderer) return m_pRenderer->GetAspectRatio(); else return 1.0f; };
  void Update(bool bPauseDrawing);
  void RenderUpdate(bool flip, bool clear, DWORD flags = 0, DWORD alpha = 255);
  double GetCurrentDisplayPts(int& playspeed, double& callclock);
  double GetDisplayDelay();
  double GetDisplaySignalToViewDelay();
  void SetupScreenshot();

  CRenderCapture* AllocRenderCapture();
  void ReleaseRenderCapture(CRenderCapture* capture);
  void Capture(CRenderCapture *capture, unsigned int width, unsigned int height, int flags);
  void ManageCaptures();

  void SetViewMode(int iViewMode) { CSharedLock lock(m_sharedSection); if (m_pRenderer) m_pRenderer->SetViewMode(iViewMode); };

  // Functions called from mplayer
  bool Configure(unsigned int width, unsigned int height, unsigned int d_width, unsigned int d_height, float fps, unsigned flags);
  bool IsConfigured();

  int AddVideoPicture(DVDVideoPicture& picture, double pts, double presenttime, int playspeed, bool vclockresync = false);

  void FlipPage(volatile bool& bStop, double timestamp = 0.0, int source = -1, EFIELDSYNC sync = FS_NONE);
  int WaitForBuffer(volatile bool& bStop);
  void ReleaseProcessor();
  void NotifyDisplayFlip();
  void UpdateDisplayInfo();
  unsigned int PreInit();
  void UnInit();
  bool WaitDrained(int timeout = 100);
  bool CheckResolutionChange(float fps);

  void AddOverlay(CDVDOverlay* o, double pts)
  {
    CSharedLock lock(m_sharedSection);
//    m_requestOverlayFlip = true;
    m_overlays.AddOverlay(o, pts);
  }

  int OverlayFlipOutput()
  {
    CSharedLock lock(m_sharedSection);
    return m_overlays.FlipOutput();
  }

  void AddCleanup(OVERLAY::COverlay* o)
  {
    CSharedLock lock(m_sharedSection);
    m_overlays.AddCleanup(o);
  }

  inline void Reset()
  {
    CSharedLock lock(m_sharedSection);
    if (m_pRenderer)
      m_pRenderer->Reset();
  }
  RESOLUTION GetResolution()
  {
    CSharedLock lock(m_sharedSection);
    if (m_pRenderer)
      return m_pRenderer->GetResolution();
    else
      return RES_INVALID;
  }

  float GetMaximumFPS();
  inline bool Paused() { return m_bPauseDrawing; };
  inline bool IsStarted() { return m_bIsStarted;}

  bool Supports(ERENDERFEATURE feature)
  {
    CSharedLock lock(m_sharedSection);
    if (m_pRenderer)
      return m_pRenderer->Supports(feature);
    else
      return false;
  }

  bool Supports(EINTERLACEMETHOD method)
  {
    CSharedLock lock(m_sharedSection);
    if (m_pRenderer)
      return m_pRenderer->Supports(method);
    else
      return false;
  }

  bool Supports(ESCALINGMETHOD method)
  {
    CSharedLock lock(m_sharedSection);
    if (m_pRenderer)
      return m_pRenderer->Supports(method);
    else
      return false;
  }

  double GetPresentTime();
  void  WaitPresentTime(double presenttime, bool reset_corr = false);

  CStdString GetVSyncState();

  void UpdateResolution();

#ifdef HAS_GL
  CLinuxRendererGL *m_pRenderer;
#elif HAS_GLES == 2
  CLinuxRendererGLES *m_pRenderer;
#elif defined(HAS_DX)
  CWinRenderer *m_pRenderer;
#elif defined(HAS_SDL)
  CLinuxRenderer *m_pRenderer;
#endif

  void Present();
  void Recover(); // called after resolution switch if something special is needed

  CSharedSection& GetSection() { return m_sharedSection; };

protected:

  void PresentSingle();
  void PresentWeave();
  void PresentBob();
  void PresentBlend();
  void CheckNextBuffer();
  void UpdatePostRenderClock();
  void UpdatePreFlipClock();
  void UpdatePostFlipClock();

  bool m_bPauseDrawing;   // true if we should pause rendering

  bool m_bIsStarted;
  CSharedSection m_sharedSection;
  CSharedSection m_sharedDisplayInfoSection; //to guard to display info only

  bool m_bReconfigured;

  int m_rendermethod;

  enum EPRESENTSTEP
  {
    PRESENT_IDLE     = 0
  , PRESENT_FLIP
  , PRESENT_FRAME
  , PRESENT_FRAME2
  };

  double     m_presenttime;
  double     m_presentcorr;
  double     m_presenterr;
  double     m_errorbuff[ERRORBUFFSIZE];
  int        m_errorindex;
  int        m_videoPicId; //id to track video pic 
  bool m_vclockresync; //video to clock resync flagged
  double m_prevwaitabserror; //previous wait absolute error fraction

  struct FrameInfo
  {
    double framedur;  
    double framepts; 
    int    frameId; 
    int    frameplayspeed;
    double refreshdur; //vblank interval in clock units
    double frameclock; //estimated clock time of actual display visibilty
  };
  FrameInfo m_displayinfo[NUM_DISPLAYINFOBUF]; //buffer of display frame infos 
  FrameInfo m_renderinfo; //frame being rendered
  FrameInfo m_refdisplayinfo; //reference display frame info

  bool m_flipasync; //true if flip/swap is done asynchronously
  double m_preflipclock; //clock time of last flip request
  double m_postflipclock; //clock time of last flip request completion
  double m_postrenderclock; //clock time after last render to back buffer request completion
  int m_shortdisplaycount; //estimated count of frames displayed for shorter period than expected
  int m_longdisplaycount; //estimated count of frames displayed for longer period than expected

  EFIELDSYNC m_presentfield;
  EINTERLACEMETHOD m_presentmethod;
  EPRESENTSTEP     m_presentstep;
  int        m_presentsource;
  CEvent     m_presentevent;
  CEvent     m_flipEvent;
  int       m_missedTickWait;
//  CDVDClock  *m_pClock;
//  bool       m_late;
//  bool       m_bDrain;
//  bool       m_requestOverlayFlip;

  OVERLAY::CRenderer m_overlays;

  void RenderCapture(CRenderCapture* capture);
  void RemoveCapture(CRenderCapture* capture);
  CCriticalSection           m_captCritSect;
  std::list<CRenderCapture*> m_captures;
  //set to true when adding something to m_captures, set to false when m_captures is made empty
  //std::list::empty() isn't thread safe, using an extra bool will save a lock per render when no captures are requested
  bool                       m_hasCaptures; 
};

extern CXBMCRenderManager g_renderManager;
