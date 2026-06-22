/******************************************************************************
 * This file is part of dirtsand.                                             *
 *                                                                            *
 * dirtsand is free software: you can redistribute it and/or modify           *
 * it under the terms of the GNU Affero General Public License as             *
 * published by the Free Software Foundation, either version 3 of the         *
 * License, or (at your option) any later version.                            *
 *                                                                            *
 * dirtsand is distributed in the hope that it will be useful,                *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU Affero General Public License for more details.                        *
 *                                                                            *
 * You should have received a copy of the GNU Affero General Public License   *
 * along with dirtsand.  If not, see <http://www.gnu.org/licenses/>.          *
 ******************************************************************************/

#include "UnifiedTime.h"
#ifdef _WIN32
#   include <chrono>
#else
#   include <sys/time.h>
#endif

void DS::UnifiedTime::read(DS::Stream* stream)
{
    m_secs = stream->read<uint32_t>();
    m_micros = stream->read<uint32_t>();
}

void DS::UnifiedTime::write(DS::Stream* stream) const
{
    stream->write<uint32_t>(m_secs);
    stream->write<uint32_t>(m_micros);
}

void DS::UnifiedTime::setNow()
{
#ifdef _WIN32
    auto tp = std::chrono::system_clock::now().time_since_epoch();
    m_secs   = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::seconds>(tp).count());
    m_micros = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(tp).count() % 1000000);
#else
    timeval now;
    gettimeofday(&now, nullptr);
    m_secs = now.tv_sec;
    m_micros = now.tv_usec;
#endif
}
