#pragma once
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

#if __has_include(<xf86drm.h>)
#include "FrameBuffer.h"
#include <xf86drm.h>
#include <xf86drmMode.h>
#define HAS_KMS_FB

#include <map>
#include <string>
#include <vector>

struct DumbBuffer {
    uint32_t handle = 0;
    uint32_t fb_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t format = 0;
    uint64_t size = 0;
    void* mapped = nullptr;
};

struct CardInfo {
    int fd = -1;
    std::string path;
    std::vector<uint32_t> reservedConnectors;
    std::vector<uint32_t> reservedCrtcs;
    std::vector<uint32_t> reservedPlanes;
};

class KMSFrameBuffer : public FrameBuffer {
public:
    KMSFrameBuffer();
    virtual ~KMSFrameBuffer();

    virtual int InitializeFrameBuffer() override;
    virtual void DestroyFrameBuffer() override;
    virtual void SyncLoop() override;
    virtual void SyncDisplay(bool pageChanged = false) override;

    virtual void EnableDisplay() override;
    virtual void DisableDisplay() override;

    int m_cardFd = -1;
    uint32_t m_connectorId = 0;
    uint32_t m_crtcId = 0;
    uint32_t m_planeId = 0;
    std::string m_connectorName;
    drmModeModeInfo m_mode = {};
    DumbBuffer m_fb[3] = {};
    bool m_displayEnabled = false;

    static std::atomic_int FRAMEBUFFER_COUNT;
    static std::vector<CardInfo*> CARDS;

private:
    static std::string ConnectorFullName(int fd, drmModeConnectorPtr conn);
    static bool CreateDumbBuffer(int fd, uint32_t width, uint32_t height, uint32_t format, DumbBuffer& buf);
    static void DestroyDumbBuffer(int fd, DumbBuffer& buf);
    uint32_t FindPlaneForCrtc(int fd, uint32_t crtcId, uint32_t format);

    // Drain the completion event of an outstanding (m_flipPending) page flip.
    // Blocks in poll() until the flip retires at vblank (or timeoutMs elapses),
    // so it costs no CPU while waiting.
    void WaitForPendingFlip(int timeoutMs);
    static void PageFlipHandler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void* data);

    // True while a drmModePageFlip has been queued but its vblank completion
    // event has not yet been drained.  Guarded by mediaOutputLock (only touched
    // from SyncDisplay / WaitForPendingFlip, both of which hold that lock).
    bool m_flipPending = false;

    // Flip-path health counters (guarded by mediaOutputLock like m_flipPending).
    // Logged rate-limited at Warn so brief output hitches are visible without
    // debug logging: a stutter on the panel with these counters unchanged means
    // the delay is NOT in the KMS flip path.
    uint32_t m_flipWaitTimeouts = 0;
    uint32_t m_flipRejects = 0;
    uint32_t m_slowSyncs = 0;
    int m_maxSyncMS = 0;
    long long m_lastFlipWarnMS = 0;

    CardInfo* m_cardInfo = nullptr;
};

#endif
