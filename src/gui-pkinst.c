/*
Copyright (c) 2018 Raspberry Pi (Trading) Ltd.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#define I_KNOW_THE_PACKAGEKIT_GLIB2_API_IS_SUBJECT_TO_CHANGE
#include <packagekit-glib2/packagekit.h>

#include <X11/Xlib.h>

#include <libintl.h>

#define MSG_PULSE  -1
#define MSG_PROMPT -2
#define MSG_PAUSE  -3

/* Controls */

static GtkWidget *msg_dlg, *msg_msg, *msg_pb, *msg_btn, *msg_pbv;

gchar *pnames[2];
gboolean needs_reboot;
int calls;

/*----------------------------------------------------------------------------*/
/* Prototypes                                                                 */
/*----------------------------------------------------------------------------*/

static gboolean net_available (void);
static gboolean clock_synced (void);
static void resync (void);
static char *get_shell_string (char *cmd, gboolean all);
static char *get_string (char *cmd);
static PkResults *error_handler (PkTask *task, PkClient *client, GAsyncResult *res, char *desc);
static void message (char *msg, int type);
static gboolean quit (GtkButton *button, gpointer data);
static gboolean ntp_check (gpointer data);
static gboolean refresh_cache (gpointer data);
static void start_install (PkClient *client, GAsyncResult *res, gpointer data);
static void resolve_done (PkClient *client, GAsyncResult *res, gpointer data);
static void install_done (PkTask *task, GAsyncResult *res, gpointer data);
static gboolean close_end (gpointer data);
static void progress (PkProgress *progress, PkProgressType type, gpointer data);


/*----------------------------------------------------------------------------*/
/* Helper functions for system status                                         */
/*----------------------------------------------------------------------------*/

static gboolean net_available (void)
{
    if (system ("hostname -I | grep -q \\\\.") == 0) return TRUE;
    else return FALSE;
}

static gboolean clock_synced (void)
{
    if (system ("test -e /usr/sbin/ntpd") == 0)
    {
        if (system ("ntpq -p | grep -q ^\\*") == 0) return TRUE;
    }
    else
    {
        if (system ("timedatectl status | grep -q \"synchronized: yes\"") == 0) return TRUE;
    }
    return FALSE;
}

static void resync (void)
{
    if (system ("test -e /usr/sbin/ntpd") == 0)
    {
        system ("sudo /etc/init.d/ntp stop; sudo ntpd -gq; sudo /etc/init.d/ntp start");
    }
    else
    {
        system ("sudo systemctl -q stop systemd-timesyncd 2> /dev/null; sudo systemctl -q start systemd-timesyncd 2> /dev/null");
    }
}

static char *get_shell_string (char *cmd, gboolean all)
{
    char *line = NULL, *res = NULL;
    size_t len = 0;
    FILE *fp = popen (cmd, "r");

    if (fp == NULL) return g_strdup ("");
    if (getline (&line, &len, fp) > 0)
    {
        g_strdelimit (line, "\n\r", 0);
        if (!all)
        {
            res = line;
            while (*res++) if (g_ascii_isspace (*res)) *res = 0;
        }
        res = g_strdup (line);
    }
    pclose (fp);
    g_free (line);
    return res ? res : g_strdup ("");
}

static char *get_string (char *cmd)
{
    return get_shell_string (cmd, FALSE);
}

/*----------------------------------------------------------------------------*/
/* Voice prompts                                                              */
/*----------------------------------------------------------------------------*/

static void speak (char *filename)
{
    char *args[3] = { "/usr/bin/aplay", filename, NULL };
    g_spawn_async (PACKAGE_DATA_DIR, args, NULL, 0, NULL, NULL, NULL, NULL);
}

/*----------------------------------------------------------------------------*/
/* Helper functions for async operations                                      */
/*----------------------------------------------------------------------------*/

static PkResults *error_handler (PkTask *task, PkClient *client, GAsyncResult *res, char *desc)
{
    PkResults *results;
    PkError *pkerror;
    GError *error = NULL;
    gchar *buf;

    if (task) results = pk_task_generic_finish (task, res, &error);
    else results = pk_client_generic_finish (client, res, &error);
    if (error != NULL)
    {
        buf = g_strdup_printf (_("Error %s - %s"), desc, error->message);
        message (buf, MSG_PROMPT);
        speak ("instfail.wav");
        g_free (buf);
        g_error_free (error);
        return NULL;
    }

    pkerror = pk_results_get_error_code (results);
    if (pkerror != NULL)
    {
        buf = g_strdup_printf (_("Error %s - %s"), desc, pk_error_get_details (pkerror));
        message (buf, MSG_PROMPT);
        speak ("instfail.wav");
        g_free (buf);
        g_object_unref (pkerror);
        return NULL;
    }

    return results;
}


/*----------------------------------------------------------------------------*/
/* Progress / error box                                                       */
/*----------------------------------------------------------------------------*/

static void message (char *msg, int type)
{
    if (!msg_dlg)
    {
        GtkBuilder *builder;

        builder = gtk_builder_new_from_file (PACKAGE_DATA_DIR "/gui-pkinst.ui");

        msg_dlg = (GtkWidget *) gtk_builder_get_object (builder, "modal");
        msg_msg = (GtkWidget *) gtk_builder_get_object (builder, "modal_msg");
        msg_pb = (GtkWidget *) gtk_builder_get_object (builder, "modal_pb");
        msg_btn = (GtkWidget *) gtk_builder_get_object (builder, "modal_ok");

        g_object_unref (builder);
    }

    gtk_label_set_text (GTK_LABEL (msg_msg), msg);

    gtk_widget_hide (msg_pb);
    gtk_widget_hide (msg_btn);

    switch (type)
    {
        case MSG_PROMPT :   g_signal_connect (msg_btn, "clicked", G_CALLBACK (quit), NULL);
                            gtk_widget_show (msg_btn);
                            break;

        case MSG_PULSE :    gtk_widget_show (msg_pb);
                            gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
                            break;

        case MSG_PAUSE :    break;

        default :           gtk_widget_show (msg_pb);
                            gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (msg_pb), (type / 100.0));
                            break;
    }

    gtk_widget_show (msg_dlg);
}

static gboolean quit (GtkButton *button, gpointer data)
{
    if (msg_dlg)
    {
        gtk_widget_destroy (GTK_WIDGET (msg_dlg));
        msg_dlg = NULL;
    }

    gtk_main_quit ();
    return FALSE;
}


/*----------------------------------------------------------------------------*/
/* Handlers for asynchronous install sequence                                 */
/*----------------------------------------------------------------------------*/

static gboolean ntp_check (gpointer data)
{
    if (clock_synced ())
    {
        g_idle_add (refresh_cache, NULL);
        return FALSE;
    }

    if (calls++ > 120)
    {
        message (_("Could not sync time - exiting"), MSG_PROMPT);
        speak ("instfail.wav");
        return FALSE;
    }

    return TRUE;
}

static gboolean refresh_cache (gpointer data)
{
    PkTask *task;

    system ("pkill piwiz");
    message (_("Finding package - please wait..."), MSG_PULSE);

    task = pk_task_new ();

    pk_client_refresh_cache_async (PK_CLIENT (task), TRUE, NULL, (PkProgressCallback) progress, NULL, (GAsyncReadyCallback) start_install, NULL);
    return FALSE;
}

static void start_install (PkClient *client, GAsyncResult *res, gpointer data)
{
    if (!error_handler (NULL, client, res, _("updating cache"))) return;

    message (_("Finding package - please wait..."), MSG_PULSE);

    pk_client_resolve_async (client, 0, pnames, NULL, (PkProgressCallback) progress, NULL, (GAsyncReadyCallback) resolve_done, NULL);
}

static void resolve_done (PkClient *client, GAsyncResult *res, gpointer data)
{
    PkResults *results;
    PkPackageSack *sack;
    GPtrArray *array;
    PkPackage *item;
    PkInfoEnum info;
    gchar *package_id;
    gchar *pinst[2];

    results = error_handler (NULL, client, res, _("finding packages"));
    if (!results) return;

    sack = pk_results_get_package_sack (results);
    array = pk_package_sack_get_array (sack);
    if (array->len == 0)
    {
        message (_("Package not found - exiting"), MSG_PROMPT);
        speak ("instfail.wav");
    }
    else
    {
        item = g_ptr_array_index (array, 0);
        g_object_get (item, "info", &info, "package-id", &package_id, NULL);

        if (info == PK_INFO_ENUM_INSTALLED)
        {
            message (_("Already installed - exiting"), MSG_PROMPT);
            speak ("instfail.wav");
        }
        else
        {
            pinst[0] = package_id;
            pinst[1] = NULL;

            message (_("Downloading package - please wait..."), MSG_PULSE);

            pk_task_install_packages_async (PK_TASK (client), pinst, NULL, (PkProgressCallback) progress, NULL, (GAsyncReadyCallback) install_done, NULL);
        }
    }

    g_ptr_array_unref (array);
    g_object_unref (sack);
}

static void install_done (PkTask *task, GAsyncResult *res, gpointer data)
{
    if (!error_handler (task, NULL, res, _("installing packages"))) return;
    gtk_window_set_title (GTK_WINDOW (msg_dlg), _("Install complete"));

    if (needs_reboot)
    {
        speak ("instcompr.wav");
        message (_("Installation complete - rebooting"), MSG_PAUSE);
    }
    else
    {
        speak ("instcomp.wav");
        message (_("Installation complete"), MSG_PAUSE);
    }

    g_timeout_add_seconds (2, close_end, NULL);
}

static gboolean close_end (gpointer data)
{
    if (msg_dlg)
    {
        gtk_widget_destroy (GTK_WIDGET (msg_dlg));
        msg_dlg = NULL;
    }

    if (needs_reboot) system ("reboot");

    gtk_main_quit ();
    return FALSE;
}

static void progress (PkProgress *progress, PkProgressType type, gpointer data)
{
    int role = pk_progress_get_role (progress);
    int status = pk_progress_get_status (progress);
    int percent = pk_progress_get_percentage (progress);

    if (msg_dlg)
    {
        if ((type == PK_PROGRESS_TYPE_PERCENTAGE || type == PK_PROGRESS_TYPE_ITEM_PROGRESS
            || type == PK_PROGRESS_TYPE_PACKAGE || type == PK_PROGRESS_TYPE_PACKAGE_ID
            || type == PK_PROGRESS_TYPE_DOWNLOAD_SIZE_REMAINING || type == PK_PROGRESS_TYPE_SPEED)
            && percent >= 0 && percent <= 100)
        {
            switch (role)
            {
                case PK_ROLE_ENUM_INSTALL_PACKAGES :    if (status == PK_STATUS_ENUM_DOWNLOAD)
                                                            message (_("Downloading package - please wait..."), percent);
                                                        else if (status == PK_STATUS_ENUM_INSTALL || status == PK_STATUS_ENUM_RUNNING)
                                                            message (_("Installing package - please wait..."), percent);
                                                        else
                                                            gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
                                                        break;

                default :                               gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
                                                        break;
            }
        }
        else gtk_progress_bar_pulse (GTK_PROGRESS_BAR (msg_pb));
    }
}


/*----------------------------------------------------------------------------*/
/* Main window                                                                */
/*----------------------------------------------------------------------------*/

int main (int argc, char *argv[])
{
    char *buf;
    int res;

    // check a package name was supplied
    if (argc < 2)
    {
        printf ("No package name specified\n");
        return -1;
    }

    // is this package in the allowed list?
    buf = g_strdup_printf ("grep -qw %s /usr/share/gui-pkinst/whitelist.conf", argv[1]);
    res = system (buf);
    g_free (buf);
    if (res != 0)
    {
        printf ("Not permitted to install this package\n");
        return -1;
    }

    // check the supplied package exists and is not already installed 
    buf = g_strdup_printf ("apt-cache policy %s | grep -q \"Installed: (none)\"", argv[1]);
    res = system (buf);
    g_free (buf);
    if (res != 0)
    {
        printf ("Package not found or already installed\n");
        return -1;
    }

#ifdef ENABLE_NLS
    setlocale (LC_ALL, "");
    bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);
#endif

    // GTK setup
    g_set_prgname ("wf-panel-pi");
    gtk_init (&argc, &argv);
    gtk_icon_theme_prepend_search_path (gtk_icon_theme_get_default(), PACKAGE_DATA_DIR);

    // create a package name array using the command line argument
    pnames[0] = argv[1];
    pnames[1] = NULL;

    if (argc > 2 && !g_strcmp0 (argv[2], "reboot")) needs_reboot = TRUE;
    else needs_reboot = FALSE;

    // check the network is connected and the clock is synced
    calls = 0;
    if (!net_available ())
    {
        message (_("No network connection - exiting"), MSG_PROMPT);
        speak ("instfail.wav");
    }
    else if (!clock_synced ())
    {
        message (_("Synchronising clock - please wait..."), MSG_PULSE);
        speak ("inst.wav");
        resync ();
        g_timeout_add_seconds (1, ntp_check, NULL);
    }
    else
    {
        speak ("inst.wav");
        g_idle_add (refresh_cache, NULL);
    }

    gtk_main ();

    return 0;
}

/* End of file                                                                */
/*----------------------------------------------------------------------------*/
