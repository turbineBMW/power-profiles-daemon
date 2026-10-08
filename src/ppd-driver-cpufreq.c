/*
 * Copyright (c) 2026 Daaboulex
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

/*
 * Generic cpufreq CPU driver, for cpufreq scaling drivers that have no
 * energy performance preference interface. It only uses the global boost
 * switch, see "Frequency Boost Support" in
 * https://docs.kernel.org/admin-guide/pm/cpufreq.html
 *
 * It is only used when every cpufreq policy runs a scaling driver listed in
 * boost_scaling_drivers, where turning boost off is known to cap the CPU at
 * its sustainable frequency. Any other system keeps its current driver.
 *
 * There is no power_changed handler: the boost switch has no setting that
 * depends on the power source, so the selected profile alone decides it,
 * on battery and on AC alike.
 */

#define G_LOG_DOMAIN "CpuDriver"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <upower.h>

#include "ppd-utils.h"
#include "ppd-driver-cpufreq.h"

#define CPUFREQ_POLICY_DIR "/sys/devices/system/cpu/cpufreq/"
#define CPUFREQ_BOOST_PATH "/sys/devices/system/cpu/cpufreq/boost"

static const char * const boost_scaling_drivers[] = {
  "apple-cpufreq",
  NULL
};

struct _PpdDriverCpufreq
{
  PpdDriverCpu  parent_instance;

  PpdProfile activated_profile;
  char *boost_path;
};

G_DEFINE_TYPE (PpdDriverCpufreq, ppd_driver_cpufreq, PPD_TYPE_DRIVER_CPU)

static GObject*
ppd_driver_cpufreq_constructor (GType                  type,
                                guint                  n_construct_params,
                                GObjectConstructParam *construct_params)
{
  GObject *object;

  object = G_OBJECT_CLASS (ppd_driver_cpufreq_parent_class)->constructor (type,
                                                                          n_construct_params,
                                                                          construct_params);
  g_object_set (object,
                "driver-name", "cpufreq",
                "profiles", PPD_PROFILE_PERFORMANCE | PPD_PROFILE_BALANCED | PPD_PROFILE_POWER_SAVER,
                NULL);

  return object;
}

static gboolean
scaling_drivers_use_boost (void)
{
  g_autoptr(GDir) dir = NULL;
  g_autofree char *policy_dir = NULL;
  const char *dirname;
  guint policies = 0;

  policy_dir = ppd_utils_get_sysfs_path (CPUFREQ_POLICY_DIR);
  dir = g_dir_open (policy_dir, 0, NULL);
  if (!dir) {
    g_debug ("Could not open %s", policy_dir);
    return FALSE;
  }

  while ((dirname = g_dir_read_name (dir)) != NULL) {
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autoptr(GError) error = NULL;

    if (!g_str_has_prefix (dirname, "policy"))
      continue;

    path = g_build_filename (policy_dir,
                             dirname,
                             "scaling_driver",
                             NULL);
    if (!g_file_get_contents (path, &contents, NULL, &error)) {
      g_debug ("Failed to read %s: %s", path, error->message);
      continue;
    }

    if (!g_strv_contains (boost_scaling_drivers, g_strchomp (contents))) {
      g_debug ("%s uses scaling driver %s, which this driver does not handle",
               dirname, contents);
      return FALSE;
    }

    policies++;
  }

  return policies > 0;
}

static PpdProbeResult
ppd_driver_cpufreq_probe (PpdDriver *driver)
{
  PpdDriverCpufreq *cpufreq = PPD_DRIVER_CPUFREQ (driver);
  int fd;

  if (!scaling_drivers_use_boost ())
    return PPD_PROBE_RESULT_FAIL;

  cpufreq->boost_path = ppd_utils_get_sysfs_path (CPUFREQ_BOOST_PATH);
  fd = g_open (cpufreq->boost_path, O_WRONLY);
  if (fd == -1) {
    g_debug ("Could not open the cpufreq boost switch %s for writing: %s",
             cpufreq->boost_path, g_strerror (errno));
    return PPD_PROBE_RESULT_FAIL;
  }
  g_close (fd, NULL);

  g_debug ("Found cpufreq boost switch at %s", cpufreq->boost_path);
  return PPD_PROBE_RESULT_SUCCESS;
}

static const char *
profile_to_boost (PpdProfile profile)
{
  switch (profile) {
  case PPD_PROFILE_POWER_SAVER:
    return "0";
  case PPD_PROFILE_BALANCED:
  case PPD_PROFILE_PERFORMANCE:
    return "1";
  }

  g_return_val_if_reached (NULL);
}

static gboolean
ppd_driver_cpufreq_activate_profile (PpdDriver                   *driver,
                                     PpdProfile                   profile,
                                     PpdProfileActivationReason   reason,
                                     GError                     **error)
{
  PpdDriverCpufreq *cpufreq = PPD_DRIVER_CPUFREQ (driver);

  g_return_val_if_fail (cpufreq->boost_path != NULL, FALSE);

  if (profile == PPD_PROFILE_UNSET)
    return TRUE;

  if (!ppd_utils_write (cpufreq->boost_path, profile_to_boost (profile), error)) {
    if (cpufreq->activated_profile != PPD_PROFILE_UNSET) {
      g_autoptr(GError) error_local = NULL;

      if (!ppd_utils_write (cpufreq->boost_path,
                            profile_to_boost (cpufreq->activated_profile),
                            &error_local))
        g_warning ("failed to restore previous profile: %s", error_local->message);
    }
    return FALSE;
  }

  cpufreq->activated_profile = profile;
  return TRUE;
}

static void
ppd_driver_cpufreq_finalize (GObject *object)
{
  PpdDriverCpufreq *cpufreq = PPD_DRIVER_CPUFREQ (object);

  g_clear_pointer (&cpufreq->boost_path, g_free);
  G_OBJECT_CLASS (ppd_driver_cpufreq_parent_class)->finalize (object);
}

static void
ppd_driver_cpufreq_class_init (PpdDriverCpufreqClass *klass)
{
  GObjectClass *object_class;
  PpdDriverClass *driver_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->constructor = ppd_driver_cpufreq_constructor;
  object_class->finalize = ppd_driver_cpufreq_finalize;

  driver_class = PPD_DRIVER_CLASS (klass);
  driver_class->probe = ppd_driver_cpufreq_probe;
  driver_class->activate_profile = ppd_driver_cpufreq_activate_profile;
}

static void
ppd_driver_cpufreq_init (PpdDriverCpufreq *self)
{
}
