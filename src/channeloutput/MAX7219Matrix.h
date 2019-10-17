/*
 *   MAX7219 Matrix Channel Output driver for Falcon Player (FPP)
 *
 *   Copyright (C) 2013-2018 the Falcon Player Developers
 *      Initial development by:
 *      - David Pitts (dpitts)
 *      - Tony Mace (MyKroFt)
 *      - Mathew Mrosko (Materdaddy)
 *      - Chris Pinkham (CaptainMurdoch)
 *      For additional credits and developers, see credits.php.
 *
 *   The Falcon Player (FPP) is free software; you can redistribute it
 *   and/or modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _MAX7219MATRIX_H
#define _MAX7219MATRIX_H

#include "ChannelOutputBase.h"

#include "util/GPIOUtils.h"
#include "util/SPIUtils.h"

class MAX7219MatrixOutput : public ChannelOutputBase {
  public:
	MAX7219MatrixOutput(unsigned int startChannel, unsigned int channelCount);
	~MAX7219MatrixOutput();

	int Init(Json::Value config);
	int Close(void);

	int SendData(unsigned char *channelData);

	void DumpConfig(void);

    virtual void GetRequiredChannelRanges(const std::function<void(int, int)> &addRange);

  private:
	void WriteCommand(uint8_t cmd, uint8_t value);

	int m_panels;
	int m_pinCS;
    
    const PinCapabilities *m_csPin;
    SPIUtils *m_spi;
};

#endif
