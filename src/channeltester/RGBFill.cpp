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

#include "RGBFill.h"

/*
 *
 */
TestPatternRGBFill::TestPatternRGBFill() :
    m_color1(0),
    m_color2(0),
    m_color3(0),
    m_color4(0) {
    LogExcess(VB_CHANNELOUT, "TestPatternRGBFill::TestPatternRGBFill()\n");

    m_testPatternName = "RGBFill";
}

/*
 *
 */
TestPatternRGBFill::~TestPatternRGBFill() {
    LogExcess(VB_CHANNELOUT, "TestPatternRGBFill::~TestPatternRGBFill()\n");
}

/*
 *
 */
int TestPatternRGBFill::Init(Json::Value config) {
    m_configChanged = 0;

    if (m_color1 != config["color1"].asInt()) {
        m_color1 = config["color1"].asInt();
        m_configChanged = 1;
    }

    if (m_color2 != config["color2"].asInt()) {
        m_color2 = config["color2"].asInt();
        m_configChanged = 1;
    }

    if (m_color3 != config["color3"].asInt()) {
        m_color3 = config["color3"].asInt();
        m_configChanged = 1;
    }

    if (m_color4 != config["color4"].asInt()) {
        m_color4 = config["color4"].asInt();
        m_configChanged = 1;
    }

    return TestPatternBase::Init(config);
}

/*
 *
 */
int TestPatternRGBFill::SetupTest(void) {
    bzero(m_testData, m_channelCount);

    char* c = m_testData;
    int offset = 0;
    
    // Check if we have a 4th color (White channel for RGBW)
    if (m_color4 != 0) {
        // RGBW mode - fill with 4 channels per pixel
        for (int i = 0; i < m_channelCount; i += 4) {
            *(c++) = m_color1;
            *(c++) = m_color2;
            *(c++) = m_color3;
            *(c++) = m_color4;
        }
    } else {
        // RGB mode - fill with 3 channels per pixel
        for (int i = 0; i < m_channelCount; i += 3) {
            *(c++) = m_color1;
            *(c++) = m_color2;
            *(c++) = m_color3;
        }
    }

    return TestPatternBase::SetupTest();
}

/*
 *
 */
void TestPatternRGBFill::DumpConfig(void) {
    LogDebug(VB_CHANNELOUT, "TestPatternRGBFill::DumpConfig\n");
    LogDebug(VB_CHANNELOUT, "    color1 : %02x\n", m_color1);
    LogDebug(VB_CHANNELOUT, "    color2 : %02x\n", m_color2);
    LogDebug(VB_CHANNELOUT, "    color3 : %02x\n", m_color3);
    LogDebug(VB_CHANNELOUT, "    color4 : %02x\n", m_color4);

    TestPatternBase::DumpConfig();
}
