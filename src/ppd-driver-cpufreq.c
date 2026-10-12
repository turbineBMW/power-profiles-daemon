/*
 * Copyright (c) 2026 Daaboulex
 * Copyright (c) 2026 Brandon Williams <turbine.bmw@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3 as published by
 * the Free Software Foundation.
 *
 */

/*
 * Generic cpufreq CPU driver, for cpufreq scaling drivers that have no
 * energy performance preference interface. Each supported scaling driver
 * uses one of two methods:
 *
 * - boost: the global boost switch, see "Frequency Boost Support" in
 *   https://docs.kernel.org/admin-guide/pm/cpufreq.html
 *   Used where turning boost off is known to cap the CPU at its sustainable
 *   frequency.
 *
 * - caps: each policy's scaling_max_freq, capped at a share of the policy's
 *   highest non-boost frequency, the way Windows' processor power management
 *   caps maximum performance per power mode. Used where boost is off by
 *   default or absent, so it can't tell the profiles apart. The boost switch
 *   is left alone: uncapped profiles allow the boost frequencies, so whether
 *   they are used stays up to the switch.
 *
 * It is only used when every cpufreq policy runs one of the scaling drivers
 * in the table below. Any other system keeps its current driver.
 *
 * The boost method follows the selected profile alone, on battery and on AC
 * alike. With the caps method, balanced is only capped on battery, so
 * machines without one, such as single-board computers and servers, keep
 * their full speed by default.
 */

#define G_LOG_DOMAIN "CpuDriver"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include <upower.h>

#include "ppd-utils.h"
#include "ppd-driver-cpufreq.h"

#define CPUFREQ_POLICY_DIR "/sys/devices/system/cpu/cpufreq/"
#define CPUFREQ_BOOST_PATH "/sys/devices/system/cpu/cpufreq/boost"

/* Share of each policy's maximum frequency allowed per profile with the caps
 * method. Measured on a Snapdragon X Elite, 75% costs ~16% of sustained
 * throughput for ~39% less energy, and 60% ~37% for ~50% less. */
#define POWER_SAVER_PERCENT        60
#define BALANCED_BATTERY_PERCENT   75
#define UNCAPPED_PERCENT           100

typedef enum {
  CPUFREQ_METHOD_NONE,
  CPUFREQ_METHOD_BOOST,
  CPUFREQ_METHOD_CAPS,
} CpufreqMethod;

static const struct {
  const char *scaling_driver;
  CpufreqMethod method;
} scaling_drivers[] = {
  { "apple-cpufreq", CPUFREQ_METHOD_BOOST },
  /* Arm SCMI and Qualcomm firmware-managed DVFS, and generic OPP tables,
   * as on the Snapdragon X series */
  { "scmi", CPUFREQ_METHOD_CAPS },
  { "qcom-cpufreq-hw", CPUFREQ_METHOD_CAPS },
  { "cpufreq-dt", CPUFREQ_METHOD_CAPS },
};

typedef struct {
  char *dir;
  guint64 max_freq; /* highest non-boost frequency, the base for the caps */
  guint64 top_freq; /* highest frequency including boost, for no cap */
  guint64 *freqs; /* ascending non-boost frequencies */
  guint n_freqs;
} Policy;

struct _PpdDriverCpufreq
{
  PpdDriverCpu  parent_instance;

  PpdProfile activated_profile;
  CpufreqMethod method;
  char *boost_path;
  GPtrArray *policies; /* of Policy, with the caps method */
  gboolean on_battery;
};

G_DEFINE_TYPE (PpdDriverCpufreq, ppd_driver_cpufreq, PPD_TYPE_DRIVER_CPU)

static void
policy_free (Policy *policy)
{
  g_free (policy->dir);
  g_free (policy->freqs);
  g_free (policy);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Policy, policy_free)

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

static CpufreqMethod
scaling_driver_to_method (const char *scaling_driver)
{
  for (guint i = 0; i < G_N_ELEMENTS (scaling_drivers); i++) {
    if (g_str_equal (scaling_drivers[i].scaling_driver, scaling_driver))
      return scaling_drivers[i].method;
  }

  return CPUFREQ_METHOD_NONE;
}

static char *
read_attr (const char *dir,
           const char *attr)
{
  g_autofree char *path = NULL;
  g_autofree char *contents = NULL;

  path = g_build_filename (dir, attr, NULL);
  if (!g_file_get_contents (path, &contents, NULL, NULL))
    return NULL;

  return g_strdup (g_strstrip (contents));
}

static gboolean
read_freq (const char *dir,
           const char *attr,
           guint64    *freq)
{
  g_autofree char *contents = read_attr (dir, attr);

  return contents &&
         g_ascii_string_to_unsigned (contents, 10, 1, G_MAXUINT64, freq, NULL);
}

static int
compare_freqs (gconstpointer a,
               gconstpointer b)
{
  guint64 fa = *(const guint64 *) a;
  guint64 fb = *(const guint64 *) b;

  return (fa > fb) - (fa < fb);
}

/* Parses a list of frequencies, skipping invalid ones, sorted ascending */
static guint64 *
read_freqs (const char *dir,
            const char *attr,
            guint      *n_freqs)
{
  g_autofree char *contents = NULL;
  g_auto(GStrv) tokens = NULL;
  guint64 *freqs;

  *n_freqs = 0;
  contents = read_attr (dir, attr);
  if (!contents)
    return NULL;

  tokens = g_strsplit_set (contents, " \n", -1);
  if (g_strv_length (tokens) == 0)
    return NULL;

  freqs = g_new0 (guint64, g_strv_length (tokens));
  for (guint i = 0; tokens[i] != NULL; i++) {
    guint64 freq;

    if (*tokens[i] == '\0')
      continue;
    if (!g_ascii_string_to_unsigned (tokens[i], 10, 1, G_MAXUINT64, &freq, NULL)) {
      g_debug ("%s/%s lists an invalid frequency '%s'", dir, attr, tokens[i]);
      continue;
    }
    freqs[(*n_freqs)++] = freq;
  }

  if (*n_freqs == 0) {
    g_free (freqs);
    return NULL;
  }
  qsort (freqs, *n_freqs, sizeof (guint64), compare_freqs);

  return freqs;
}

static Policy *
probe_policy (const char *dir)
{
  g_autofree char *epp_path = NULL;
  g_autofree guint64 *boost_freqs = NULL;
  g_autoptr(Policy) policy = NULL;
  guint n_boost_freqs;

  epp_path = g_build_filename (dir, "energy_performance_preference", NULL);
  if (g_file_test (epp_path, G_FILE_TEST_EXISTS)) {
    g_debug ("%s has an energy-performance preference", dir);
    return NULL;
  }

  policy = g_new0 (Policy, 1);
  policy->dir = g_strdup (dir);
  if (!read_freq (dir, "cpuinfo_max_freq", &policy->top_freq)) {
    g_debug ("%s has no valid cpuinfo_max_freq", dir);
    return NULL;
  }

  /* cpuinfo_max_freq includes boost frequencies once boost has been on, so
   * the caps are based on the highest frequency outside boost, from the
   * frequency table every supported scaling driver has */
  policy->freqs = read_freqs (dir, "scaling_available_frequencies", &policy->n_freqs);
  if (policy->n_freqs == 0) {
    g_debug ("%s has no scaling_available_frequencies", dir);
    return NULL;
  }
  policy->max_freq = policy->freqs[policy->n_freqs - 1];

  boost_freqs = read_freqs (dir, "scaling_boost_frequencies", &n_boost_freqs);
  if (n_boost_freqs)
    policy->top_freq = MAX (policy->top_freq, boost_freqs[n_boost_freqs - 1]);
  policy->top_freq = MAX (policy->top_freq, policy->max_freq);

  return g_steal_pointer (&policy);
}

/* Paths of the cpufreq policy directories */
static GPtrArray *
list_policy_dirs (void)
{
  g_autoptr(GDir) dir = NULL;
  g_autofree char *policy_dir = NULL;
  GPtrArray *dirs = g_ptr_array_new_with_free_func (g_free);
  const char *dirname;

  policy_dir = ppd_utils_get_sysfs_path (CPUFREQ_POLICY_DIR);
  dir = g_dir_open (policy_dir, 0, NULL);
  if (!dir) {
    g_debug ("Could not open %s", policy_dir);
    return dirs;
  }

  while ((dirname = g_dir_read_name (dir)) != NULL) {
    if (g_str_has_prefix (dirname, "policy"))
      g_ptr_array_add (dirs, g_build_filename (policy_dir, dirname, NULL));
  }

  return dirs;
}

/* Every readable policy must use the same supported scaling driver. A policy
 * whose CPUs are all offline can't be read and is skipped. Only one cpufreq
 * driver can be registered at a time, so the readable ones agree anyway. */
static CpufreqMethod
probe_scaling_driver (GPtrArray *policy_dirs)
{
  g_autoptr(GPtrArray) dirs = list_policy_dirs ();
  CpufreqMethod method = CPUFREQ_METHOD_NONE;

  for (guint i = 0; i < dirs->len; i++) {
    const char *base = g_ptr_array_index (dirs, i);
    g_autofree char *contents = NULL;
    CpufreqMethod policy_method;

    contents = read_attr (base, "scaling_driver");
    if (!contents) {
      g_debug ("Skipping %s, its scaling driver can't be read", base);
      continue;
    }

    policy_method = scaling_driver_to_method (contents);
    if (policy_method == CPUFREQ_METHOD_NONE ||
        (method != CPUFREQ_METHOD_NONE && policy_method != method)) {
      g_debug ("%s uses scaling driver %s, which this driver does not handle",
               base, contents);
      return CPUFREQ_METHOD_NONE;
    }

    method = policy_method;
    g_ptr_array_add (policy_dirs, g_strdup (base));
  }

  return method;
}

/* The path of the global boost switch if it can be written, or NULL */
static char *
find_boost_switch (void)
{
  g_autofree char *path = NULL;
  int fd;

  path = ppd_utils_get_sysfs_path (CPUFREQ_BOOST_PATH);
  fd = g_open (path, O_WRONLY);
  if (fd == -1) {
    g_debug ("Could not open the cpufreq boost switch %s for writing: %s",
             path, g_strerror (errno));
    return NULL;
  }
  g_close (fd, NULL);

  g_debug ("Found cpufreq boost switch at %s", path);
  return g_steal_pointer (&path);
}

static gboolean
probe_boost (PpdDriverCpufreq *cpufreq)
{
  cpufreq->boost_path = find_boost_switch ();
  return cpufreq->boost_path != NULL;
}

static gboolean
probe_caps (PpdDriverCpufreq *cpufreq,
            GPtrArray        *policy_dirs)
{
  g_autoptr(GPtrArray) policies = NULL;

  policies = g_ptr_array_new_with_free_func ((GDestroyNotify) policy_free);
  for (guint i = 0; i < policy_dirs->len; i++) {
    const char *dir = g_ptr_array_index (policy_dirs, i);
    g_autofree char *driver = NULL;
    Policy *policy;

    policy = probe_policy (dir);
    if (policy) {
      g_ptr_array_add (policies, policy);
      continue;
    }

    /* A cluster going offline during probe is picked up later */
    driver = read_attr (dir, "scaling_driver");
    if (!driver) {
      g_debug ("Skipping %s, it went offline", dir);
      continue;
    }

    /* Every policy must be capped, or the profiles would mean little */
    return FALSE;
  }

  if (policies->len == 0)
    return FALSE;

  g_debug ("Capping %u cpufreq policies", policies->len);
  cpufreq->policies = g_steal_pointer (&policies);
  return TRUE;
}

static PpdProbeResult
ppd_driver_cpufreq_probe (PpdDriver *driver)
{
  PpdDriverCpufreq *cpufreq = PPD_DRIVER_CPUFREQ (driver);
  g_autoptr(GPtrArray) policy_dirs = NULL;
  gboolean ret = FALSE;

  policy_dirs = g_ptr_array_new_with_free_func (g_free);
  cpufreq->method = probe_scaling_driver (policy_dirs);
  switch (cpufreq->method) {
  case CPUFREQ_METHOD_NONE:
    return PPD_PROBE_RESULT_FAIL;
  case CPUFREQ_METHOD_BOOST:
    ret = probe_boost (cpufreq);
    break;
  case CPUFREQ_METHOD_CAPS:
    ret = probe_caps (cpufreq, policy_dirs);
    break;
  }

  return ret ? PPD_PROBE_RESULT_SUCCESS : PPD_PROBE_RESULT_FAIL;
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

static guint
profile_to_percent (PpdProfile profile,
                    gboolean   on_battery)
{
  switch (profile) {
  case PPD_PROFILE_POWER_SAVER:
    return POWER_SAVER_PERCENT;
  case PPD_PROFILE_BALANCED:
    return on_battery ? BALANCED_BATTERY_PERCENT : UNCAPPED_PERCENT;
  case PPD_PROFILE_PERFORMANCE:
    return UNCAPPED_PERCENT;
  }

  g_return_val_if_reached (UNCAPPED_PERCENT);
}

/* The supported frequency nearest to the share of the maximum, the lower one
 * on a tie, so sparse frequency tables don't collapse to their minimum */
static guint64
policy_cap (Policy *policy,
            guint   percent)
{
  guint64 target = policy->max_freq * percent / 100;
  guint64 cap = policy->freqs[0];

  if (percent >= 100)
    return policy->top_freq;

  for (guint i = 1; i < policy->n_freqs; i++) {
    guint64 freq = policy->freqs[i];
    guint64 distance = freq > target ? freq - target : target - freq;
    guint64 best = cap > target ? cap - target : target - cap;

    if (distance < best)
      cap = freq;
  }

  return cap;
}

static gboolean
write_cap (Policy   *policy,
           guint64   cap,
           GError  **error)
{
  g_autofree char *path = NULL;
  g_autofree char *value = NULL;

  path = g_build_filename (policy->dir, "scaling_max_freq", NULL);
  value = g_strdup_printf ("%" G_GUINT64_FORMAT, cap);
  if (ppd_utils_write (path, value, error))
    return TRUE;

  if (error && !*error)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not write to %s", path);
  return FALSE;
}

static gboolean
has_policy (PpdDriverCpufreq *cpufreq,
            const char       *dir)
{
  for (guint i = 0; i < cpufreq->policies->len; i++) {
    Policy *policy = g_ptr_array_index (cpufreq->policies, i);

    if (g_str_equal (policy->dir, dir))
      return TRUE;
  }

  return FALSE;
}

/* Policies whose CPUs were all offline at probe, or that appeared since, are
 * picked up once they can be read, the next time a profile is applied. */
static void
pick_up_new_policies (PpdDriverCpufreq *cpufreq)
{
  g_autoptr(GPtrArray) dirs = list_policy_dirs ();

  for (guint i = 0; i < dirs->len; i++) {
    const char *base = g_ptr_array_index (dirs, i);
    g_autofree char *driver = NULL;
    Policy *policy;

    if (has_policy (cpufreq, base))
      continue;

    driver = read_attr (base, "scaling_driver");
    if (!driver || scaling_driver_to_method (driver) != CPUFREQ_METHOD_CAPS)
      continue;

    policy = probe_policy (base);
    if (policy) {
      g_debug ("Capping %s too", base);
      g_ptr_array_add (cpufreq->policies, policy);
    }
  }
}

/* A policy whose CPUs are all offline returns EBUSY; cap the others. Other
 * failures are reported, and the change only fails if nothing was capped. */
static gboolean
apply_caps (PpdDriverCpufreq  *cpufreq,
            PpdProfile         profile,
            GError           **error)
{
  GPtrArray *policies = cpufreq->policies;
  g_autoptr(GError) last_error = NULL;
  guint percent;
  guint written = 0;

  pick_up_new_policies (cpufreq);

  percent = profile_to_percent (profile, cpufreq->on_battery);
  for (guint i = 0; i < policies->len;) {
    Policy *policy = g_ptr_array_index (policies, i);
    guint64 cap = policy_cap (policy, percent);

    /* Kernels without scaling_boost_frequencies only reveal the boost
     * frequencies in cpuinfo_max_freq once boost is on. Allowing them
     * leaves the choice to the boost switch. */
    if (percent >= 100) {
      guint64 freq;

      if (read_freq (policy->dir, "cpuinfo_max_freq", &freq))
        policy->top_freq = cap = MAX (cap, freq);
    }

    g_clear_error (&last_error);
    if (!write_cap (policy, cap, &last_error)) {
      if (g_error_matches (last_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
        /* Gone; picked up again if it comes back */
        g_debug ("%s disappeared", policy->dir);
        g_ptr_array_remove_index (policies, i);
        continue;
      }
      if (g_error_matches (last_error, G_IO_ERROR, G_IO_ERROR_BUSY))
        g_debug ("Skipping inactive %s", policy->dir);
      else
        g_warning ("Could not cap %s: %s", policy->dir, last_error->message);
      i++;
      continue;
    }
    written++;
    i++;
  }

  if (written == 0) {
    if (!last_error)
      g_set_error (&last_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No cpufreq policy to cap");
    g_propagate_error (error, g_steal_pointer (&last_error));
    return FALSE;
  }

  return TRUE;
}

static gboolean
apply_profile (PpdDriverCpufreq  *cpufreq,
               PpdProfile         profile,
               GError           **error)
{
  switch (cpufreq->method) {
  case CPUFREQ_METHOD_BOOST:
    return ppd_utils_write (cpufreq->boost_path, profile_to_boost (profile), error);
  case CPUFREQ_METHOD_CAPS:
    return apply_caps (cpufreq, profile, error);
  case CPUFREQ_METHOD_NONE:
    break;
  }

  g_return_val_if_reached (FALSE);
}

static gboolean
ppd_driver_cpufreq_activate_profile (PpdDriver                   *driver,
                                     PpdProfile                   profile,
                                     PpdProfileActivationReason   reason,
                                     GError                     **error)
{
  PpdDriverCpufreq *cpufreq = PPD_DRIVER_CPUFREQ (driver);

  g_return_val_if_fail (cpufreq->method != CPUFREQ_METHOD_NONE, FALSE);

  if (profile == PPD_PROFILE_UNSET)
    return TRUE;

  if (!apply_profile (cpufreq, profile, error)) {
    if (cpufreq->activated_profile != PPD_PROFILE_UNSET) {
      g_autoptr(GError) error_local = NULL;

      if (!apply_profile (cpufreq, cpufreq->activated_profile, &error_local))
        g_warning ("failed to restore previous profile: %s", error_local->message);
    }
    return FALSE;
  }

  cpufreq->activated_profile = profile;
  return TRUE;
}

static gboolean
ppd_driver_cpufreq_power_changed (PpdDriver              *driver,
                                  PpdPowerChangedReason   reason,
                                  GError                **error)
{
  PpdDriverCpufreq *cpufreq = PPD_DRIVER_CPUFREQ (driver);
  gboolean on_battery = (reason == PPD_POWER_CHANGED_REASON_BATTERY);

  /* An unknown power source counts as AC, as with amd_pstate */
  if (cpufreq->on_battery == on_battery)
    return TRUE;
  cpufreq->on_battery = on_battery;

  /* Only balanced depends on the power source */
  if (cpufreq->method != CPUFREQ_METHOD_CAPS ||
      cpufreq->activated_profile != PPD_PROFILE_BALANCED)
    return TRUE;

  g_debug ("Power source changed, re-applying the balanced caps");
  return apply_caps (cpufreq, PPD_PROFILE_BALANCED, error);
}

/* Re-apply the profile after resume in case anything changed the boost
 * switch or lifted the caps. A cluster coming back online keeps the cap it
 * had, or none if it was offline at probe, until the next profile change or
 * resume: the daemon's sandbox (PrivateNetwork=yes) keeps udev events from
 * reaching it. */
static gboolean
ppd_driver_cpufreq_prepare_for_sleep (PpdDriver  *driver,
                                      gboolean    start,
                                      GError    **error)
{
  PpdDriverCpufreq *cpufreq = PPD_DRIVER_CPUFREQ (driver);
  g_autoptr(GError) local_error = NULL;

  if (start || cpufreq->activated_profile == PPD_PROFILE_UNSET)
    return TRUE;

  g_debug ("Re-applying the cpufreq profile");
  if (!apply_profile (cpufreq, cpufreq->activated_profile, &local_error)) {
    g_propagate_prefixed_error (error, g_steal_pointer (&local_error),
                                "Could not reapply the cpufreq profile on resume: ");
    return FALSE;
  }

  return TRUE;
}

/* A cap left behind would slow the machine down until reboot */
static void
ppd_driver_cpufreq_dispose (GObject *object)
{
  PpdDriverCpufreq *cpufreq = PPD_DRIVER_CPUFREQ (object);

  if (cpufreq->method == CPUFREQ_METHOD_CAPS &&
      cpufreq->policies &&
      cpufreq->activated_profile != PPD_PROFILE_UNSET) {
    g_debug ("Lifting cpufreq caps");
    for (guint i = 0; i < cpufreq->policies->len; i++) {
      Policy *policy = g_ptr_array_index (cpufreq->policies, i);
      g_autoptr(GError) error = NULL;

      if (write_cap (policy, policy->top_freq, &error))
        continue;
      /* The kernel keeps an offline policy's limit until it is written again */
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BUSY))
        g_warning ("%s is offline, its cap stays until it is written again or the system restarts",
                   policy->dir);
      else
        g_warning ("Could not lift the cap on %s: %s", policy->dir, error->message);
    }
    cpufreq->activated_profile = PPD_PROFILE_UNSET;
  }


  G_OBJECT_CLASS (ppd_driver_cpufreq_parent_class)->dispose (object);
}

static void
ppd_driver_cpufreq_finalize (GObject *object)
{
  PpdDriverCpufreq *cpufreq = PPD_DRIVER_CPUFREQ (object);

  g_clear_pointer (&cpufreq->boost_path, g_free);
  g_clear_pointer (&cpufreq->policies, g_ptr_array_unref);
  G_OBJECT_CLASS (ppd_driver_cpufreq_parent_class)->finalize (object);
}

static void
ppd_driver_cpufreq_class_init (PpdDriverCpufreqClass *klass)
{
  GObjectClass *object_class;
  PpdDriverClass *driver_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->constructor = ppd_driver_cpufreq_constructor;
  object_class->dispose = ppd_driver_cpufreq_dispose;
  object_class->finalize = ppd_driver_cpufreq_finalize;

  driver_class = PPD_DRIVER_CLASS (klass);
  driver_class->probe = ppd_driver_cpufreq_probe;
  driver_class->activate_profile = ppd_driver_cpufreq_activate_profile;
  driver_class->power_changed = ppd_driver_cpufreq_power_changed;
  driver_class->prepare_to_sleep = ppd_driver_cpufreq_prepare_for_sleep;
}

static void
ppd_driver_cpufreq_init (PpdDriverCpufreq *self)
{
}
