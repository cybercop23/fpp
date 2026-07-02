/*
 * This file is part of the Falcon Player (FPP) and is Copyright (C)
 * 2013-2022 by the Falcon Player Developers.
 *
 * The Falcon Player (FPP) is free software, and is covered under
 * multiple Open Source licenses.  Please see the included 'LICENSES'
 * file for descriptions of what files are covered by each license.
 *
 * This source file is covered under the LGPL v2.1 as described in the
 * included LICENSE.LGPL file.
 */

#include "fpp-pch.h"

#include "fpp-json.h"

#include <cstring>
#include <errno.h>
#include <unistd.h>

#include "../commands/Commands.h"
#include "../common.h"
#include "../log.h"

#include "ThreadedChannelOutput.h"

ThreadedChannelOutput::ThreadedChannelOutput(unsigned int startChannel,
                                             unsigned int channelCount) :
    ChannelOutput(startChannel, channelCount),
    m_threadIsRunning(0),
    m_runThread(0),
    m_dataWaiting(0),
    m_useDoubleBuffer(0),
    m_maxWait(0),
    m_inBuf(NULL),
    m_outBuf(NULL) {
    // std::thread/std::mutex/std::condition_variable are default
    // constructed above (member declaration order in the header), no
    // explicit init needed the way pthread_mutex_init()/pthread_cond_init()
    // were required.
}

ThreadedChannelOutput::~ThreadedChannelOutput() {
    // Unlike pthread_t, std::thread's destructor calls std::terminate()
    // if the thread is still joinable (i.e. StopOutputThread() was never
    // called, e.g. a subclass that skips Close()). The pthread version had
    // no such requirement. This is a safety net only; normal shutdown
    // already goes through Close() -> StopOutputThread() -> join().
    if (m_thread.joinable()) {
        StopOutputThread();
    }
}

int ThreadedChannelOutput::Init(void) {
    LogDebug(VB_CHANNELOUT, "ThreadedChannelOutput::Init()\n");

    if (m_useDoubleBuffer) {
        m_inBuf = new unsigned char[m_channelCount];
        m_outBuf = new unsigned char[m_channelCount];
    }
    StartOutputThread();
    DumpConfig();

    return 1;
}

int ThreadedChannelOutput::Init(Json::Value config) {
    return ChannelOutput::Init(config) && Init();
}

int ThreadedChannelOutput::Close(void) {
    LogDebug(VB_CHANNELOUT, "ThreadedChannelOutput::Close()\n");

    StopOutputThread();

    if (m_useDoubleBuffer) {
        delete[] m_inBuf;
        delete[] m_outBuf;
    }

    return ChannelOutput::Close();
}

int ThreadedChannelOutput::SendData(unsigned char* channelData) {
    LogExcess(VB_CHANNELOUT, "ThreadedChannelOutput::SendData(%p)\n", channelData);

    if (m_useDoubleBuffer) {
        std::lock_guard<std::mutex> lock(m_bufLock);
        memcpy(m_inBuf, channelData, m_channelCount);
        m_dataWaiting = 1;
    } else {
        m_outBuf = channelData;
        m_dataWaiting = 1;
    }

    m_sendCond.notify_one();
    return 0;
}

int ThreadedChannelOutput::SendOutputBuffer(void) {
    LogExcess(VB_CHANNELOUT, "ChannelOutput::SendOutputBuffer()\n");

    if (m_useDoubleBuffer) {
        std::lock_guard<std::mutex> lock(m_bufLock);
        memcpy(m_outBuf, m_inBuf, m_channelCount);
        m_dataWaiting = 0;
    } else {
        m_dataWaiting = 0;
    }

    RawSendData(m_outBuf);
    return m_channelCount;
}

void ThreadedChannelOutput::DumpConfig(void) {
    ChannelOutput::DumpConfig();
    LogDebug(VB_CHANNELOUT, "    Thread Running   : %u\n", m_threadIsRunning);
    LogDebug(VB_CHANNELOUT, "    Run Thread       : %u\n", m_runThread);
    LogDebug(VB_CHANNELOUT, "    Data Waiting     : %u\n", m_dataWaiting);
}

int ThreadedChannelOutput::StartOutputThread(void) {
    LogDebug(VB_CHANNELOUT, "ThreadedChannelOutput::StartOutputThread()\n");

    m_runThread = 1;

    // std::thread has no "not a member of the class" trampoline
    // requirement like pthread_create() did (no void*(*)(void*) function
    // pointer signature to satisfy), so OutputThread() can be invoked
    // directly via a lambda. The lambda replicates the trampoline's
    // original debug log line so log output is unchanged.
    //
    // Unlike pthread_create(), which reports failure via its int return
    // value, std::thread's constructor reports failure by throwing
    // std::system_error. Catch it here so the same error-code -> message
    // mapping and the same "leave m_threadID/m_thread unset" behavior on
    // failure are preserved.
    try {
        m_thread = std::thread([this]() {
            LogDebug(VB_CHANNELOUT, "ThreadedChannelOutput::RunChannelOutputThread()\n");
            OutputThread();
        });
    } catch (const std::system_error& ex) {
        char msg[256];

        m_runThread = 0;
        switch (ex.code().value()) {
        case EAGAIN:
            strcpy(msg, "Insufficient Resources");
            break;
        case EINVAL:
            strcpy(msg, "Invalid settings");
            break;
        case EPERM:
            strcpy(msg, "Invalid Permissions");
            break;
        default:
            // Any other code (std::thread isn't limited to the three
            // pthread_create errno values) — report it rather than reading
            // msg uninitialized, which the old pthread version did.
            snprintf(msg, sizeof(msg), "%s (code %d)", ex.what(), ex.code().value());
            break;
        }
        LogErr(VB_CHANNELOUT, "ERROR creating ChannelOutput thread: %s\n", msg);
    }

    while (!m_threadIsRunning)
        usleep(10000);

    return 0;
}

int ThreadedChannelOutput::StopOutputThread(void) {
    LogDebug(VB_CHANNELOUT, "ChannelOutput::StopOutputThread()\n");

    if (!m_thread.joinable())
        return -1;

    m_runThread = 0;

    m_sendCond.notify_one();

    int loops = 0;
    // Wait up to 110ms for data to be sent
    while ((m_dataWaiting) &&
           (m_threadIsRunning) &&
           (loops++ < 11))
        usleep(10000);

    // NOTE: preserved as-is from the pthread version. m_bufLock is held
    // across the join() below. This only avoids deadlock because by this
    // point OutputThread() is expected to be parked in
    // m_sendCond.wait()/wait_for() on m_sendLock (not holding m_bufLock)
    // per the 110ms settle loop above; OutputThread() does not need
    // m_bufLock again on its way out once m_runThread is 0 (it just
    // `continue`s straight to the top-of-loop check and exits). Do not
    // "fix" this ordering without auditing that invariant.
    std::unique_lock<std::mutex> lock(m_bufLock);

    if (!m_thread.joinable()) {
        lock.unlock();
        return -1;
    }

    m_thread.join();
    lock.unlock();

    return 0;
}

void ThreadedChannelOutput::OutputThread(void) {
    LogDebug(VB_CHANNELOUT, "ThreadedChannelOutput::OutputThread()\n");
    SetThreadName("FPP-" + m_outputType);

    long long wakeTime = GetTime();

    // unique_lock, not locked yet (mirrors the pthread version which did
    // not hold m_sendLock outside the loop body either); locked/unlocked
    // explicitly at the same points the old pthread_mutex_lock/unlock
    // calls were, rather than relying on scope-based RAII, so the
    // lock/unlock coverage stays a 1:1 match with the original.
    std::unique_lock<std::mutex> sendLock(m_sendLock, std::defer_lock);

    m_threadIsRunning = 1;
    LogDebug(VB_CHANNELOUT, "ThreadedChannelOutput thread started\n");

    while (m_runThread) {
        // Wait for more data
        sendLock.lock();
        long long nowTime = GetTime();
        LogExcess(VB_CHANNELOUT, "ThreadedChannelOutput thread: sent: %lld, elapsed: %lld\n",
                  nowTime, nowTime - wakeTime);

        if (m_useDoubleBuffer)
            m_bufLock.lock();

        if (m_dataWaiting || m_maxWait) {
            if (m_useDoubleBuffer)
                m_bufLock.unlock();

            // Old code computed an absolute CLOCK_REALTIME deadline
            // (now + duration) via gettimeofday()/timespec math for
            // pthread_cond_timedwait(). That duration was:
            //   - m_maxWait milliseconds, when m_maxWait is set
            //   - 200ms otherwise ((tv_usec + 200000us) rolled to nsec)
            // wait_for() takes that same duration directly, so the
            // timespec/overflow-carry arithmetic is no longer needed.
            std::chrono::milliseconds waitDuration(m_maxWait ? m_maxWait : 200);

            // No predicate is passed (matches pthread_cond_timedwait,
            // which also does not loop internally): a spurious or timed
            // wakeup here just falls through to the checks below, and
            // the outer while(m_runThread) loop is what re-evaluates
            // state on the next pass, exactly as before.
            m_sendCond.wait_for(sendLock, waitDuration);
        } else {
            if (m_useDoubleBuffer)
                m_bufLock.unlock();

            // No predicate here either, matching pthread_cond_wait().
            m_sendCond.wait(sendLock);
        }

        sendLock.unlock();

        if (!m_runThread)
            continue;

        wakeTime = GetTime();
        LogExcess(VB_CHANNELOUT, "ThreadedChannelOutput thread: woke: %lld\n", wakeTime);

        // See if there is any data waiting to process or if we timed out
        if (m_useDoubleBuffer)
            m_bufLock.lock();

        if (m_dataWaiting) {
            if (m_useDoubleBuffer)
                m_bufLock.unlock();

            SendOutputBuffer();
        } else {
            if (m_useDoubleBuffer)
                m_bufLock.unlock();
            WaitTimedOut();
        }
    }

    LogDebug(VB_CHANNELOUT, "ThreadedChannelOutput thread complete\n");
    m_threadIsRunning = 0;
}
