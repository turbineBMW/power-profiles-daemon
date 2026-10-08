/*
 * Copyright (c) 2026 Daaboulex
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

#pragma once

#include "ppd-driver-cpu.h"

#define PPD_TYPE_DRIVER_CPUFREQ (ppd_driver_cpufreq_get_type ())
G_DECLARE_FINAL_TYPE (PpdDriverCpufreq, ppd_driver_cpufreq, PPD, DRIVER_CPUFREQ, PpdDriverCpu)
