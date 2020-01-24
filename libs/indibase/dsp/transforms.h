/*******************************************************************************
  Copyright(c) 2017 Jasem Mutlaq. All rights reserved.

 Connection Plugin Interface

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Library General Public
 License version 2 as published by the Free Software Foundation.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Library General Public License for more details.

 You should have received a copy of the GNU Library General Public License
 along with this library; see the file COPYING.LIB.  If not, write to
 the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 Boston, MA 02110-1301, USA.
*******************************************************************************/

#pragma once

#include "dspinterface.h"
#include "dsp.h"

#include <string>

namespace DSP
{
class Transforms : public Interface
{
public:
    Transforms(INDI::DefaultDevice *dev);
    virtual ~Transforms();

protected:
    uint8_t *Callback(uint8_t *out, long dims, long *sizes, int bits_per_sample);

private:
    void Histogram(int histogram_size);
    void FourierTransform();
};
}
