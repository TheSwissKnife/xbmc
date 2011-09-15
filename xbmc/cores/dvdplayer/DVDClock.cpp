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

#include "DVDClock.h"
#include "video/VideoReferenceClock.h"
#include <math.h>
#include "utils/MathUtils.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "utils/TimeUtils.h"

int64_t CDVDClock::m_systemOffset;
int64_t CDVDClock::m_systemFrequency;
CCriticalSection CDVDClock::m_systemsection;

bool CDVDClock::m_ismasterclock;

//TODO: the locks in this class seem to be a mess and not correctly set
// change to sharedlocks and have just the one? fix ref clock to sharedlocks to match

CDVDClock::CDVDClock()
{
  CSingleLock lock(m_systemsection);
  CheckSystemClock();

  m_systemUsed = m_systemFrequency;
  m_pauseClock = 0;
  m_bReset = true;
  m_iDisc = 0;
  m_maxspeedadjust = 0.0;
  m_speedadjust = false;
  SetVideoNotController();

  m_ismasterclock = true;
}

CDVDClock::~CDVDClock()
{}

// Returns the current absolute clock in units of DVD_TIME_BASE (usually microseconds).
double CDVDClock::GetAbsoluteClock(bool interpolated /*= true*/)
{
  CSingleLock lock(m_systemsection);
  CheckSystemClock();

  int64_t current;
  current = g_VideoReferenceClock.GetTime(interpolated);

#if _DEBUG
  if (interpolated) //only compare interpolated time, clock might go backwards otherwise
  {
    static int64_t old;
    if(old > current)
      CLog::Log(LOGWARNING, "CurrentHostCounter() moving backwords by %"PRId64" ticks with freq of %"PRId64, old - current, m_systemFrequency);
    old = current;
  }
#endif

  return SystemToAbsolute(current);
}

double CDVDClock::GetNextAbsoluteClockTick(double target)
{
  CSingleLock lock(m_systemsection);
  CheckSystemClock();

  int64_t systemtarget, system;

  lock.Leave();

  systemtarget = AbsoluteToSystem(target);
  system = g_VideoReferenceClock.GetNextTickTime(systemtarget);
  if (system == (int64_t)-1)
     return DVD_NOPTS_VALUE;

  return SystemToAbsolute(system);
}

double CDVDClock::WaitAbsoluteClock(double target, double* WaitClockDur /*= 0 */)
{
  CSingleLock lock(m_systemsection);
  CheckSystemClock();

  int64_t systemtarget, system, freq;
  freq = m_systemFrequency;

  lock.Leave();

  int64_t* WaitedTime;
  if (!WaitClockDur)
     WaitedTime = 0;
     
  systemtarget = AbsoluteToSystem(target);
  system = g_VideoReferenceClock.Wait(systemtarget, WaitedTime);
  if (WaitedTime)
     *WaitClockDur = *WaitedTime / freq * DVD_TIME_BASE;

  return SystemToAbsolute(system);
}

double CDVDClock::GetClock(bool interpolated /*= true*/, bool unPausedClock /* = false */)
{
  CSharedLock lock(m_critSection);
  return SystemToPlaying(g_VideoReferenceClock.GetTime(interpolated), unPausedClock);
}

double CDVDClock::GetClock(double& absolute, bool interpolated /*= true*/, bool unPausedClock /* = false */)
{
  int64_t current = g_VideoReferenceClock.GetTime(interpolated);
  {
    CSingleLock lock(m_systemsection);
    CheckSystemClock();
    absolute = SystemToAbsolute(current);
  }

  CSharedLock lock(m_critSection);
  return SystemToPlaying(current, unPausedClock);
}

void CDVDClock::SetSpeed(int iSpeed)
{
  // this will sometimes be a little bit of due to rounding errors, ie clock might jump abit when changing speed
  CExclusiveLock lock(m_critSection);
  int prevSpeed = m_speed;
  m_speed = iSpeed;

CLog::Log(LOGDEBUG, "ASB: CDVDClock::SetSpeed iSpeed: %i", iSpeed);
  
  if(iSpeed == DVD_PLAYSPEED_PAUSE)
  {
    if(!m_pauseClock)
      m_pauseClock = g_VideoReferenceClock.GetTime();
    return;
  }

  if (iSpeed == prevSpeed)
    return;
    
  int64_t current;
  int64_t newfreq = m_systemFrequency * DVD_PLAYSPEED_NORMAL / iSpeed;

  current = g_VideoReferenceClock.GetTime();
  if( m_pauseClock )
  {
    m_startClock += current - m_pauseClock;
    m_pauseClock = 0;
  }

  m_startClock = current - (int64_t)((double)(current - m_startClock) * newfreq / m_systemUsed);
  m_systemUsed = newfreq;
}

void CDVDClock::SetCurrentTickClock(double clock /* = 0.0 */)
{
  CExclusiveLock lock(m_critSection);
  m_startClock = g_VideoReferenceClock.GetTime(false);
  if(m_pauseClock)
    m_pauseClock = m_startClock;
  m_iDisc = clock;
CLog::Log(LOGDEBUG, "ASB: SetCurrentTickClock m_iDisc: %f m_startClock: %"PRId64"", m_iDisc, m_startClock);
  m_bReset = false;
}

void CDVDClock::Discontinuity(double currentPts /* = 0.0 */)
{
  CExclusiveLock lock(m_critSection);
  m_startClock = g_VideoReferenceClock.GetTime();
  if(m_pauseClock)
    m_pauseClock = m_startClock;
  m_iDisc = currentPts;
CLog::Log(LOGDEBUG, "ASB: Discontinuity m_iDisc: %f m_startClock: %"PRId64"", m_iDisc, m_startClock);
  m_bReset = false;
}

void CDVDClock::Pause()
{
  CExclusiveLock lock(m_critSection);
  if(!m_pauseClock)
    m_pauseClock = g_VideoReferenceClock.GetTime();
}

void CDVDClock::Resume()
{
  CExclusiveLock lock(m_critSection);
  if( m_pauseClock )
  {
    int64_t current;
    current = g_VideoReferenceClock.GetTime();

    m_startClock += current - m_pauseClock;
    m_pauseClock = 0;
  }
}

bool CDVDClock::IsPaused()
{
  CSharedLock lock(m_critSection);
  return (m_pauseClock != 0);
}

int CDVDClock::GetSpeed()
{
  CSharedLock lock(m_critSection);
  return m_speed;
}

bool CDVDClock::SetMaxSpeedAdjust(double speed)
{
  CSingleLock lock(m_speedsection);

  m_maxspeedadjust = speed;
  return m_speedadjust;
}

void CDVDClock::SetVideoNotController()
{
  CExclusiveLock lock(m_controlLeaseSection);
  m_videoControlDurSys = 0;
  m_videoControlMaxTickAdjustOffer = 0;
  m_videoControlAdjustOfferExpirySys = 0;
  m_videoControlLeaseStart = 0;
  m_videoIsController = false;
}

bool CDVDClock::SetVideoIsController(int64_t controlDurSys, int maxTickAdjustOffer /* = 0 */, int64_t adjustOfferExpirySys /* = 0 */)
{
   CExclusiveLock lock(m_controlLeaseSection);
   m_videoControlDurSys = controlDurSys;
   m_videoControlMaxTickAdjustOffer = maxTickAdjustOffer;
   m_videoControlAdjustOfferExpirySys = adjustOfferExpirySys;
   m_videoControlLeaseStart = CurrentHostCounter();
   m_videoIsController = true;
}

bool CDVDClock::VideoIsController()
{
   int64_t controlDurSys;
   int64_t adjustOfferExpirySys;
   int maxTickAdjustOffer;
   return VideoIsController(controlDurSys, maxTickAdjustOffer, adjustOfferExpirySys);
}

bool CDVDClock::CheckVideoControllerLease()
{
   CSharedLock lock(m_controlLeaseSection);
   if (m_videoIsController && (CurrentHostCounter() - m_videoControlLeaseStart > m_videoControlDurSys))
   {
      lock.Leave();
      SetVideoNotController();
      return false;
   }
   else
      return true;
}

bool CDVDClock::VideoIsController(int64_t& controlDurSys, int& maxTickAdjustOffer, int64_t& adjustOfferExpirySys)
{
   CheckVideoControllerLease();
   controlDurSys = m_videoControlDurSys;
   maxTickAdjustOffer = m_videoControlMaxTickAdjustOffer;
   adjustOfferExpirySys = m_videoControlAdjustOfferExpirySys;
   CSharedLock lock(m_controlLeaseSection);
   return m_videoIsController;
}

double CDVDClock::GetTickInterval()
{
  double interval;
  interval = 0.0;
  if (g_VideoReferenceClock.GetRefreshRate(&interval) <= 0)
     return 0.0;
  else
     return interval * DVD_TIME_BASE;
} 

//returns the refreshrate if the videoreferenceclock is running, -1 otherwise
int CDVDClock::UpdateFramerate(double fps, double* interval /*= NULL*/)
{
  //sent with fps of 0 means we are not playing video
  if(fps == 0.0)
  {
    CSingleLock lock(m_speedsection);
    m_speedadjust = false;
    return -1;
  }

  //check if the videoreferenceclock is running, will return -1 if not
  int rate = g_VideoReferenceClock.GetRefreshRate(interval);

  if (rate <= 0)
    return -1;

  CSingleLock lock(m_speedsection);

  m_speedadjust = true;

  double weight = (double)rate / (double)MathUtils::round_int(fps);

  //set the speed of the videoreferenceclock based on fps, refreshrate and maximum speed adjust set by user
  if (m_maxspeedadjust > 0.05)
  {
    if (weight / MathUtils::round_int(weight) < 1.0 + m_maxspeedadjust / 100.0
    &&  weight / MathUtils::round_int(weight) > 1.0 - m_maxspeedadjust / 100.0)
      weight = MathUtils::round_int(weight);
  }
  double speed = (double)rate / (fps * weight);
  lock.Leave();

  g_VideoReferenceClock.SetSpeed(speed);

  return rate;
}

void CDVDClock::CheckSystemClock()
{
  if(!m_systemFrequency)
    m_systemFrequency = g_VideoReferenceClock.GetFrequency();

  if(!m_systemOffset)
    m_systemOffset = g_VideoReferenceClock.GetTime();
}

double CDVDClock::SystemToAbsolute(int64_t system)
{
  return DVD_TIME_BASE * (double)(system - m_systemOffset) / m_systemFrequency;
}

int64_t CDVDClock::AbsoluteToSystem(double absolute)
{
  return (int64_t)(absolute / DVD_TIME_BASE * (double)m_systemFrequency) + m_systemOffset;
}

double CDVDClock::SystemToPlaying(int64_t system, bool unPausedClock /* = false */)
{
  int64_t current;

  if (m_bReset)
  {
    //TODO: locking here can cause deadlock at present
    m_startClock = system;
    m_systemUsed = m_systemFrequency;
    m_pauseClock = 0;
    m_iDisc = 0;
    m_bReset = false;
    m_videoIsController = false;
  }
  
  if (m_pauseClock && !unPausedClock)
    current = m_pauseClock;
  else
    current = system;

  return DVD_TIME_BASE * (double)(current - m_startClock) / m_systemUsed + m_iDisc;
}

