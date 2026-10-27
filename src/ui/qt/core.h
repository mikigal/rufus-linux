/*
 * Rufus: The Reliable USB Formatting Utility
 * The headers of librufus-core, for the Qt user interface
 * Copyright © 2026 Rufus for Linux contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * The core is C. Its headers define many macros (BOOL, TRUE, MB, IDOK...), so
 * this must be included after the Qt headers.
 */

#pragma once

// C++23: _Atomic(T) is std::atomic<T>, which has the same layout
#include <stdatomic.h>

extern "C" {
#include "rufus.h"
#include "options.h"
#include "ui_iface.h"
#include "rules.h"
#include "devices.h"
#include "image_scan.h"
#include "boot_check.h"
#include "download.h"
#include "progress.h"
#include "platform.h"
#include "helper_client.h"
#include "localization.h"
#include "resource.h"
#include "settings.h"
}

#include <QString>

/* lmprintf() as a QString */
template <typename... Args> static inline QString lmstr(uint32_t msg_id, Args... args)
{
	return QString::fromUtf8(lmprintf(msg_id, args...));
}
