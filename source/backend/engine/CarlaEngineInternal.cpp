/*
 * Carla Plugin Host
 * Copyright (C) 2011-2019 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the doc/GPL.txt file.
 */

#include "CarlaEngineInternal.hpp"
#include "CarlaPlugin.hpp"
#include "CarlaSemUtils.hpp"

#include "jackbridge/JackBridge.hpp"

#include <ctime>
#include <sys/time.h>

CARLA_BACKEND_START_NAMESPACE

// -----------------------------------------------------------------------
// Engine Internal helper macro, sets lastError and returns false/NULL

#define CARLA_SAFE_ASSERT_RETURN_INTERNAL_ERR(cond, err)  if (! (cond)) { carla_safe_assert(#cond, __FILE__, __LINE__); lastError = err; return false;   }
#define CARLA_SAFE_ASSERT_RETURN_INTERNAL_ERRN(cond, err) if (! (cond)) { carla_safe_assert(#cond, __FILE__, __LINE__); lastError = err; return nullptr; }

// -----------------------------------------------------------------------
// InternalEvents

EngineInternalEvents::EngineInternalEvents() noexcept
    : in(nullptr),
      out(nullptr) {}

EngineInternalEvents::~EngineInternalEvents() noexcept
{
    CARLA_SAFE_ASSERT(in == nullptr);
    CARLA_SAFE_ASSERT(out == nullptr);
}

void EngineInternalEvents::clear() noexcept
{
    if (in != nullptr)
    {
        delete[] in;
        in = nullptr;
    }

    if (out != nullptr)
    {
        delete[] out;
        out = nullptr;
    }
}

// -----------------------------------------------------------------------
// InternalTime

static const double kTicksPerBeat = 1920.0;

#if defined(HAVE_HYLIA) && !defined(BUILD_BRIDGE)
static uint32_t calculate_link_latency(const double bufferSize, const double sampleRate) noexcept
{
    CARLA_SAFE_ASSERT_RETURN(carla_isNotZero(sampleRate), 0);

    const long long int latency = llround(1.0e6 * bufferSize / sampleRate);
    CARLA_SAFE_ASSERT_RETURN(latency >= 0 && latency < UINT32_MAX, 0);

    return static_cast<uint32_t>(latency);
}
#endif

EngineInternalTime::EngineInternalTime(EngineTimeInfo& ti, const EngineTransportMode& tm) noexcept
    : beatsPerBar(4.0),
      beatsPerMinute(120.0),
      bufferSize(0.0),
      sampleRate(0.0),
      tick(0.0),
      needsReset(false),
      nextFrame(0),
#ifndef BUILD_BRIDGE
      hylia(),
#endif
      timeInfo(ti),
      transportMode(tm) {}

void EngineInternalTime::init(const uint32_t bsize, const double srate)
{
    bufferSize = bsize;
    sampleRate = srate;

#if defined(HAVE_HYLIA) && !defined(BUILD_BRIDGE)
    if (hylia.instance != nullptr)
    {
        hylia_set_beats_per_bar(hylia.instance, beatsPerBar);
        hylia_set_beats_per_minute(hylia.instance, beatsPerMinute);
        hylia_set_output_latency(hylia.instance, calculate_link_latency(bsize, srate));

        if (hylia.enabled)
            hylia_enable(hylia.instance, true);
    }
#endif

    needsReset = true;
}

void EngineInternalTime::updateAudioValues(const uint32_t bsize, const double srate)
{
    bufferSize = bsize;
    sampleRate = srate;

#if defined(HAVE_HYLIA) && !defined(BUILD_BRIDGE)
    if (hylia.instance != nullptr)
        hylia_set_output_latency(hylia.instance, calculate_link_latency(bsize, srate));
#endif

    needsReset = true;
}

void EngineInternalTime::enableLink(const bool enable)
{
#if defined(HAVE_HYLIA) && !defined(BUILD_BRIDGE)
    if (hylia.enabled == enable)
        return;

    if (hylia.instance != nullptr)
    {
        hylia.enabled = enable;
        hylia_enable(hylia.instance, enable);
    }
#else
    // unused
    (void)enable;
#endif

    needsReset = true;
}

void EngineInternalTime::setBPM(const double bpm)
{
    beatsPerMinute = bpm;

#if defined(HAVE_HYLIA) && !defined(BUILD_BRIDGE)
    if (hylia.instance != nullptr)
        hylia_set_beats_per_minute(hylia.instance, bpm);
#endif
}

void EngineInternalTime::setNeedsReset() noexcept
{
    needsReset = true;
}

void EngineInternalTime::pause() noexcept
{
    timeInfo.playing = false;
    nextFrame = timeInfo.frame;
    needsReset = true;
}

void EngineInternalTime::relocate(const uint64_t frame) noexcept
{
    timeInfo.frame = frame;
    nextFrame = frame;
    needsReset = true;
}

void EngineInternalTime::fillEngineTimeInfo(const uint32_t newFrames) noexcept
{
    CARLA_SAFE_ASSERT_RETURN(carla_isNotZero(sampleRate),);
    CARLA_SAFE_ASSERT_RETURN(newFrames > 0,);

    double ticktmp;

    if (transportMode == ENGINE_TRANSPORT_MODE_INTERNAL)
    {
        timeInfo.usecs = 0;
        timeInfo.frame = nextFrame;
    }

    if (needsReset)
    {
        timeInfo.bbt.valid = true;
        timeInfo.bbt.beatType = 4.0f;
        timeInfo.bbt.ticksPerBeat = kTicksPerBeat;

        double abs_beat, abs_tick;

#if defined(HAVE_HYLIA) && !defined(BUILD_BRIDGE)
        if (hylia.enabled)
        {
            if (hylia.timeInfo.beat >= 0.0)
            {
                abs_beat = hylia.timeInfo.beat;
                abs_tick = abs_beat * kTicksPerBeat;
            }
            else
            {
                abs_beat = 0.0;
                abs_tick = 0.0;
                timeInfo.playing = false;
            }
        }
        else
#endif
        {
            const double min = static_cast<double>(timeInfo.frame) / (sampleRate * 60.0);
            abs_beat = min * beatsPerMinute;
            abs_tick = abs_beat * kTicksPerBeat;
            needsReset = false;
        }

        const double bar  = std::floor(abs_beat / beatsPerBar);
        const double beat = std::floor(std::fmod(abs_beat, beatsPerBar));

        timeInfo.bbt.bar  = static_cast<int32_t>(bar) + 1;
        timeInfo.bbt.beat = static_cast<int32_t>(beat) + 1;
        timeInfo.bbt.barStartTick = ((bar * beatsPerBar) + beat) * kTicksPerBeat;

        ticktmp = abs_tick - timeInfo.bbt.barStartTick;
    }
    else if (timeInfo.playing)
    {
        ticktmp = tick + (newFrames * kTicksPerBeat * beatsPerMinute / (sampleRate * 60));

        while (ticktmp >= kTicksPerBeat)
        {
            ticktmp -= kTicksPerBeat;

            if (++timeInfo.bbt.beat > beatsPerBar)
            {
                ++timeInfo.bbt.bar;
                timeInfo.bbt.beat = 1;
                timeInfo.bbt.barStartTick += beatsPerBar * kTicksPerBeat;
            }
        }
    }
    else
    {
        ticktmp = tick;
    }

    timeInfo.bbt.beatsPerBar = static_cast<float>(beatsPerBar);
    timeInfo.bbt.beatsPerMinute = beatsPerMinute;
    timeInfo.bbt.tick = ticktmp;
    tick = ticktmp;

    if (transportMode == ENGINE_TRANSPORT_MODE_INTERNAL && timeInfo.playing)
        nextFrame += newFrames;
}

void EngineInternalTime::fillJackTimeInfo(jack_position_t* const pos, const uint32_t newFrames) noexcept
{
    CARLA_SAFE_ASSERT_RETURN(carla_isNotZero(sampleRate),);
    CARLA_SAFE_ASSERT_RETURN(newFrames > 0,);
    CARLA_SAFE_ASSERT(transportMode == ENGINE_TRANSPORT_MODE_JACK);

    fillEngineTimeInfo(newFrames);

    pos->valid = JackPositionBBT;
    pos->bar   = timeInfo.bbt.bar;
    pos->beat  = timeInfo.bbt.beat;
    pos->tick  = static_cast<int32_t>(tick + 0.5);
    pos->bar_start_tick = timeInfo.bbt.barStartTick;
    pos->beats_per_bar = timeInfo.bbt.beatsPerBar;
    pos->beat_type = timeInfo.bbt.beatType;
    pos->ticks_per_beat = kTicksPerBeat;
    pos->beats_per_minute = beatsPerMinute;
}

void EngineInternalTime::preProcess(const uint32_t numFrames)
{
#if defined(HAVE_HYLIA) && !defined(BUILD_BRIDGE)
    if (hylia.enabled)
    {
        hylia_process(hylia.instance, numFrames, &hylia.timeInfo);

        const double new_bpb = hylia.timeInfo.beatsPerBar;
        const double new_bpm = hylia.timeInfo.beatsPerMinute;

        if (new_bpb >= 1.0 && carla_isNotEqual(beatsPerBar, new_bpb))
        {
            beatsPerBar = new_bpb;
            needsReset = true;
        }
        if (new_bpm > 0.0 && carla_isNotEqual(beatsPerMinute, new_bpm))
        {
            beatsPerMinute = new_bpm;
            needsReset = true;
        }
    }
#endif

    if (transportMode == ENGINE_TRANSPORT_MODE_INTERNAL)
        fillEngineTimeInfo(numFrames);
}

// -----------------------------------------------------------------------
// EngineInternalTime::Hylia

#ifndef BUILD_BRIDGE
EngineInternalTime::Hylia::Hylia()
    : enabled(false),
      instance(nullptr),
      timeInfo()
{
    carla_zeroStruct(timeInfo);

# ifdef HAVE_HYLIA
    instance = hylia_create();
# endif
}

EngineInternalTime::Hylia::~Hylia()
{
# ifdef HAVE_HYLIA
    hylia_cleanup(instance);
# endif
}
#endif

// -----------------------------------------------------------------------
// NextAction

EngineNextAction::EngineNextAction() noexcept
    : opcode(kEnginePostActionNull),
      pluginId(0),
      value(0),
      mutex(),
      needsPost(false),
      postDone(false),
      sem(carla_sem_create(false)) {}

EngineNextAction::~EngineNextAction() noexcept
{
    CARLA_SAFE_ASSERT(opcode == kEnginePostActionNull);

    if (sem != nullptr)
    {
        carla_sem_destroy(sem);
        sem = nullptr;
    }
}

void EngineNextAction::clearAndReset() noexcept
{
    mutex.lock();
    CARLA_SAFE_ASSERT(opcode == kEnginePostActionNull);

    opcode    = kEnginePostActionNull;
    pluginId  = 0;
    value     = 0;
    needsPost = false;
    postDone  = false;
    mutex.unlock();
}

// -----------------------------------------------------------------------
// CarlaEngine::ProtectedData

CarlaEngine::ProtectedData::ProtectedData(CarlaEngine* const engine) noexcept
    : thread(engine),
#if defined(HAVE_LIBLO) && !defined(BUILD_BRIDGE)
      osc(engine),
      oscData(nullptr),
#endif
      callback(nullptr),
      callbackPtr(nullptr),
      fileCallback(nullptr),
      fileCallbackPtr(nullptr),
      actionCanceled(false),
#ifndef BUILD_BRIDGE_ALTERNATIVE_ARCH
      loadingProject(false),
      currentProjectFilename(),
#endif
      hints(0x0),
      bufferSize(0),
      sampleRate(0.0),
      aboutToClose(false),
      isIdling(0),
      curPluginCount(0),
      maxPluginNumber(0),
      nextPluginId(0),
      envMutex(),
      lastError(),
      name(),
      options(),
      timeInfo(),
#ifndef BUILD_BRIDGE_ALTERNATIVE_ARCH
      plugins(nullptr),
      xruns(0),
      dspLoad(0.0f),
#endif
      events(),
#ifndef BUILD_BRIDGE_ALTERNATIVE_ARCH
      graph(engine),
#endif
      time(timeInfo, options.transportMode),
      nextAction()
{
#ifdef BUILD_BRIDGE_ALTERNATIVE_ARCH
    carla_zeroStructs(plugins, 1);
#endif
}

CarlaEngine::ProtectedData::~ProtectedData() noexcept
{
    CARLA_SAFE_ASSERT(curPluginCount == 0);
    CARLA_SAFE_ASSERT(maxPluginNumber == 0);
    CARLA_SAFE_ASSERT(nextPluginId == 0);
    CARLA_SAFE_ASSERT(isIdling == 0);
#ifndef BUILD_BRIDGE_ALTERNATIVE_ARCH
    CARLA_SAFE_ASSERT(plugins == nullptr);
#endif
}

// -----------------------------------------------------------------------

bool CarlaEngine::ProtectedData::init(const char* const clientName)
{
    CARLA_SAFE_ASSERT_RETURN_INTERNAL_ERR(name.isEmpty(), "Invalid engine internal data (err #1)");
#if defined(HAVE_LIBLO) && !defined(BUILD_BRIDGE)
    CARLA_SAFE_ASSERT_RETURN_INTERNAL_ERR(oscData == nullptr, "Invalid engine internal data (err #2)");
#endif
    CARLA_SAFE_ASSERT_RETURN_INTERNAL_ERR(events.in  == nullptr, "Invalid engine internal data (err #4)");
    CARLA_SAFE_ASSERT_RETURN_INTERNAL_ERR(events.out == nullptr, "Invalid engine internal data (err #5)");
    CARLA_SAFE_ASSERT_RETURN_INTERNAL_ERR(clientName != nullptr && clientName[0] != '\0', "Invalid client name");
#ifndef BUILD_BRIDGE_ALTERNATIVE_ARCH
    CARLA_SAFE_ASSERT_RETURN_INTERNAL_ERR(plugins == nullptr, "Invalid engine internal data (err #3)");
#endif

    aboutToClose   = false;
    curPluginCount = 0;
    nextPluginId   = 0;

    switch (options.processMode)
    {
    case ENGINE_PROCESS_MODE_CONTINUOUS_RACK:
        maxPluginNumber = MAX_RACK_PLUGINS;
        options.forceStereo = true;
        break;
    case ENGINE_PROCESS_MODE_PATCHBAY:
        maxPluginNumber = MAX_PATCHBAY_PLUGINS;
        break;
    case ENGINE_PROCESS_MODE_BRIDGE:
        maxPluginNumber = 1;
        break;
    default:
        maxPluginNumber = MAX_DEFAULT_PLUGINS;
        break;
    }

    switch (options.processMode)
    {
    case ENGINE_PROCESS_MODE_CONTINUOUS_RACK:
    case ENGINE_PROCESS_MODE_PATCHBAY:
    case ENGINE_PROCESS_MODE_BRIDGE:
        events.in  = new EngineEvent[kMaxEngineEventInternalCount];
        events.out = new EngineEvent[kMaxEngineEventInternalCount];
        carla_zeroStructs(events.in,  kMaxEngineEventInternalCount);
        carla_zeroStructs(events.out, kMaxEngineEventInternalCount);
        break;
    default:
        break;
    }

    nextPluginId = maxPluginNumber;

    name = clientName;
    name.toBasic();

    timeInfo.clear();

#if defined(HAVE_LIBLO) && !defined(BUILD_BRIDGE)
    if (options.oscEnabled)
    {
        osc.init(clientName, options.oscPortTCP, options.oscPortUDP);
        oscData = osc.getControlData();
    }
#endif

#ifndef BUILD_BRIDGE_ALTERNATIVE_ARCH
    plugins = new EnginePluginData[maxPluginNumber];
    carla_zeroStructs(plugins, maxPluginNumber);
    xruns = 0;
    dspLoad = 0.0f;
#endif

    nextAction.clearAndReset();
    thread.startThread();

    return true;
}

void CarlaEngine::ProtectedData::close()
{
    CARLA_SAFE_ASSERT(name.isNotEmpty());
#ifndef BUILD_BRIDGE_ALTERNATIVE_ARCH
    CARLA_SAFE_ASSERT(plugins != nullptr);
    CARLA_SAFE_ASSERT(nextPluginId == maxPluginNumber);
#endif

    aboutToClose = true;

    thread.stopThread(500);
    nextAction.clearAndReset();

#if defined(HAVE_LIBLO) && !defined(BUILD_BRIDGE)
    osc.close();
    oscData = nullptr;
#endif

    aboutToClose    = false;
    curPluginCount  = 0;
    maxPluginNumber = 0;
    nextPluginId    = 0;

#ifndef BUILD_BRIDGE_ALTERNATIVE_ARCH
    if (plugins != nullptr)
    {
        delete[] plugins;
        plugins = nullptr;
    }
#endif

    events.clear();
    name.clear();
}

void CarlaEngine::ProtectedData::initTime(const char* const features)
{
    time.init(bufferSize, sampleRate);

#if defined(HAVE_HYLIA) && !defined(BUILD_BRIDGE)
    const bool linkEnabled = features != nullptr && std::strstr(features, ":link:") != nullptr;
    time.enableLink(linkEnabled);
#else
    return;

    // unused
    (void)features;
#endif
}

// -----------------------------------------------------------------------

#ifndef BUILD_BRIDGE_ALTERNATIVE_ARCH
void CarlaEngine::ProtectedData::doPluginRemove(const uint pluginId) noexcept
{
    CARLA_SAFE_ASSERT_RETURN(curPluginCount > 0,);
    CARLA_SAFE_ASSERT_RETURN(pluginId < curPluginCount,);
    --curPluginCount;

    // move all plugins 1 spot backwards
    for (uint i=pluginId; i < curPluginCount; ++i)
    {
        CarlaPlugin* const plugin(plugins[i+1].plugin);

        CARLA_SAFE_ASSERT_BREAK(plugin != nullptr);

        plugin->setId(i);

        plugins[i].plugin = plugin;
        carla_zeroFloats(plugins[i].peaks, 4);
    }

    const uint id(curPluginCount);

    // reset last plugin (now removed)
    plugins[id].plugin = nullptr;
    carla_zeroFloats(plugins[id].peaks, 4);
}

void CarlaEngine::ProtectedData::doPluginsSwitch(const uint idA, const uint idB) noexcept
{
    CARLA_SAFE_ASSERT_RETURN(curPluginCount >= 2,);

    CARLA_SAFE_ASSERT_RETURN(idA < curPluginCount,);
    CARLA_SAFE_ASSERT_RETURN(idB < curPluginCount,);

    CarlaPlugin* const pluginA(plugins[idA].plugin);
    CARLA_SAFE_ASSERT_RETURN(pluginA != nullptr,);

    CarlaPlugin* const pluginB(plugins[idB].plugin);
    CARLA_SAFE_ASSERT_RETURN(pluginB != nullptr,);

    pluginA->setId(idB);
    plugins[idA].plugin = pluginB;

    pluginB->setId(idA);
    plugins[idB].plugin = pluginA;
}
#endif

void CarlaEngine::ProtectedData::doNextPluginAction() noexcept
{
    if (! nextAction.mutex.tryLock())
        return;

    const EnginePostAction opcode    = nextAction.opcode;
    const bool             needsPost = nextAction.needsPost;
#ifndef BUILD_BRIDGE_ALTERNATIVE_ARCH
    const uint             pluginId  = nextAction.pluginId;
    const uint             value     = nextAction.value;
#endif

    nextAction.opcode    = kEnginePostActionNull;
    nextAction.pluginId  = 0;
    nextAction.value     = 0;
    nextAction.needsPost = false;

    nextAction.mutex.unlock();

    switch (opcode)
    {
    case kEnginePostActionNull:
        break;
    case kEnginePostActionZeroCount:
        curPluginCount = 0;
        break;
#ifndef BUILD_BRIDGE_ALTERNATIVE_ARCH
    case kEnginePostActionRemovePlugin:
        doPluginRemove(pluginId);
        break;
    case kEnginePostActionSwitchPlugins:
        doPluginsSwitch(pluginId, value);
        break;
#endif
    }

    if (needsPost)
    {
        if (nextAction.sem != nullptr)
            carla_sem_post(*nextAction.sem);
        nextAction.postDone = true;
    }
}

// -----------------------------------------------------------------------
// PendingRtEventsRunner

static int64_t getTimeInMicroseconds() noexcept
{
#if defined(CARLA_OS_MAC) || defined(CARLA_OS_WIN)
    struct timeval tv;
    gettimeofday(&tv, nullptr);

    return (tv.tv_sec * 1000000) + tv.tv_usec;
#else
    struct timespec ts;
# ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
# else
    clock_gettime(CLOCK_MONOTONIC, &ts);
# endif

    return (ts.tv_sec * 1000000) + (ts.tv_nsec / 1000);
#endif
}

PendingRtEventsRunner::PendingRtEventsRunner(CarlaEngine* const engine,
                                             const uint32_t frames,
                                             const bool calcDSPLoad) noexcept
    : pData(engine->pData),
      prevTime(calcDSPLoad ? getTimeInMicroseconds() : 0)
{
    pData->time.preProcess(frames);
}

PendingRtEventsRunner::~PendingRtEventsRunner() noexcept
{
    pData->doNextPluginAction();

#ifndef BUILD_BRIDGE_ALTERNATIVE_ARCH
    if (prevTime > 0)
    {
        const int64_t newTime = getTimeInMicroseconds();

        if (newTime < prevTime)
            return;

        const double timeDiff = static_cast<double>(newTime - prevTime) / 1000000.0;
        const double maxTime = pData->bufferSize / pData->sampleRate;
        const float dspLoad = static_cast<float>(timeDiff / maxTime) * 100.0f;

        if (dspLoad > pData->dspLoad)
            pData->dspLoad = std::min(100.0f, dspLoad);
        else
            pData->dspLoad *= static_cast<float>(1.0 - maxTime) + 1e-12f;
    }
#endif
}

// -----------------------------------------------------------------------
// ScopedActionLock

ScopedActionLock::ScopedActionLock(CarlaEngine* const engine,
                                   const EnginePostAction action,
                                   const uint pluginId,
                                   const uint value) noexcept
    : pData(engine->pData)
{
    CARLA_SAFE_ASSERT_RETURN(action != kEnginePostActionNull,);

    {
        const CarlaMutexLocker cml(pData->nextAction.mutex);

        CARLA_SAFE_ASSERT_RETURN(pData->nextAction.opcode == kEnginePostActionNull,);

        pData->nextAction.opcode    = action;
        pData->nextAction.pluginId  = pluginId;
        pData->nextAction.value     = value;
        pData->nextAction.needsPost = engine->isRunning();
        pData->nextAction.postDone  = false;
    }

   #ifdef BUILD_BRIDGE
    #define ACTION_MSG_PREFIX "Bridge: "
   #else
    #define ACTION_MSG_PREFIX ""
   #endif

    if (pData->nextAction.needsPost)
    {
       #if defined(DEBUG) || defined(BUILD_BRIDGE)
        // block wait for unlock on processing side
        carla_stdout(ACTION_MSG_PREFIX "ScopedPluginAction(%i) - blocking START", pluginId);
       #endif

        bool engineStoppedWhileWaiting = false;

        if (! pData->nextAction.postDone)
        {
            for (int i = 10; --i >= 0;)
            {
                if (pData->nextAction.sem != nullptr)
                {
                    if (carla_sem_timedwait(*pData->nextAction.sem, 200))
                        break;
                }
                else
                {
                    carla_msleep(200);
                }

                if (! engine->isRunning())
                {
                    engineStoppedWhileWaiting = true;
                    break;
                }
            }
        }

       #if defined(DEBUG) || defined(BUILD_BRIDGE)
        carla_stdout(ACTION_MSG_PREFIX "ScopedPluginAction(%i) - blocking DONE", pluginId);
       #endif

        // check if anything went wrong...
        if (! pData->nextAction.postDone)
        {
            bool needsCorrection = false;

            {
                const CarlaMutexLocker cml(pData->nextAction.mutex);

                if (pData->nextAction.opcode != kEnginePostActionNull)
                {
                    needsCorrection = true;
                    pData->nextAction.needsPost = false;
                }
            }

            if (needsCorrection)
            {
                pData->doNextPluginAction();

                if (! engineStoppedWhileWaiting)
                    carla_stderr2(ACTION_MSG_PREFIX "Failed to wait for engine, is audio not running?");
            }
        }
    }
    else
    {
        pData->doNextPluginAction();
    }
}

ScopedActionLock::~ScopedActionLock() noexcept
{
    CARLA_SAFE_ASSERT(pData->nextAction.opcode == kEnginePostActionNull);
}

// -----------------------------------------------------------------------
// ScopedThreadStopper

ScopedThreadStopper::ScopedThreadStopper(CarlaEngine* const e) noexcept
    : engine(e),
      pData(e->pData)
{
    pData->thread.stopThread(500);
}

ScopedThreadStopper::~ScopedThreadStopper() noexcept
{
    if (engine->isRunning() && ! pData->aboutToClose)
        pData->thread.startThread();
}

// -----------------------------------------------------------------------
// ScopedEngineEnvironmentLocker

ScopedEngineEnvironmentLocker::ScopedEngineEnvironmentLocker(CarlaEngine* const engine) noexcept
    : pData(engine->pData)
{
    pData->envMutex.lock();
}

ScopedEngineEnvironmentLocker::~ScopedEngineEnvironmentLocker() noexcept
{
    pData->envMutex.unlock();
}

// -----------------------------------------------------------------------

CARLA_BACKEND_END_NAMESPACE
