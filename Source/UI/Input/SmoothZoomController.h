/*
    This file is part of Helio Workstation.

    Helio is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Helio is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Helio. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "SmoothZoomListener.h"

class SmoothZoomController final :
    private Thread,
    private WaitableEvent,
    private AsyncUpdater
{
public:

    explicit SmoothZoomController(SmoothZoomListener &parent) :
        Thread("SmoothZoom"),
        listener(parent)
    {
        this->startThread(8);
    }

    ~SmoothZoomController() override
    {
        this->signalThreadShouldExit();
        this->signal();
        this->stopThread(500);
    }

    inline float getInitialZoomSpeed() const noexcept
    {
        return this->initialZoomSpeed;
    }

    inline bool isZooming() const noexcept
    {
        return this->factorX.get() != 0.f;
    }

    void cancelZoom() noexcept
    {
        this->factorX = 0.f;
        this->factorY = 0.f;
    }

    void zoomRelative(const Point<float> &from, const Point<float> &zoom) noexcept
    {
        this->factorX = (this->factorX.get() + zoom.getX()) * this->zoomSmoothFactor;
        this->factorY = (this->factorY.get() + zoom.getY()) * this->zoomSmoothFactor;
        this->originX = from.getX();
        this->originY = from.getY();

        WaitableEvent::signal();
    }

private:

    inline bool stillNeedsZoom() const noexcept
    {
        return juce_hypot(this->factorX.get(), this->factorY.get()) >= this->zoomStopFactor;
    }

    void run() override
    {
        while (! this->threadShouldExit())
        {
            while (this->stillNeedsZoom())
            {
                if (this->threadShouldExit())
                {
                    return;
                }

                this->factorX = this->factorX.get() * this->zoomDecayFactor;
                this->factorY = this->factorY.get() * this->zoomDecayFactor;

                this->triggerAsyncUpdate();
                Thread::sleep(this->timerDelay);
            }

            this->cancelZoom();

            if (this->threadShouldExit())
            {
                return;
            }

            WaitableEvent::wait();
        }
    }

    void handleAsyncUpdate() noexcept override
    {
        this->listener.zoomRelative(
            { this->originX.get(), this->originY.get() },
            { this->factorX.get(), this->factorY.get() });
    }

    SmoothZoomListener &listener;

    Atomic<float> factorX = 0.f;
    Atomic<float> factorY = 0.f;
    Atomic<float> originX = 0.f;
    Atomic<float> originY = 0.f;

private:

    const int timerDelay = 8;
    const float zoomStopFactor = 0.001f;
    const float zoomDecayFactor = 0.825f;
    const float zoomSmoothFactor = 0.9f;
    const float initialZoomSpeed = 0.25f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SmoothZoomController)
};
