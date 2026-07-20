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

#include "../log.h"

#include "RGBCycle.h"

/*
 *
 */
TestPatternRGBCycle::TestPatternRGBCycle() :
    m_colorPatternStr(""),
    m_colorPatternSize(0),
    m_patternOffset(0),
    m_channelsPerNode(3) {
    LogExcess(VB_CHANNELOUT, "TestPatternRGBCycle::TestPatternRGBCycle()\n");

    m_testPatternName = "RGBCycle";
}

/*
 *
 */
TestPatternRGBCycle::~TestPatternRGBCycle() {
    LogExcess(VB_CHANNELOUT, "TestPatternRGBCycle::~TestPatternRGBCycle()\n");
}

/*
 *
 */
int TestPatternRGBCycle::Init(Json::Value config) {
    m_configChanged = 0;

    if (m_colorPatternStr != config["colorPattern"].asString()) {
        m_colorPatternStr = config["colorPattern"].asString();
        m_configChanged = 1;
    }

    int cpn = config.isMember("channelsPerNode") ? config["channelsPerNode"].asInt() : 3;
    if (cpn != 3 && cpn != 4) {
        cpn = 3;
    }
    if (m_channelsPerNode != cpn) {
        m_channelsPerNode = cpn;
        m_configChanged = 1;
    }

    return TestPatternBase::Init(config);
}

/*
 *
 */
int TestPatternRGBCycle::SetupTest(void) {
    bzero(m_testData, m_channelCount);

    m_colorPattern.clear();

    char digit = 0;
    for (int i = 0; i < m_colorPatternStr.size(); i += 2) {
        digit = (char)strtol(m_colorPatternStr.substr(i, 2).c_str(), NULL, 16);
        m_colorPattern.push_back(digit);
    }

    // Make sure we have a valid set of color groups (3 channels for RGB, 4 for RGBW)
    const int stride = m_channelsPerNode;
    while (m_colorPattern.size() < stride || m_colorPattern.size() % stride) {
        m_colorPattern.push_back(0);
    }

    char* c = m_testData;
    for (int i = 0; i + stride <= m_channelCount; i += stride) {
        for (int j = 0; j < stride; j++)
            *(c++) = m_colorPattern[j];
    }

    m_patternOffset = 0;
    m_colorPatternSize = m_colorPattern.size() / stride;

    return TestPatternBase::SetupTest();
}

/*
 *
 */
void TestPatternRGBCycle::CycleData(void) {
    const int stride = m_channelsPerNode;
    m_patternOffset += stride;
    if (m_patternOffset >= m_colorPattern.size()) {
        m_patternOffset = 0;
    }
    char* c = m_testData;
    for (int i = 0; i + stride <= m_channelCount; i += stride) {
        for (int j = 0; j < stride; j++)
            *(c++) = m_colorPattern[m_patternOffset + j];
    }
}

/*
 *
 */
void TestPatternRGBCycle::DumpConfig(void) {
    LogDebug(VB_CHANNELOUT, "TestPatternRGBCycle::DumpConfig\n");
    LogDebug(VB_CHANNELOUT, "    colorPattern    : %s\n", m_colorPatternStr.c_str());
    LogDebug(VB_CHANNELOUT, "    colorPatternSize: %d\n", m_colorPatternSize);

    TestPatternBase::DumpConfig();
}
