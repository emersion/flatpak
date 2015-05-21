/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <gio/gio.h>
#include "libgsystem.h"
#include "libglnx/libglnx.h"

#include "xdg-app-run.h"
#include "xdg-app-utils.h"
#include "xdg-app-systemd-dbus.h"

gboolean
xdg_app_run_verify_environment_keys (const char **keys,
				     GError **error)
{
  const char *key;
  const char *environment_keys[] = {
    "x11", "wayland", "ipc", "pulseaudio", "system-dbus", "session-dbus",
    "network", "host-fs", "homedir", "dri", NULL
  };

  if (keys == NULL)
    return TRUE;

  if ((key = g_strv_subset (environment_keys, keys)) != NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "Unknown Environment key %s", key);
      return FALSE;
    }

  return TRUE;
}


static char *
extract_unix_path_from_dbus_address (const char *address)
{
  const char *path, *path_end;

  if (address == NULL)
    return NULL;

  if (!g_str_has_prefix (address, "unix:"))
    return NULL;

  path = strstr (address, "path=");
  if (path == NULL)
    return NULL;
  path += strlen ("path=");
  path_end = path;
  while (*path_end != 0 && *path_end != ',')
    path_end++;

  return g_strndup (path, path_end - path);
}

static void
xdg_app_run_add_x11_args (GPtrArray *argv_array)
{
  char *x11_socket = NULL;
  const char *display = g_getenv ("DISPLAY");

  if (display && display[0] == ':' && g_ascii_isdigit (display[1]))
    {
      const char *display_nr = &display[1];
      const char *display_nr_end = display_nr;
      g_autofree char *d = NULL;

      while (g_ascii_isdigit (*display_nr_end))
        display_nr_end++;

      d = g_strndup (display_nr, display_nr_end - display_nr);
      x11_socket = g_strdup_printf ("/tmp/.X11-unix/X%s", d);

      g_ptr_array_add (argv_array, g_strdup ("-x"));
      g_ptr_array_add (argv_array, x11_socket);
    }
}

static void
xdg_app_run_add_wayland_args (GPtrArray *argv_array)
{
  char *wayland_socket = g_build_filename (g_get_user_runtime_dir (), "wayland-0", NULL);
  if (g_file_test (wayland_socket, G_FILE_TEST_EXISTS))
    {
      g_ptr_array_add (argv_array, g_strdup ("-y"));
      g_ptr_array_add (argv_array, wayland_socket);
    }
  else
    g_free (wayland_socket);
}

static void
xdg_app_run_add_pulseaudio_args (GPtrArray *argv_array)
{
  char *pulseaudio_socket = g_build_filename (g_get_user_runtime_dir (), "pulse/native", NULL);
  if (g_file_test (pulseaudio_socket, G_FILE_TEST_EXISTS))
    {
      g_ptr_array_add (argv_array, g_strdup ("-p"));
      g_ptr_array_add (argv_array, pulseaudio_socket);
    }
}

static char *
create_proxy_socket (char *template)
{
  g_autofree char *dir = g_build_filename (g_get_user_runtime_dir (), "bus-proxy", NULL);
  g_autofree char *proxy_socket = g_build_filename (dir, template, NULL);
  int fd;

  if (mkdir (dir, 0700) == -1 && errno != EEXIST)
    return NULL;

  fd = g_mkstemp (proxy_socket);
  if (fd == -1)
    return NULL;

  close (fd);

  return g_steal_pointer (&proxy_socket);
}

void
xdg_app_run_add_system_dbus_args (GPtrArray *argv_array,
				  GPtrArray *dbus_proxy_argv)
{
  const char *dbus_address = g_getenv ("DBUS_SYSTEM_BUS_ADDRESS");
  char *dbus_system_socket = NULL;

  if (dbus_address != NULL)
    dbus_system_socket = extract_unix_path_from_dbus_address (dbus_address);
  else if (g_file_test ("/var/run/dbus/system_bus_socket", G_FILE_TEST_EXISTS))
    dbus_system_socket = g_strdup ("/var/run/dbus/system_bus_socket");

  if (dbus_system_socket != NULL)
    {
      g_ptr_array_add (argv_array, g_strdup ("-D"));
      g_ptr_array_add (argv_array, dbus_system_socket);
    }
  else if (dbus_proxy_argv && dbus_address != NULL)
    {
      g_autofree char *proxy_socket = create_proxy_socket ("system-bus-proxy-XXXXXX");

      if (proxy_socket == NULL)
	return;

      g_ptr_array_add (dbus_proxy_argv, g_strdup (dbus_address));
      g_ptr_array_add (dbus_proxy_argv, g_strdup (proxy_socket));

      g_ptr_array_add (argv_array, g_strdup ("-D"));
      g_ptr_array_add (argv_array, g_strdup (proxy_socket));
    }
}

gboolean
xdg_app_run_add_session_dbus_args (GPtrArray *argv_array,
                                   GPtrArray *dbus_proxy_argv,
                                   gboolean unrestricted)
{
  const char *dbus_address = g_getenv ("DBUS_SESSION_BUS_ADDRESS");
  char *dbus_session_socket = NULL;

  if (dbus_address == NULL)
    return FALSE;

  dbus_session_socket = extract_unix_path_from_dbus_address (dbus_address);
  if (dbus_session_socket != NULL && unrestricted)
    {
      g_ptr_array_add (argv_array, g_strdup ("-d"));
      g_ptr_array_add (argv_array, dbus_session_socket);

      return TRUE;
    }
  else if (dbus_proxy_argv && dbus_address != NULL)
    {
      g_autofree char *proxy_socket = create_proxy_socket ("session-bus-proxy-XXXXXX");

      if (proxy_socket == NULL)
	return FALSE;

      g_ptr_array_add (dbus_proxy_argv, g_strdup (dbus_address));
      g_ptr_array_add (dbus_proxy_argv, g_strdup (proxy_socket));

      g_ptr_array_add (argv_array, g_strdup ("-d"));
      g_ptr_array_add (argv_array, g_strdup (proxy_socket));

      return TRUE;
    }

  return FALSE;
}

static void
xdg_app_add_bus_filters_for_meta (GPtrArray *dbus_proxy_argv,
                                  const char *header,
                                  GKeyFile *metakey)
{
  glnx_strfreev char **keys = NULL;
  gsize i, keys_count;

  keys = g_key_file_get_keys (metakey, header, &keys_count, NULL);
  if (keys)
    {
      for (i = 0; i < keys_count; i++)
        {
          const char *key = keys[i];
          const char *key_part;
          g_autofree char *tmp = NULL;
          g_autofree char *value = g_key_file_get_string (metakey, header, key, NULL);

          if (g_str_has_suffix (key, ".*"))
            {
              tmp = g_strndup (key, strlen (key) - 2);
              key_part = tmp;
            }
          else
            key_part = key;

          if (!(g_dbus_is_name (key_part) && !g_dbus_is_unique_name (key_part)))
            {
              g_warning ("Invalid dbus name %s\n", key);
              continue;
            }

          if (strcmp (value, "see") != 0 &&
              strcmp (value, "talk") != 0 &&
              strcmp (value, "own") != 0)
            {
              g_warning ("Invalid dbus policy: %s\n", value);
              continue;
            }

          g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--%s=%s", value, key));
        }
    }
}

static void
xdg_app_add_bus_filters (GPtrArray *dbus_proxy_argv,
                         const char *app_id,
                         GKeyFile *runtime_metakey,
                         GKeyFile *metakey)
{
  g_ptr_array_add (dbus_proxy_argv, g_strdup ("--filter"));
  g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--own=%s", app_id));
  g_ptr_array_add (dbus_proxy_argv, g_strdup_printf ("--own=%s.*", app_id));

  xdg_app_add_bus_filters_for_meta (dbus_proxy_argv,
                                    "Session Bus Policy",
                                    runtime_metakey);
  if (metakey)
    xdg_app_add_bus_filters_for_meta (dbus_proxy_argv,
                                      "Session Bus Policy",
                                      metakey);
}

void
xdg_app_run_add_environment_args (GPtrArray *argv_array,
				  GPtrArray *dbus_proxy_argv,
                                  const char *app_id,
				  GKeyFile *runtime_metakey,
				  GKeyFile *metakey,
				  const char **allow,
				  const char **forbid)
{
  const char *no_opts[1] = { NULL };
  gboolean unrestricted_session_bus;
  char opts[16];
  int i;

  i = 0;
  opts[i++] = '-';

  if (allow == NULL)
    allow = no_opts;

  if (forbid == NULL)
    forbid = no_opts;

  if ((g_key_file_get_boolean (metakey, "Environment", "ipc", NULL) || g_strv_contains (allow, "ipc")) &&
      !g_strv_contains (forbid, "ipc"))
    {
      g_debug ("Allowing ipc access");
      opts[i++] = 'i';
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "dri", NULL) || g_strv_contains (allow, "dri")) &&
      !g_strv_contains (forbid, "dri"))
    {
      g_debug ("Allowing dri access");
      opts[i++] = 'g';
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "host-fs", NULL) || g_strv_contains (allow, "nost-fs")) &&
      !g_strv_contains (forbid, "host-fs"))
    {
      g_debug ("Allowing host-fs access");
      opts[i++] = 'f';
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "homedir", NULL) || g_strv_contains (allow, "homedir")) &&
      !g_strv_contains (forbid, "homedir"))
    {
      g_debug ("Allowing homedir access");
      opts[i++] = 'H';
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "network", NULL) || g_strv_contains (allow, "network")) &&
      !g_strv_contains (forbid, "network"))
    {
      g_debug ("Allowing network access");
      opts[i++] = 'n';
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "x11", NULL) || g_strv_contains (allow, "x11")) &&
      !g_strv_contains (forbid, "x11"))
    {
      g_debug ("Allowing x11 access");
      xdg_app_run_add_x11_args (argv_array);
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "wayland", NULL) || g_strv_contains (allow, "wayland")) &&
      !g_strv_contains (forbid, "wayland"))
    {
      g_debug ("Allowing wayland access");
      xdg_app_run_add_wayland_args (argv_array);
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "pulseaudio", NULL) || g_strv_contains (allow, "pulseaudio")) &&
      !g_strv_contains (forbid, "pulseaudio"))
    {
      g_debug ("Allowing pulseaudio access");
      xdg_app_run_add_pulseaudio_args (argv_array);
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "system-dbus", NULL) || g_strv_contains (allow, "system-dbus")) &&
      !g_strv_contains (forbid, "system-dbus"))
    {
      g_debug ("Allowing system-dbus access");
      xdg_app_run_add_system_dbus_args (argv_array, dbus_proxy_argv);
    }

  unrestricted_session_bus =
    (g_key_file_get_boolean (metakey, "Environment", "session-dbus", NULL) || g_strv_contains (allow, "session-dbus")) &&
    !g_strv_contains (forbid, "session-dbus");
  if (xdg_app_run_add_session_dbus_args (argv_array, dbus_proxy_argv, unrestricted_session_bus) &&
      !unrestricted_session_bus && dbus_proxy_argv)
    {
      xdg_app_add_bus_filters (dbus_proxy_argv, app_id,
                               runtime_metakey, metakey);
    }

  if ((g_key_file_get_boolean (metakey, "Environment", "session-dbus", NULL) || g_strv_contains (allow, "session-dbus")) &&
      !g_strv_contains (forbid, "session-dbus"))
    {
      g_debug ("Allowing session-dbus access");
    }

  g_assert (sizeof(opts) > i);
  if (i > 1)
    {
      opts[i++] = 0;
      g_ptr_array_add (argv_array, g_strdup (opts));
    }
}

static const struct {const char *env; const char *val;} default_exports[] = {
  {"PATH","/app/bin:/usr/bin"},
  {"LD_LIBRARY_PATH", ""},
  {"_LD_LIBRARY_PATH", "/app/lib"},
  {"XDG_CONFIG_DIRS","/app/etc/xdg:/etc/xdg"},
  {"XDG_DATA_DIRS","/app/share:/usr/share"},
  {"GI_TYPELIB_PATH","/app/lib/girepository-1.0"},
  {"SHELL","/bin/sh"},
};

static const struct {const char *env; const char *val;} devel_exports[] = {
  {"ACLOCAL_PATH","/app/share/aclocal"},
  {"C_INCLUDE_PATH","/app/include"},
  {"CPLUS_INCLUDE_PATH","/app/include"},
  {"LDFLAGS","-L/app/lib "},
  {"PKG_CONFIG_PATH","/app/lib/pkgconfig:/app/share/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"},
  {"LC_ALL","en_US.utf8"},
};

char **
xdg_app_run_get_minimal_env (gboolean devel)
{
  GPtrArray *env_array;
  static const char const *copy[] = {
    "GDMSESSION",
    "XDG_CURRENT_DESKTOP",
    "XDG_SESSION_DESKTOP",
    "DESKTOP_SESSION",
    "EMAIL_ADDRESS",
    "HOME",
    "HOSTNAME",
    "LOGNAME",
    "REAL_NAME",
    "TERM",
    "USER",
    "USERNAME",
  };
  static const char const *copy_nodevel[] = {
    "LANG",
    "LANGUAGE",
    "LC_ALL",
    "LC_ADDRESS",
    "LC_COLLATE",
    "LC_CTYPE",
    "LC_IDENTIFICATION",
    "LC_MEASUREMENT",
    "LC_MESSAGES",
    "LC_MONETARY",
    "LC_NAME",
    "LC_NUMERIC",
    "LC_PAPER",
    "LC_TELEPHONE",
    "LC_TIME",
  };
  int i;

  env_array = g_ptr_array_new_with_free_func (g_free);

  for (i = 0; i < G_N_ELEMENTS(default_exports); i++)
    g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", default_exports[i].env, default_exports[i].val));

  if (devel)
    {
      for (i = 0; i < G_N_ELEMENTS(devel_exports); i++)
        g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", devel_exports[i].env, devel_exports[i].val));
    }

  for (i = 0; i < G_N_ELEMENTS(copy); i++)
    {
      const char *current = g_getenv(copy[i]);
      if (current)
        g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", copy[i], current));
    }

  if (!devel)
    {
      for (i = 0; i < G_N_ELEMENTS(copy_nodevel); i++)
        {
          const char *current = g_getenv(copy_nodevel[i]);
          if (current)
            g_ptr_array_add (env_array, g_strdup_printf ("%s=%s", copy_nodevel[i], current));
        }
    }

  g_ptr_array_add (env_array, NULL);
  return (char **)g_ptr_array_free (env_array, FALSE);
}

char **
xdg_app_run_apply_env_default (char **envp)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS(default_exports); i++)
    envp = g_environ_setenv (envp, default_exports[i].env, default_exports[i].val, TRUE);

  return envp;
}

char **
xdg_app_run_apply_env_appid (char       **envp,
                             GFile       *app_dir)
{
  g_autoptr(GFile) app_dir_data = NULL;
  g_autoptr(GFile) app_dir_config = NULL;
  g_autoptr(GFile) app_dir_cache = NULL;

  app_dir_data = g_file_get_child (app_dir, "data");
  app_dir_config = g_file_get_child (app_dir, "config");
  app_dir_cache = g_file_get_child (app_dir, "cache");
  envp = g_environ_setenv (envp, "XDG_DATA_HOME", gs_file_get_path_cached (app_dir_data), TRUE);
  envp = g_environ_setenv (envp, "XDG_CONFIG_HOME", gs_file_get_path_cached (app_dir_config), TRUE);
  envp = g_environ_setenv (envp, "XDG_CACHE_HOME", gs_file_get_path_cached (app_dir_cache), TRUE);

  return envp;
}

char **
xdg_app_run_apply_env_vars (char **envp, GKeyFile *metakey)
{
  glnx_strfreev char **keys = NULL;
  gsize i, keys_count;

  keys = g_key_file_get_keys (metakey, "Environment Vars", &keys_count, NULL);
  if (keys)
    {
      for (i = 0; i < keys_count; i++)
        {
          const char *key = keys[i];
          g_autofree char *value = g_key_file_get_string (metakey, "Environment Vars", key, NULL);

          /* We special case LD_LIBRARY_PATH to avoid passing it top
             the helper */
          if (strcmp (key, "LD_LIBRARY_PATH") == 0)
            key = "_LD_LIBRARY_PATH";

          if (value)
            envp = g_environ_setenv (envp, key, value, TRUE);
          else
            envp = g_environ_unsetenv (envp, key);
        }
    }

  return envp;
}

GFile *
xdg_app_get_data_dir (const char *app_id)
{
  g_autoptr(GFile) home = g_file_new_for_path (g_get_home_dir ());
  g_autoptr(GFile) var_app = g_file_resolve_relative_path (home, ".var/app");

  return g_file_get_child (var_app, app_id);
}

GFile *
xdg_app_ensure_data_dir (const char *app_id,
			 GCancellable  *cancellable,
			 GError **error)
{
  g_autoptr(GFile) dir = xdg_app_get_data_dir (app_id);
  g_autoptr(GFile) data_dir = g_file_get_child (dir, "data");
  g_autoptr(GFile) cache_dir = g_file_get_child (dir, "cache");
  g_autoptr(GFile) config_dir = g_file_get_child (dir, "config");

  if (!gs_file_ensure_directory (data_dir, TRUE, cancellable, error))
    return NULL;

  if (!gs_file_ensure_directory (cache_dir, TRUE, cancellable, error))
    return NULL;

  if (!gs_file_ensure_directory (config_dir, TRUE, cancellable, error))
    return NULL;

  return g_object_ref (dir);
}

struct JobData {
  char *job;
  GMainLoop *main_loop;
};

static void
job_removed_cb (SystemdManager *manager,
                guint32 id,
                char *job,
                char *unit,
                char *result,
                struct JobData *data)
{
  if (strcmp (job, data->job) == 0)
    g_main_loop_quit (data->main_loop);
}

void
xdg_app_run_in_transient_unit (const char *appid)
{
  GDBusConnection *conn = NULL;
  GError *error = NULL;
  char *path = NULL;
  char *address = NULL;
  char *name = NULL;
  char *job = NULL;
  SystemdManager *manager = NULL;
  GVariantBuilder builder;
  GVariant *properties = NULL;
  GVariant *aux = NULL;
  guint32 pid;
  GMainContext *main_context = NULL;
  GMainLoop *main_loop = NULL;
  struct JobData data;

  path = g_strdup_printf ("/run/user/%d/systemd/private", getuid());

  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    goto out;

  main_context = g_main_context_new ();
  main_loop = g_main_loop_new (main_context, FALSE);

  g_main_context_push_thread_default (main_context);


  address = g_strconcat ("unix:path=", path, NULL);

  conn = g_dbus_connection_new_for_address_sync (address,
                                                 G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                                 NULL,
                                                 NULL, &error);
  if (!conn)
    {
      g_warning ("Can't connect to systemd: %s\n", error->message);
      goto out;
    }

  manager = systemd_manager_proxy_new_sync (conn,
                                            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                            NULL,
                                            "/org/freedesktop/systemd1",
                                            NULL, &error);
  if (!manager)
    {
      g_warning ("Can't create manager proxy: %s\n", error->message);
      goto out;
    }

  name = g_strdup_printf ("xdg-app-%s-%d.scope", appid, getpid());

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sv)"));

  pid = getpid ();
  g_variant_builder_add (&builder, "(sv)",
                         "PIDs",
                         g_variant_new_fixed_array (G_VARIANT_TYPE ("u"),
                                                    &pid, 1, sizeof (guint32))
                         );

  properties = g_variant_builder_end (&builder);

  aux = g_variant_new_array (G_VARIANT_TYPE ("(sa(sv))"), NULL, 0);

  if (!systemd_manager_call_start_transient_unit_sync (manager,
                                                       name,
                                                       "fail",
                                                       properties,
                                                       aux,
                                                       &job,
                                                       NULL,
                                                       &error))
    {
      g_warning ("Can't start transient unit: %s\n", error->message);
      goto out;
    }

  data.job = job;
  data.main_loop = main_loop;
  g_signal_connect (manager,"job-removed", G_CALLBACK (job_removed_cb), &data);

  g_main_loop_run (main_loop);

 out:
  if (main_context)
    {
      g_main_context_pop_thread_default (main_context);
      g_main_context_unref (main_context);
    }
  if (main_loop)
    g_main_loop_unref (main_loop);
  if (error)
    g_error_free (error);
  if (manager)
    g_object_unref (manager);
  if (conn)
    g_object_unref (conn);
  g_free (path);
  g_free (address);
  g_free (job);
  g_free (name);
}
