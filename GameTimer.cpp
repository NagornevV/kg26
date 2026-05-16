#include "GameTimer.h"
#include <windows.h>

GameTimer::GameTimer()
{
    __int64 countsPerSec = 0;
    QueryPerformanceFrequency((LARGE_INTEGER*)&countsPerSec);
    mSecondsPerCount = 1.0 / (double)countsPerSec;
}

float GameTimer::TotalTime() const
{
    if (mStopped)
        return (float)(((mStopTime - mPausedTime) - mBaseTime) * mSecondsPerCount);
    return (float)(((mCurrTime - mPausedTime) - mBaseTime) * mSecondsPerCount);
}

float GameTimer::DeltaTime() const { return (float)mDeltaTime; }

void GameTimer::Reset()
{
    __int64 currTime = 0;
    QueryPerformanceCounter((LARGE_INTEGER*)&currTime);
    mBaseTime   = currTime;
    mPrevTime   = currTime;
    mStopTime   = 0;
    mPausedTime = 0;
    mStopped    = false;
}

void GameTimer::Start()
{
    if (mStopped)
    {
        __int64 startTime = 0;
        QueryPerformanceCounter((LARGE_INTEGER*)&startTime);
        mPausedTime += (startTime - mStopTime);
        mPrevTime    = startTime;
        mStopTime    = 0;
        mStopped     = false;
    }
}

void GameTimer::Stop()
{
    if (!mStopped)
    {
        __int64 currTime = 0;
        QueryPerformanceCounter((LARGE_INTEGER*)&currTime);
        mStopTime = currTime;
        mStopped  = true;
    }
}

void GameTimer::Tick()
{
    if (mStopped) { mDeltaTime = 0.0; return; }

    QueryPerformanceCounter((LARGE_INTEGER*)&mCurrTime);
    mDeltaTime = (mCurrTime - mPrevTime) * mSecondsPerCount;
    mPrevTime  = mCurrTime;

    if (mDeltaTime < 0.0) mDeltaTime = 0.0;
}
