/*
 * This file is part of the Falcon Player (FPP) and is Copyright (C)
 * 2013-2022 by the Falcon Player Developers.
 *
 * The Falcon Player (FPP) is free software, and is covered under
 * multiple Open Source licenses.  Please see the included 'LICENSES'
 * file for descriptions of what files are covered by each license.
 *
 * This source file is covered under the CC-BY-ND as described in the
 * included LICENSE.CC-BY-ND file.  This file may be modified for
 * personal use, but modified copies MAY NOT be redistributed in any form.
 */

#include "fpp-pch.h"

#include "fpp-json.h"

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <tuple>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

// FPP includes
#include "../../Sequence.h"
#include "../../Warnings.h"
#include "../../common.h"
#include "../../log.h"
#include "../../settings.h"

#include "BBShiftPanel.h"
#include "../CapeUtils/CapeUtils.h"
#include "../../pru/SMEMRing.hp"
#include "util/BBBUtils.h"

#include "MapPixelsByDepth16.h"

#include "channeloutput/PanelInterleaveHandler.h"
#include "overlays/PixelOverlay.h"

#include "Plugin.h"
class BBShiftPanelPlugin : public FPPPlugins::Plugin, public FPPPlugins::ChannelOutputPlugin {
public:
    BBShiftPanelPlugin() :
        FPPPlugins::Plugin("BBShiftPanel") {
    }
    virtual ChannelOutput* createChannelOutput(unsigned int startChannel, unsigned int channelCount) override {
        return new BBShiftPanelOutput(startChannel, channelCount);
    }
};

extern "C" {
FPPPlugins::Plugin* createPlugin() {
    return new BBShiftPanelPlugin();
}
}

constexpr int ADDRESSING_MODE_STANDARD = 0;
constexpr int ADDRESSING_MODE_DIRECT = 2;
constexpr int ADDRESSING_MODE_FM6363C = 51;

constexpr int PWM_COMMAND_SYNC = 0x0001;
constexpr int PWM_COMMAND_REGISTERS = 0x0002;
constexpr int PWM_COMMAND_STARTGCLK = 0x0004;
constexpr int PWM_COMMAND_DATA = 0x0008;
constexpr int PWM_COMMAND_HALT = 0xFFFF;

// Note: the ordering of the bit planes within a frame (formerly a set of
// hand-tuned BIT_ORDERS tables) is now computed by buildStrideSchedule(),
// which also splits the long MSB on-times into multiple pulses spread
// across the frame to raise the perceived refresh rate.

static const std::vector<std::string> PRU_PINS = { "P1-20", "P1-29", "P1-31", "P1-33", "P1-36",
                                                   "P2-02", "P2-04", "P2-06", "P2-08",
                                                   "P2-18", "P2-20", "P2-22", "P2-24",
                                                   "P2-28", "P2-30", "P2-32", "P2-33",
                                                   "P2-34", "P2-35" };

static const std::vector<std::string> PRU0_PWM_PINS = {
    "P1-31", "P2-28", "P2-30", "P2-32", "P2-34", "P1-36"
};

constexpr int MAX_OUTPUTS = 16;

// how much the pump thread copies into the shared memory ring at a time;
// a multiple of 48 and 8 that divides SMEM_RING_DEFAULT_SIZE evenly
constexpr uint32_t PUMP_BLOCK_SIZE = 3072;

BBShiftPanelOutput::BBShiftPanelOutput(unsigned int startChannel, unsigned int channelCount) :
    ChannelOutput(startChannel, channelCount) {
    LogDebug(VB_CHANNELOUT, "BBShiftPanelOutput::BBShiftPanelOutput(%u, %u)\n",
             startChannel, channelCount);
}

void BBShiftPanelOutput::StartingOutput() {
    // StoppingOutput is also called when the channel output thread simply
    // goes idle, so the stopping flag has to clear when output resumes
    m_stopping = false;
}

void BBShiftPanelOutput::StoppingOutput() {
    // Called when the channel output thread stops and before the output is
    // deleted on a config reload; the thread may still be inside
    // PrepData/SendData, so refuse new work and wait for any in-flight call
    // to drain so a teardown cannot race them.
    m_stopping = true;
    while (m_inFlight > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

BBShiftPanelOutput::~BBShiftPanelOutput() {
    LogDebug(VB_CHANNELOUT, "BBShiftPanelOutput::~BBShiftPanelOutput()\n");

    m_stopping = true;
    while (m_inFlight > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bgThreadsRunning = false;
    bgTaskCondVar.notify_all();
    while (bgThreadCount > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    StopPRU();

    if (m_heapBuffers) {
        for (auto& b : outputBuffers) {
            if (b) {
                free(b);
                b = nullptr;
            }
        }
    }

    if (channelOffsets)
        delete[] channelOffsets;
    if (currentChannelData)
        delete[] currentChannelData;

    if (pru)
        delete pru;
    if (m_matrix)
        delete m_matrix;
    if (m_panelMatrix)
        delete m_panelMatrix;
}

bool BBShiftPanelOutput::isPWMPanel() {
    if (m_addressingMode == ADDRESSING_MODE_FM6363C) {
        return true;
    }
    return false;
}

int BBShiftPanelOutput::Init(Json::Value config) {
    LogDebug(VB_CHANNELOUT, "BBShiftPanelOutput::Init(JSON)\n");

    for (auto& pinName : PRU_PINS) {
        const PinCapabilities& pin = PinCapabilities::getPinByName(pinName);
        pin.configPin("pru1out", true);
    }

    Json::Value root;
    std::string subType = config["configName"].asString();
    int outputs = CapeUtils::INSTANCE.getLicensedOutputs();
    if (!CapeUtils::INSTANCE.getPanelConfig(subType, root)) {
        LogErr(VB_CHANNELOUT, "Could not read panel pin configuration for %s\n", subType.c_str());
        return 0;
    }
    m_numOutputs = root["outputs"].size();
    if (m_numOutputs > MAX_OUTPUTS) {
        m_numOutputs = MAX_OUTPUTS;
    }
    m_numOutputSlots = 8;
    for (int i = 0; i < m_numOutputs; i++) {
        Json::Value s = root["outputs"][i];
        std::string pinName = s["pin"].asString();
        const BBBPinCapabilities* pin = (const BBBPinCapabilities*)(PinCapabilities::getPinByName(pinName).ptr());
        outputPin[i] = pin->pruPin(1);
        outputBank[i] = 0;
        if (s.isMember("index")) {
            outputBank[i] = s["index"].asInt();
            if (outputBank[i] > 0) {
                m_numOutputSlots = 16;
            }
        }
    }
    if (root.isMember("singlePRU")) {
        singlePRU = root["singlePRU"].asBool();
    }
    // singlePRU = true;
    if (!singlePRU) {
        // if not using a single PRU, then we need to change the OE pin to the other PRU
        const PinCapabilities& pin = PinCapabilities::getPinByName("P1-36");
        pin.configPin("pru0out", true);
    }
    m_panelWidth = config["panelWidth"].asInt();
    m_panelHeight = config["panelHeight"].asInt();
    if (!m_panelWidth) {
        m_panelWidth = 32;
    }
    if (!m_panelHeight) {
        m_panelHeight = 16;
    }

    m_addressingMode = config["panelRowAddressType"].asInt();

    if (isPWMPanel()) {
        for (auto& pinName : PRU0_PWM_PINS) {
            const PinCapabilities& pin = PinCapabilities::getPinByName(pinName);
            pin.configPin("pru0out", true);
        }
    }

    m_invertedData = config["invertedData"].asInt();
    m_colorOrder = ColorOrderFromString(config["colorOrder"].asString());

    m_panelMatrix = new PanelMatrix(m_panelWidth, m_panelHeight, m_invertedData);
    if (!m_panelMatrix) {
        LogErr(VB_CHANNELOUT, "BBShiftPanelOutput: Unable to create PanelMatrix\n");
        return 0;
    }
    bool usesOutput[MAX_OUTPUTS] = { false };
    for (int i = 0; i < config["panels"].size(); i++) {
        Json::Value p = config["panels"][i];
        if (p["outputNumber"].asInt() <= outputs) {
            char orientation = 'N';
            const char* o = p["orientation"].asString().c_str();

            if (o && *o) {
                orientation = o[0];
            }

            if (p["colorOrder"].asString() == "") {
                p["colorOrder"] = ColorOrderToString(m_colorOrder);
            }

            m_panelMatrix->AddPanel(p["outputNumber"].asInt(),
                                    p["panelNumber"].asInt(),
                                    orientation,
                                    p["xOffset"].asInt(), p["yOffset"].asInt(),
                                    ColorOrderFromString(p["colorOrder"].asString()));
            usesOutput[p["outputNumber"].asInt()] = true;
            if (p["panelNumber"].asInt() > m_longestChain) {
                m_longestChain = p["panelNumber"].asInt();
            }
        }
    }
    m_longestChain++;

    // get the dimensions of the matrix
    m_panels = m_panelMatrix->PanelCount();
    m_width = m_panelMatrix->Width();
    m_height = m_panelMatrix->Height();

    if (config.isMember("brightness")) {
        m_brightness = config["brightness"].asInt();
    }
    if (m_brightness < 1 || m_brightness > 10) {
        m_brightness = 10;
    }
    if (config.isMember("panelColorDepth")) {
        m_colorDepth = config["panelColorDepth"].asInt();
    }

    if (config.isMember("panelOutputOrder")) {
        m_outputByRow = config["panelOutputOrder"].asBool();
    }
    if (config.isMember("panelOutputBlankRow")) {
        m_outputBlankData = config["panelOutputBlankRow"].asBool();
    }
    if (m_colorDepth < 0) {
        m_colorDepth = -m_colorDepth;
        m_outputBlankData = true;
    }
    if (m_colorDepth > 12 || m_colorDepth < 6) {
        m_colorDepth = 8;
    }

    if (isPWMPanel()) {
        m_colorDepth = 16;
    }

    float gamma = 2.2;
    if (config.isMember("gamma")) {
        gamma = atof(config["gamma"].asString().c_str());
    }
    setupGamma(gamma);

    if (config.isMember("panelInterleave")) {
        m_panelInterleave = config["panelInterleave"].asString();
    }

    m_panelScan = config["panelScan"].asInt();
    // printf("Interleave: %d     Scan: %d    ZZI: %d    ZZCI:  %d    SI: %d\n", m_interleave, m_panelScan, zigZagInterleave, zigZagClusterInterleave, stripeInterleave);
    if (m_panelScan == 0) {
        //  default scan is 1/2 the height of the panel
        m_panelScan = m_panelHeight / 2;
    }

    m_channelCount = m_width * m_height * 3;

    m_matrix = new Matrix(m_startChannel, m_width, m_height);
    if (config.isMember("subMatrices")) {
        for (int i = 0; i < config["subMatrices"].size(); i++) {
            Json::Value sm = config["subMatrices"][i];

            m_matrix->AddSubMatrix(
                sm["enabled"].asInt(),
                sm["startChannel"].asInt() - 1,
                sm["width"].asInt(),
                sm["height"].asInt(),
                sm["xOffset"].asInt(),
                sm["yOffset"].asInt());
        }
    }
    if (!setupChannelOffsets()) {
        return 0;
    }
    pru = new BBBPru(1, true, true);
    pruData = (BBShiftPanelData*)pru->data_ram;
    if (!isPWMPanel()) {
        buildStrideSchedule();
    }
    if (StartPRU() == 0) {
        return 0;
    }
    if (isPWMPanel()) {
        setupPWMRegisters();
    } else {
        setupBrightnessValues();
    }

    if (PixelOverlayManager::INSTANCE.isAutoCreatePixelOverlayModels()) {
        std::string dd = "LED Panels";
        if (config.isMember("LEDPanelMatrixName") && !config["LEDPanelMatrixName"].asString().empty()) {
            dd = config["LEDPanelMatrixName"].asString();
        }
        if (config.isMember("description")) {
            dd = config["description"].asString();
        }
        std::string desc = dd;
        int count = 0;
        while (PixelOverlayManager::INSTANCE.getModel(desc) != nullptr) {
            count++;
            desc = dd + "-" + std::to_string(count);
        }
        PixelOverlayManager::INSTANCE.addAutoOverlayModel(desc,
                                                          m_startChannel, m_channelCount, 3,
                                                          "H", m_invertedData ? "BL" : "TL",
                                                          m_height, 1);
        m_autoCreatedModelName = desc;
    }

    bgThreadsRunning = true;
    for (int i = 0; i < std::thread::hardware_concurrency(); i++) {
        std::thread th(&BBShiftPanelOutput::runBackgroundTasks, this);
        th.detach();
    }
    return ChannelOutput::Init(config);
}

int BBShiftPanelOutput::StartPRU() {
    if (!pru) {
        pru = new BBBPru(1, true, true);
        pruData = (BBShiftPanelData*)pru->data_ram;
    }
    bool started = true;
    if (isPWMPanel()) {
        pwmPru = new BBBPru(0);
        started &= pwmPru->run("/opt/fpp/src/non-gpl/BBShiftPanel/BBShiftPanel_gclk.out");
        if (m_numOutputSlots == 16) {
            started &= pru->run("/opt/fpp/src/non-gpl/BBShiftPanel/BBShiftPanel_pwm_16.out");
        } else {
            started &= pru->run("/opt/fpp/src/non-gpl/BBShiftPanel/BBShiftPanel_pwm.out");
        }
    } else {
        if (singlePRU) {
            if (m_numOutputSlots == 16) {
                started &= pru->run("/opt/fpp/src/non-gpl/BBShiftPanel/BBShiftPanel_single_16.out");
            } else {
                started &= pru->run("/opt/fpp/src/non-gpl/BBShiftPanel/BBShiftPanel_single.out");
            }
        } else {
            if (m_numOutputSlots == 16) {
                started &= pru->run("/opt/fpp/src/non-gpl/BBShiftPanel/BBShiftPanel_16.out");
            } else {
                started &= pru->run("/opt/fpp/src/non-gpl/BBShiftPanel/BBShiftPanel.out");
            }
            pwmPru = new BBBPru(0);
            started &= pwmPru->run("/opt/fpp/src/non-gpl/BBShiftPanel/BBShiftPanel_oe.out");
        }
    }
    if (!started) {
        LogErr(VB_CHANNELOUT, "BBShiftPanel: Unable to start PRU(s). May require a reboot.\n");
        WarningHolder::AddWarning("BBShiftPanel: Unable to start PRU(s). May require a reboot.");
        return 0;
    }
    // Both panel types are fed through a ring in the PRU shared memory (see
    // SMEMRing.hp); attach() must happen after run() since the firmware load
    // clears the PRU memories (and writes while the PRUSS is powered down
    // are silently dropped).  For now the panels get the entire shared RAM;
    // when panels later share the PRUSS with a pixel string program on the
    // other PRU this becomes the SMEM_RING_SPLIT configuration.
    m_ring.attach(pru, SMEM_RING_DEFAULT_BASE, SMEM_RING_DEFAULT_SIZE, false);
    uint32_t bytesPerPixel = (m_numOutputSlots * 6 * 2) / 16;
    uint32_t strideLen = rowLen * bytesPerPixel;
    uint32_t numStride = m_strideSchedule.empty() ? (numRows * m_colorDepth) : (m_strideSchedule.size() * numRows);
    uint32_t oframeSize = numStride * strideLen;
    // PWM panels consume numRows passes of 16 PWM bit planes of numBlocks
    // (rowLen / 16) blocks of 16 pixels, which works out to the same full
    // frame the pixel mapping produces
    m_frameBytes = oframeSize;
    if (!isPWMPanel()) {
        pruData->numStrides = numStride;
    }
    // Frames live in normal cached memory now that the pump thread streams
    // them to the PRU; the prep threads write cached memory much faster than
    // the uncached PRU DDR region this replaced
    uint32_t allocSize = (oframeSize + 4095) & ~4095;
    for (int x = 0; x < NUM_OUTPUT_BUFFERS; x++) {
        outputBuffers[x] = (uint8_t*)aligned_alloc(4096, allocSize);
        memset(outputBuffers[x], 0, allocSize);
    }
    m_heapBuffers = true;
    m_frontBuffer = nullptr;
    m_pumpRunning = true;
    m_pumpThread = std::thread(&BBShiftPanelOutput::runPumpThread, this);
    return 1;
}

void BBShiftPanelOutput::runPumpThread() {
    struct sched_param sp;
    sp.sched_priority = 50;
    int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (err) {
        LogWarn(VB_CHANNELOUT, "BBShiftPanel: could not set pump thread to SCHED_FIFO: %s\n", strerror(err));
    }
    if (isPWMPanel()) {
        // PWM panels send one frame per DATA command (the panels refresh
        // themselves from their internal PWM); stream once per queued
        // command so the byte flow stays in step with the commands
        while (m_pumpRunning) {
            if (m_pumpedSeq == m_pumpSeq.load(std::memory_order_acquire)) {
                struct timespec ts = { 0, 500000 };
                nanosleep(&ts, nullptr);
                continue;
            }
            ++m_pumpedSeq;
            uint8_t* src = m_frontBuffer.load(std::memory_order_acquire);
            uint32_t srcOff = 0;
            while (srcOff < m_frameBytes && m_pumpRunning) {
                uint32_t n = m_ring.write(src + srcOff, std::min(PUMP_BLOCK_SIZE, m_frameBytes - srcOff));
                if (n == 0) {
                    struct timespec ts = { 0, 150000 };
                    nanosleep(&ts, nullptr);
                    continue;
                }
                srcOff += n;
            }
        }
        return;
    }
    while (m_pumpRunning) {
        // frames stream back to back; a new frame from SendData is picked up
        // at the frame boundary so the PRU always sees whole frames
        uint8_t* src = m_frontBuffer.load(std::memory_order_acquire);
        if (!src) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        uint32_t srcOff = 0;
        while (srcOff < m_frameBytes && m_pumpRunning) {
            uint32_t n = m_ring.write(src + srcOff, std::min(PUMP_BLOCK_SIZE, m_frameBytes - srcOff));
            if (n == 0) {
                // ring full; a full ring lasts ~480us at the max drain rate
                // so a short sleep cannot underrun the PRU
                struct timespec ts = { 0, 150000 };
                nanosleep(&ts, nullptr);
                continue;
            }
            srcOff += n;
        }
    }
}
void BBShiftPanelOutput::StopPRU(bool wait) {
    // Send the stop command
    if (pru) {
        pruData->command = PWM_COMMAND_HALT;
        pruData->result = PWM_COMMAND_HALT;
    }
    __asm__ __volatile__("" ::
                             : "memory");

    if (pru) {
        int cnt = 0;
        // the pump keeps feeding the ring here so the PRU can reach the end
        // of the frame, which is where the halt command is checked
        while (wait && cnt < 25 && pruData->result == PWM_COMMAND_HALT) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            cnt++;
            __asm__ __volatile__("" ::
                                     : "memory");
        }
    }
    // stop the pump before the PRU memory mappings go away
    m_pumpRunning = false;
    if (m_pumpThread.joinable()) {
        m_pumpThread.join();
    }
    if (pru) {
        pru->stop();
        delete pru;
        pru = nullptr;

        if (pwmPru) {
            pwmPru->stop();
            delete pwmPru;
            pwmPru = nullptr;
        }
    }
}

int BBShiftPanelOutput::Close(void) {
    LogDebug(VB_CHANNELOUT, "BBShiftPanelOutput::Close()\n");
    if (!m_autoCreatedModelName.empty()) {
        PixelOverlayManager::INSTANCE.removeAutoOverlayModel(m_autoCreatedModelName);
    }
    for (auto& pinName : PRU_PINS) {
        const PinCapabilities& pin = PinCapabilities::getPinByName(pinName);
        pin.releasePin();
    }
    if (!singlePRU) {
        // if not using a single PRU, then we need to change the OE pin to the other PRU
        const PinCapabilities& pin = PinCapabilities::getPinByName("P1-36");
        pin.releasePin();
    }
    if (isPWMPanel()) {
        for (auto& pinName : PRU0_PWM_PINS) {
            const PinCapabilities& pin = PinCapabilities::getPinByName(pinName);
            pin.releasePin();
        }
    }
    return ChannelOutput::Close();
}

void BBShiftPanelOutput::runBackgroundTasks() {
    ++bgThreadCount;
    while (bgThreadsRunning) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(bgTaskMutex);
            bgTaskCondVar.wait_for(lock, std::chrono::milliseconds(100), [&]() { return !bgTasks.empty(); });
            if (!bgTasks.empty()) {
                task = bgTasks.front();
                bgTasks.pop();
            }
        }
        if (task) {
            task();
        }
    }
    --bgThreadCount;
}

void BBShiftPanelOutput::processTasks(std::atomic<int>& counter) {
    // This must always drain to zero, even during shutdown: the queued tasks
    // reference the calling frame's stack (counter, results) and this object,
    // so returning while any are queued or running leaves them with dangling
    // references.  The pool threads exit without draining the queue, but this
    // loop executes leftover tasks itself so it cannot deadlock.
    std::unique_lock<std::mutex> lock(bgTaskMutex);
    while (counter > 0) {
        std::function<void()> task;
        if (!bgTasks.empty()) {
            task = bgTasks.front();
            bgTasks.pop();
        }
        lock.unlock();
        if (task) {
            task();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        lock.lock();
    }
}

void BBShiftPanelOutput::PrepData(unsigned char* channelData) {
    // if (!isPWMPanel() && FileExists(FPP_DIR_MEDIA("/config/panel_timing.txt"))) {
    //     setupBrightnessValues();
    // }
    LogExcess(VB_CHANNELOUT, "BBShiftPanelOutput::PrepData(%p)\n", channelData);
    ++m_inFlight;
    if (m_stopping) {
        --m_inFlight;
        return;
    }
    m_matrix->OverlaySubMatrices(channelData);
    channelData += m_startChannel;

    std::unique_lock<std::mutex> lock(bgTaskMutex);
    int start = 0;
    int end = 32 * 1024;
    if (end > m_channelCount) {
        end = m_channelCount;
    }
    std::atomic<int> counter(0);
    while (start < m_channelCount) {
        ++counter;
        bgTasks.push([this, channelData, start, end, &counter]() {
            int x = start;
            while (x < end) {
                currentChannelData[channelOffsets[x]] = gammaCurve[channelData[x]];
                x++;
            }
            --counter;
        });
        start = end;
        end += 32 * 1024;
        // make sure we don't go past the end of the channel data
        if (end > m_channelCount) {
            end = m_channelCount;
        }
    }
    lock.unlock();
    bgTaskCondVar.notify_all();
    processTasks(counter);
    if (bgThreadsRunning && !m_stopping) {
        if (isPWMPanel()) {
            PrepDataPWM();
        } else {
            PrepDataShift();
        }
    }
    --m_inFlight;
}

void BBShiftPanelOutput::PrepDataPWM() {
    uint8_t* buf = outputBuffers[currOutputBuffer];

    std::unique_lock<std::mutex> lock(bgTaskMutex);
    std::atomic<int> counter(0);
    for (int curRow = 0; curRow < numRows; curRow++) {
        // Map the pixels for this row
        ++counter;
        bgTasks.push([this, curRow, buf, &counter]() {
            uint32_t start = curRow * rowLen;
            uint32_t end = start + rowLen;

            if (m_numOutputSlots == 16) {
                ispc::MapPixelsForPWM16(currentChannelData, start, end, (uint16_t*)buf);
            } else {
                ispc::MapPixelsForPWM(currentChannelData, start, end, (uint16_t*)buf);
            }
            --counter;
        });
    }
    lock.unlock();
    bgTaskCondVar.notify_all();
    processTasks(counter);

    /*
    for (int x = 0; x < 48; x++) {
        printf("%04x  ", currentChannelData[x]);
    }
    printf("\n");
    for (int x = 0; x < 96; x += 6) {
        printf("%02x %02x %02x %02x %02x %02x\n", buf[x], buf[x + 1], buf[x + 2], buf[x + 3], buf[x + 4], buf[x + 5]);
    }
    printf("\n");
    for (int x = 48; x < 96; x++) {
        printf("%04x  ", currentChannelData[x]);
    }
    printf("\n");
    for (int x = 96; x < (96 * 2); x += 6) {
        printf("%02x %02x %02x %02x %02x %02x\n", buf[x], buf[x + 1], buf[x + 2], buf[x + 3], buf[x + 4], buf[x + 5]);
    }
    printf("\n");
    */

    // wait for the PRU to take the previous command before queueing the next
    while (pruData->command && !m_stopping) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        __asm__ __volatile__("" ::
                                 : "memory");
    }

    // hand the frame to the pump thread; the PRU is paced by the ring so the
    // DATA command can be queued immediately
    m_frontBuffer.store(buf, std::memory_order_release);
    m_pumpSeq.fetch_add(1, std::memory_order_release);

    pruData->numBlocks = rowLen / 16;
    pruData->numRows = numRows;
    pruData->cmd = PWM_COMMAND_DATA;
    __asm__ __volatile__("" ::
                             : "memory");
}

void BBShiftPanelOutput::PrepDataShift() {
    std::array<std::array<uint16_t*, 16>, 32> results;

    uint32_t bytesPerPixel = (m_numOutputSlots * 6 * 2) / 16;
    uint32_t strideLen = rowLen * bytesPerPixel;
    uint8_t* base = outputBuffers[currOutputBuffer];
    int numSlots = m_strideSchedule.size();
    for (int s = 0; s < numSlots; s++) {
        if (!m_strideSchedule[s].primary) {
            continue;
        }
        int b = m_strideSchedule[s].bit;
        for (int r = 0; r < numRows; r++) {
            uint32_t idx = m_outputByRow ? (r * numSlots + s) : (s * numRows + r);
            results[r][b] = (uint16_t*)(base + idx * strideLen);
        }
    }

    /*
        int len = rowLen * numRows * 6 * 8
        uint16_t *d = data;
        for (int x = 0; x < len; x += 8) {
            for (int y = 0; y < 8; y++) {
                uint16_t d2 = *d;
                uint16_t mask = 0x1;
                uint8_t bit = 0x1 << y;
                for (int pos = 0; pos < bits; pos++) {
                    if (d2 & mask) {
                        results[pos][x] |= bit;
                    }
                    mask <<= 1;
                }
                ++d;
            }
        }
    */
    // Use ISPC generated code for the above.  It's about 9x faster
    std::unique_lock<std::mutex> lock(bgTaskMutex);
    std::atomic<int> counter(0);
    for (int curRow = 0; curRow < numRows; curRow++) {
        // Map the pixels for this row
        ++counter;
        bgTasks.push([this, curRow, strideLen, base, &results, &counter]() {
            uint32_t start = curRow * rowLen * 6 * m_numOutputSlots;
            uint32_t end = start + (rowLen * 6 * m_numOutputSlots);
            ispc::MapPixelsByDepth16(currentChannelData, start, end, m_colorDepth,
                                     results[curRow][0], results[curRow][1],
                                     results[curRow][2], results[curRow][3],
                                     results[curRow][4], results[curRow][5],
                                     results[curRow][6], results[curRow][7],
                                     results[curRow][8], results[curRow][9],
                                     results[curRow][10], results[curRow][11],
                                     results[curRow][12], results[curRow][13],
                                     results[curRow][14], results[curRow][15]);
            // fill in the duplicated strides for bits that display as multiple pulses
            for (auto& dup : m_dupCopies[curRow]) {
                memcpy(base + dup.first, base + dup.second, strideLen);
            }
            --counter;
        });
    }
    lock.unlock();
    bgTaskCondVar.notify_all();
    processTasks(counter);
}

int BBShiftPanelOutput::SendData(unsigned char* channelData) {
    LogExcess(VB_CHANNELOUT, "BBShiftPanelOutput::SendData(%p)\n", channelData);
    if (!bgThreadsRunning || m_stopping) {
        return m_channelCount;
    }
    ++m_inFlight;
    if (m_stopping) {
        --m_inFlight;
        return m_channelCount;
    }

    if (!isPWMPanel()) {
        // hand the just-prepared frame to the pump thread; it picks it up at
        // its next frame boundary so the PRU always sees whole frames
        m_frontBuffer.store(outputBuffers[currOutputBuffer], std::memory_order_release);
        pruData->numStrides = m_strideSchedule.size() * numRows;
        pruData->pixelsPerStride = rowLen;
    } else {
        while (pruData->command && !m_stopping) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            __asm__ __volatile__("" ::
                                     : "memory");
        }

        // Send the command to setup the registers
        pruData->numBlocks = rowLen / 16;
        pruData->numRows = numRows;
        pruData->cmd = PWM_COMMAND_REGISTERS | PWM_COMMAND_SYNC | PWM_COMMAND_STARTGCLK;
    }

    __asm__ __volatile__("" ::
                             : "memory");
    currOutputBuffer = (currOutputBuffer + 1) % NUM_OUTPUT_BUFFERS;
    while (outputBuffers[currOutputBuffer] == nullptr) {
        currOutputBuffer = (currOutputBuffer + 1) % NUM_OUTPUT_BUFFERS;
    }
    --m_inFlight;
    return m_channelCount;
}
inline int mapRow(int row, int mode) {
    if (mode == ADDRESSING_MODE_DIRECT) {
        switch (row) {
        case 0:
            return 0x0E;
        case 1:
            return 0x0D;
        case 2:
            return 0x0B;
        case 3:
            return 0x07;
        }
    }
    return row;
}

static int outputRegData(int curidx, uint8_t* odata, uint16_t r, uint16_t g, uint16_t b, int numOutputs = 8) {
    // int sidx = curidx;
    int bytesPerClock = numOutputs == 16 ? 12 : 6;
    for (int x = 0; x < 16; x++) {
        odata[curidx] = r & 0x8000 ? 0xFF : 0x00;
        curidx++;
        odata[curidx] = g & 0x8000 ? 0xFF : 0x00;
        curidx++;
        odata[curidx] = b & 0x8000 ? 0xFF : 0x00;
        curidx++;
        odata[curidx] = r & 0x8000 ? 0xFF : 0x00;
        curidx++;
        odata[curidx] = g & 0x8000 ? 0xFF : 0x00;
        curidx++;
        odata[curidx] = b & 0x8000 ? 0xFF : 0x00;
        curidx++;
        if (numOutputs == 16) {
            // Duplicate for outputs 8-15
            odata[curidx] = r & 0x8000 ? 0xFF : 0x00;
            curidx++;
            odata[curidx] = g & 0x8000 ? 0xFF : 0x00;
            curidx++;
            odata[curidx] = b & 0x8000 ? 0xFF : 0x00;
            curidx++;
            odata[curidx] = r & 0x8000 ? 0xFF : 0x00;
            curidx++;
            odata[curidx] = g & 0x8000 ? 0xFF : 0x00;
            curidx++;
            odata[curidx] = b & 0x8000 ? 0xFF : 0x00;
            curidx++;
        }
        r <<= 1;
        g <<= 1;
        b <<= 1;
    }
    /*
    if (sidx == 0) {
        printf("%04X %04X %04X\n", r, g, b);
        for (int x = 0; x < 16; x++) {
            printf(" %02x %02x %02x %02x %02x %02x\n",
                   odata[sidx], odata[sidx + 1], odata[sidx + 2],
                   odata[sidx + 3], odata[sidx + 4], odata[sidx + 5]);
            sidx += 6;
        }
    }
    */
    return curidx;
}

void BBShiftPanelOutput::setupPWMRegisters() {
    /*
    // FM6363 from colorlight + logic analyzer
    // Confirmed via "advanced" tab in LEDVision
    Reg 1: 0x0970:    b0000100101110000
    Reg 2: 0xFF9B  R: b1111111110011011  (default values)
           0xF39B  G: b1111001110011011
           0xDF9B  B: b1101111110011011
    Reg 2: 0xFE01  R: b1111110000000001 (with current stripped off)
           0xF201  G: b1111000000000001
           0xDE01  B: b1101110000000001
    Reg 3: 0x4007     b0100000000000111
    Reg 4: 0x0040     b0000000001000000
    Reg 5: 0x0000     b0000000000000000
    */
    static uint16_t conf_6363[] = {
        // R/G/B triplets
        0x0070, 0x0070, 0x0070,
        0xFC01, 0xF001, 0xDC01,
        0x4007, 0x4007, 0x4007,
        0x0040, 0x0040, 0x0040,
        0x0000, 0x0000, 0x0000
    };

    // create the "data" array of for all outputs of r/g/b triplets for the registers
    // 16 clocks * bytesPerClock * 5 registers
    int bytesPerClock = m_numOutputSlots == 16 ? 12 : 6;
    uint8_t odata[192 * 5];

    // register 1 contains the number of scan lines (rows)
    uint16_t rn = ((numRows - 1) << 8) & 0x3F00;
    int curidx = outputRegData(0, odata, conf_6363[0] | rn, conf_6363[1] | rn, conf_6363[2] | rn, m_numOutputSlots);

    // register 2 contains adjustments for current

    uint16_t b = 205; // stick with default brightness for now
    if (m_brightness >= 5) {
        b = (m_brightness - 5) * 10;
        if (m_brightness > 8) {
            b *= (245 - 64) * 2;
        } else {
            b *= (205 - 64) * 2;
        }
        b /= 100;
        b += 64;
        b <<= 1;
        b &= 0x1FE;
        b |= 0x200;
    } else {
        b = (m_brightness - 1) * 10;
        b *= 100;
        b /= 40;
        b *= (255 - 64);
        b /= 100;
        b += 64;
        b <<= 1;
        b &= 0x1FE;
    }

    curidx = outputRegData(curidx, odata, conf_6363[3] | b, conf_6363[4] | b, conf_6363[5] | b, m_numOutputSlots);
    curidx = outputRegData(curidx, odata, conf_6363[6], conf_6363[7], conf_6363[8], m_numOutputSlots);
    curidx = outputRegData(curidx, odata, conf_6363[9], conf_6363[10], conf_6363[11], m_numOutputSlots);
    curidx = outputRegData(curidx, odata, conf_6363[12], conf_6363[13], conf_6363[14], m_numOutputSlots);

    pru->memcpyToPRU(&pruData->registers[0], &odata[0], curidx);

    while (pruData->command) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        __asm__ __volatile__("" ::
                                 : "memory");
    }

    // Send the command to setup the registers
    pwmPru->data_ram[0] = m_brightness > 8 ? 3 : 11 - m_brightness;
    pruData->numBlocks = rowLen / 16;
    pruData->numRows = numRows;
    pruData->cmd = PWM_COMMAND_REGISTERS;
    __asm__ __volatile__("" ::
                             : "memory");
}

uint32_t BBShiftPanelOutput::computeMaxBrightnessCycles() {
    // Scale the row length by the amount of data actually shifted per pixel
    // clock: 16 output slots shift twice the bytes of 8 slots, so the same
    // physical row takes twice as long to shift out
    uint32_t effLen = rowLen * m_numOutputSlots / 8;
    // Longer rows take longer to shift, so they get a longer maximum on-time
    // to keep the duty cycle (brightness) up at the cost of refresh rate.
    // This is a smooth ramp so that adding one panel cannot step the
    // brightness; it passes through 0x8800 at 256, 0xA800 at 384, 0xC800 at
    // 512 and 0xE800 at 640 (the values the old stepped defaults used).
    uint32_t maxBright = 64 * effLen + 18432;
    maxBright = std::clamp(maxBright, 0x8800u, 0xE800u);
    if (FileExists(FPP_DIR_MEDIA("/config/panel_timing.txt"))) {
        std::string v = GetFileContents(FPP_DIR_MEDIA("/config/panel_timing.txt"));
        if (!v.empty()) {
            maxBright = std::stoi(v, nullptr, 16);
        }
    }
    return maxBright;
}

void BBShiftPanelOutput::buildStrideSchedule() {
    m_strideSchedule.clear();
    m_dupCopies.clear();

    uint32_t maxBright = computeMaxBrightnessCycles();
    // Approximate PRU cycles to shift one full stride out to the panels
    // (per-pixel cost including the amortized ring load overhead)
    uint32_t cyclesPerPixel = (m_numOutputSlots == 16) ? 48 : 26;
    uint32_t shiftCycles = rowLen * cyclesPerPixel + 100;

    // Capacity limit: the PRU brightness table holds 384 stride entries
    uint32_t bytesPerPixel = (m_numOutputSlots * 6 * 2) / 16;
    uint32_t strideLen = rowLen * bytesPerPixel;
    int maxSlots = 384 / (int)numRows;
    if (maxSlots < m_colorDepth) {
        // no room for splitting, the base schedule has to fit regardless
        maxSlots = m_colorDepth;
    }

    // Decide how many pulses each bit is displayed as.  A bit whose on-time
    // is >= k * shift time can be shown as k shorter pulses spread across the
    // frame at zero cost in frame time (the shifting still fits under each
    // pulse), which multiplies the perceived refresh rate.  The count rounds
    // to nearest: pieces slightly shorter than the shift time cost a little
    // dead time (bounded by half a shift per bit) but move that bit's light
    // to a much higher frequency - on longer chains the second MSB would
    // otherwise sit unsplit at the frame rate carrying a quarter of the light.
    int budget = maxSlots - m_colorDepth;
    std::vector<int> pieces(m_colorDepth, 1);
    for (int b = m_colorDepth - 1; b >= 0 && budget > 0; b--) {
        uint32_t on = (m_brightness * maxBright / 10) >> (m_colorDepth - 1 - b);
        int k = std::min((int)((on + shiftCycles / 2) / shiftCycles), 4);
        k = std::min(k, budget + 1);
        if (k <= 1) {
            // lower bits have shorter on-times, no more splitting is useful
            break;
        }
        pieces[b] = k;
        budget -= k - 1;
    }

    // Place the pieces on the timeline.  Each bit's pieces are spread evenly
    // across the frame with a half-spacing phase offset so that every pixel
    // value, not just full white, sees its light distributed across the whole
    // frame (e.g. 4 pieces land at 1/8, 3/8, 5/8, 7/8 and interleave with the
    // 2-piece bit at 1/4, 3/4).  Bits place MSB first; a collision moves to
    // the nearest free slot.
    int n = 0;
    for (int b = 0; b < m_colorDepth; b++) {
        n += pieces[b];
    }
    m_strideSchedule.assign(n, StrideSlot{ 0, false, 0 });
    std::vector<bool> used(n, false);
    for (int b = m_colorDepth - 1; b >= 0; b--) {
        uint32_t on = (m_brightness * maxBright / 10) >> (m_colorDepth - 1 - b);
        uint32_t per = on / pieces[b];
        for (int i = 0; i < pieces[b]; i++) {
            int target = (int)(((i + 0.5f) * n) / pieces[b]);
            int slot = target;
            for (int d = 0; d < n; d++) {
                // search outward from the target, alternating below/above
                int cand = (target + ((d & 1) ? (n - (d + 1) / 2) : (d / 2))) % n;
                if (!used[cand]) {
                    slot = cand;
                    break;
                }
            }
            used[slot] = true;
            // keep the total on-time exact, first piece takes the remainder
            uint32_t t = (i == 0) ? (on - per * (pieces[b] - 1)) : per;
            m_strideSchedule[slot] = { (uint8_t)b, false, t };
        }
    }
    bool seen[16] = { false };
    for (auto& slot : m_strideSchedule) {
        slot.primary = !seen[slot.bit];
        seen[slot.bit] = true;
    }

    // Frame buffer copies needed for the non-primary (duplicated) strides
    m_dupCopies.assign(numRows, {});
    int primarySlot[16];
    for (int s = 0; s < n; s++) {
        if (m_strideSchedule[s].primary) {
            primarySlot[m_strideSchedule[s].bit] = s;
        }
    }
    for (int s = 0; s < n; s++) {
        if (m_strideSchedule[s].primary) {
            continue;
        }
        int ps = primarySlot[m_strideSchedule[s].bit];
        for (int r = 0; r < numRows; r++) {
            uint32_t dstIdx = m_outputByRow ? (r * n + s) : (s * numRows + r);
            uint32_t srcIdx = m_outputByRow ? (r * n + ps) : (ps * numRows + r);
            m_dupCopies[r].emplace_back(dstIdx * strideLen, srcIdx * strideLen);
        }
    }

    // log what this schedule works out to
    uint64_t rowCycles = 0;
    uint64_t onCycles = 0;
    for (auto& slot : m_strideSchedule) {
        rowCycles += std::max(shiftCycles, slot.onTime) + 60;
        onCycles += slot.onTime;
    }
    uint64_t frameCycles = rowCycles * numRows;
    float refresh = 250000000.0f / (float)frameCycles;
    LogInfo(VB_CHANNELOUT, "BBShiftPanel: %d strides/row (%d bit color, MSB split %d ways), est refresh %d Hz, duty %d%%\n",
            n, m_colorDepth, pieces[m_colorDepth - 1], (int)refresh, (int)(onCycles * 100 / rowCycles));
}

void BBShiftPanelOutput::setupBrightnessValues() {
    uint32_t* cur = &pruData->brightness[0];
    int n = m_strideSchedule.size();
    auto writeEntry = [&](int s, int r) {
        int mappedRow = mapRow(r, m_addressingMode);
        cur[0] = m_strideSchedule[s].onTime;
        cur[1] = (mappedRow << 24) & 0x7F000000;
        if (m_outputByRow && m_outputBlankData && (s == 0)) {
            cur[1] |= 0x80000000;
        }
        // printf("Brightness[%d %d] = %08x  %08x\n", s, r, cur[0], cur[1]);
        cur += 2;
    };
    if (m_outputByRow) {
        for (int r = 0; r < numRows; r++) {
            for (int s = 0; s < n; s++) {
                writeEntry(s, r);
            }
        }
    } else {
        for (int s = 0; s < n; s++) {
            for (int r = 0; r < numRows; r++) {
                writeEntry(s, r);
            }
        }
    }
}

bool BBShiftPanelOutput::setupChannelOffsets() {
    PanelInterleaveHandler* handler = PanelInterleaveHandler::createHandler(m_panelInterleave, m_panelWidth, m_panelHeight, m_panelScan);
    if (!handler) {
        LogErr(VB_CHANNELOUT, "Failed to create panel interleave handler\n");
        return false;
    }
    numRows = 0;
    rowLen = 0;
    int maxRowLen = 0;
    for (int output = 0; output < m_numOutputs; output++) {
        int panelsOnOutput = m_panelMatrix->m_outputPanels[output].size();

        for (int i = 0; i < panelsOnOutput; i++) {
            for (int y = 0; y < (m_panelHeight / 2); y++) {
                for (int x = 0; x < m_panelWidth; ++x) {
                    int yOut = y;
                    int xOut = x;
                    handler->map(xOut, yOut);
                    if (yOut >= numRows) {
                        numRows = yOut + 1;
                    }
                    if (xOut >= maxRowLen) {
                        maxRowLen = xOut + 1;
                    }
                }
            }
        }
    }
    rowLen = maxRowLen * m_longestChain;
    channelOffsets = new uint32_t[m_channelCount];
    memset(channelOffsets, 0xFF, m_channelCount * sizeof(uint32_t));

    int pixelStride = m_numOutputSlots * 6;
    int totalRowLen = rowLen * pixelStride;
    for (int output = 0; output < m_numOutputs; output++) {
        int panelsOnOutput = m_panelMatrix->m_outputPanels[output].size();
        // For 16 outputs, bank1 data starts at offset 48 (= 8 slots * 6 colors)
        // so that ISPC's 16-lane reduce_add packs bank0/bank1 pairs correctly:
        // bank0 channels occupy slots 0-47, bank1 channels occupy slots 48-95
        int outputIdx = outputPin[output] + outputBank[output] * 48;

        for (int i = 0; i < panelsOnOutput; i++) {
            int panel = m_panelMatrix->m_outputPanels[output][i];
            int c = m_panelMatrix->m_panels[panel].chain;
            int chain = m_longestChain - c - 1;
            int xOff = chain * maxRowLen;

            for (int y = 0; y < (m_panelHeight / 2); y++) {
                int yw1 = y * m_panelWidth * 3;
                int yw2 = (y + (m_panelHeight / 2)) * m_panelWidth * 3;

                for (int x = 0; x < m_panelWidth; ++x) {
                    uint32_t r1 = m_panelMatrix->m_panels[panel].pixelMap[yw1 + x * 3];
                    uint32_t g1 = m_panelMatrix->m_panels[panel].pixelMap[yw1 + x * 3 + 1];
                    uint32_t b1 = m_panelMatrix->m_panels[panel].pixelMap[yw1 + x * 3 + 2];

                    uint32_t r2 = m_panelMatrix->m_panels[panel].pixelMap[yw2 + x * 3];
                    uint32_t g2 = m_panelMatrix->m_panels[panel].pixelMap[yw2 + x * 3 + 1];
                    uint32_t b2 = m_panelMatrix->m_panels[panel].pixelMap[yw2 + x * 3 + 2];
                    int yOut = y;
                    int xOut = x;
                    handler->map(xOut, yOut);
                    xOut += xOff;

                    if (isPWMPanel()) {
                        // For PWM panels, the first of each group of 16 pixels is out first,
                        // then the second of each group of 16, etc...
                        int xo2 = xOut % 16;
                        int xo3 = xOut / 16;

                        xOut = xo2 * (rowLen / 16) + xo3;
                    }
                    // Color stride is always 8: within each bank, 8 slots per color channel.
                    // For 8 outputs: outputIdx=pin(0-7), channels at +0,+8,+16,+24,+32,+40
                    // For 16 outputs bank0: same; bank1: outputIdx=pin+48, same offsets
                    channelOffsets[r1] = yOut * totalRowLen + xOut * pixelStride + outputIdx;
                    channelOffsets[g1] = yOut * totalRowLen + xOut * pixelStride + outputIdx + 8;
                    channelOffsets[b1] = yOut * totalRowLen + xOut * pixelStride + outputIdx + 16;
                    channelOffsets[r2] = yOut * totalRowLen + xOut * pixelStride + outputIdx + 24;
                    channelOffsets[g2] = yOut * totalRowLen + xOut * pixelStride + outputIdx + 32;
                    channelOffsets[b2] = yOut * totalRowLen + xOut * pixelStride + outputIdx + 40;
                }
            }
        }
    }
    delete handler;

    /*
    for (int x = 0; x < rowLen * 3; x++) {
        printf("%06x ", channelOffsets[x]);
        if ((x % 3) == 2) {
            printf("  ");
        }
        if ((x % 48) == 47) {
            printf("\n");
        }
    }
    printf("\n");
    int offset = 32 * rowLen * 3;
    for (int x = 0; x < rowLen * 3; x++) {
        printf("%06x ", channelOffsets[x + offset]);
        if ((x % 3) == 2) {
            printf("  ");
        }
        if ((x % 48) == 47) {
            printf("\n");
        }
    }
    */
    // Channels not covered by any panel still have the 0xFFFFFFFF fill from
    // above (partially invalid layouts, unused canvas regions, panels beyond
    // the licensed output count).  Point them at a scratch slot just past
    // the end of the data so the prep loop needs no bounds check; writing
    // through the marker used to scatter to wild addresses and crash.
    uint32_t dataSize = rowLen * m_numOutputSlots * 6 * numRows;
    uint32_t unmapped = 0;
    for (int x = 0; x < m_channelCount; x++) {
        if (channelOffsets[x] == 0xFFFFFFFF) {
            channelOffsets[x] = dataSize;
            ++unmapped;
        }
    }
    if (unmapped) {
        LogWarn(VB_CHANNELOUT, "BBShiftPanel: %u of %u channels are not mapped to any panel; check the panel layout\n",
                unmapped, m_channelCount);
    }
    currentChannelData = new uint16_t[dataSize + 1];
    memset(currentChannelData, 0, (dataSize + 1) * sizeof(uint16_t));
    return true;
}

void BBShiftPanelOutput::setupGamma(float gamma) {
    if (gamma < 0.01 || gamma > 50.0) {
        gamma = 2.2;
    }

    int colorDepth = m_colorDepth;
    if (isPWMPanel()) {
        // we are outputting 16 bit data as that's what the
        // PWM registers require, but only the bottom 12 bits are used
        colorDepth = 12;
    }

    for (int x = 0; x < 256; x++) {
        int v = x;
        if (colorDepth == 6 && (v == 3 || v == 2)) {
            v = 4;
        } else if (colorDepth == 7 && v == 1) {
            v = 2;
        }
        float max = 255.0f;
        switch (colorDepth) {
        case 16:
            max = 65535.0f;
            break;
        case 15:
            max = 32767.0f;
            break;
        case 14:
            max = 16383.0f;
            break;
        case 13:
            max = 8191.0f;
            break;
        case 12:
            max = 4095.0f;
            break;
        case 11:
            max = 2047.0f;
            break;
        case 10:
            max = 1023.0f;
            break;
        case 9:
            max = 511.0f;
            break;
        }
        float f = v;
        f = max * std::pow(f / 255.0f, gamma);
        if (f > max) {
            f = max;
        }
        if (f < 0.0) {
            f = 0.0;
        }
        gammaCurve[x] = std::round(f);
        if (gammaCurve[x] == 0 && f > 0.25) {
            // don't drop as much of the low end to 0
            gammaCurve[x] = 1;
        }
    }
    /*
    for (int x = 0; x < 256; x++) {
        printf("%d: %04x\n", x, gammaCurve[x]);
    }
    */
}

void BBShiftPanelOutput::OverlayTestData(unsigned char* channelData, int cycleNum, float percentOfCycle, int testType, const Json::Value& config) {
    for (int output = 0; output < m_numOutputs; output++) {
        int panelsOnOutput = m_panelMatrix->m_outputPanels[output].size();
        for (int i = 0; i < panelsOnOutput; i++) {
            int panel = m_panelMatrix->m_outputPanels[output][i];

            m_panelMatrix->m_panels[panel].drawTestPattern(channelData + m_startChannel, cycleNum, percentOfCycle, testType);
        }
    }
}

void BBShiftPanelOutput::GetRequiredChannelRanges(const std::function<void(int, int)>& addRange) {
    addRange(m_startChannel, m_startChannel + m_channelCount - 1);
}

void BBShiftPanelOutput::DumpConfig(void) {
    LogDebug(VB_CHANNELOUT, "BBShiftPanelOutput::DumpConfig()\n");
    LogDebug(VB_CHANNELOUT, "BBBMatrix::DumpConfig()\n");
    LogDebug(VB_CHANNELOUT, "    Width          : %d\n", m_width);
    LogDebug(VB_CHANNELOUT, "    Height         : %d\n", m_height);
    LogDebug(VB_CHANNELOUT, "    Panel Width    : %d\n", m_panelWidth);
    LogDebug(VB_CHANNELOUT, "    Panel Height   : %d\n", m_panelHeight);
    LogDebug(VB_CHANNELOUT, "    Color Depth    : %d\n", m_colorDepth);
    LogDebug(VB_CHANNELOUT, "    Longest Chain  : %d\n", m_longestChain);
    LogDebug(VB_CHANNELOUT, "    Inverted Data  : %d\n", m_invertedData);
    LogDebug(VB_CHANNELOUT, "    Output Rows    : %d\n", numRows);
    LogDebug(VB_CHANNELOUT, "    Output Length  : %d\n", rowLen);
    LogDebug(VB_CHANNELOUT, "    Num Outputs    : %d (%d slots)\n", m_numOutputs, m_numOutputSlots);
    LogDebug(VB_CHANNELOUT, "    Addressing Mode: %d %s\n", m_addressingMode, isPWMPanel() ? "PWM" : "Shift");
    if (!m_strideSchedule.empty()) {
        std::string sched;
        for (auto& slot : m_strideSchedule) {
            sched += " " + std::to_string(slot.bit) + (slot.primary ? "" : "*") + "/" + std::to_string(slot.onTime);
        }
        LogDebug(VB_CHANNELOUT, "    Stride Schedule:%s\n", sched.c_str());
    }

    ChannelOutput::DumpConfig();
}
