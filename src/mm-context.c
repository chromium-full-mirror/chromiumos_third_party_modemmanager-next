/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2012 Aleksander Morgado <aleksander@gnu.org>
 */

#include <stdlib.h>

#include "mm-context.h"

/* Application context */
static gboolean debug;
static const gchar *log_level;
static const gchar *log_file;
static gboolean show_ts;
static gboolean rel_ts;

/* Context to initialize test mode */
static gboolean test_mode;
static const gchar *test_plugin_name;
static const gchar **test_at_ports;
static const gchar **test_net_ports;
/* Optional */
static gboolean test_modem_hotplugged;
static guint16 test_modem_vendor;
static guint16 test_modem_product;
static const gchar **test_modem_drivers;

static const GOptionEntry entries[] = {
    { "debug", 0, 0, G_OPTION_ARG_NONE, &debug, "Run with extended debugging capabilities", NULL },
    { "log-level", 0, 0, G_OPTION_ARG_STRING, &log_level, "Log level: one of [ERR, WARN, INFO, DEBUG]", "INFO" },
    { "log-file", 0, 0, G_OPTION_ARG_STRING, &log_file, "Path to log file", NULL },
    { "timestamps", 0, 0, G_OPTION_ARG_NONE, &show_ts, "Show timestamps in log output", NULL },
    { "relative-timestamps", 0, 0, G_OPTION_ARG_NONE, &rel_ts, "Use relative timestamps (from MM start)", NULL },
    { NULL }
};

static const GOptionEntry test_entries[] = {
    { "test", 0, 0, G_OPTION_ARG_NONE, &test_mode,
      "Run in test mode.", NULL },
    { "test-plugin", 0, 0, G_OPTION_ARG_STRING, &test_plugin_name,
      "Load the specified plugin.", "plugin" },
    { "test-at-port", 0, 0, G_OPTION_ARG_STRING_ARRAY, &test_at_ports,
      "[multiple OK] Add specified port to list of modem AT ports.", "port" },
    { "test-net-port", 0, 0, G_OPTION_ARG_STRING_ARRAY, &test_net_ports,
      "[mulitple OK] Add specified port to list of modem net ports.", "net" },
    { "test-modem-hotplugged", 0, 0, G_OPTION_ARG_NONE, &test_modem_hotplugged,
      "Was the device hotplugged? default:FALSE", NULL },
    { "test-modem-vendor", 0, 0, G_OPTION_ARG_INT, &test_modem_vendor,
      "[optional] Vendor ID (VID) for the test modem.", "vendor" },
    { "test-modem-product", 0, 0, G_OPTION_ARG_INT, &test_modem_product,
      "[optional] Product ID (PID) for the test modem.", "product" },
    { "test-modem-driver", 0, 0, G_OPTION_ARG_STRING_ARRAY, &test_modem_drivers,
      "[optional/multiple OK] Driver to be used for the test modem.",
      "driver" },
    { NULL }
};

gboolean
mm_context_get_debug (void)
{
    return debug;
}

const gchar *
mm_context_get_log_level (void)
{
    return log_level;
}

const gchar *
mm_context_get_log_file (void)
{
    return log_file;
}

gboolean
mm_context_get_timestamps (void)
{
    return show_ts;
}

gboolean
mm_context_get_relative_timestamps (void)
{
    return rel_ts;
}

gboolean
mm_context_get_test_mode (void)
{
    return test_mode;
}

const gchar *
mm_context_get_test_plugin_name (void)
{
    return test_plugin_name;
}

const gchar **
mm_context_get_test_at_ports (void)
{
    return test_at_ports;
}

const gchar **
mm_context_get_test_net_ports (void)
{
    return test_net_ports;
}

gboolean
mm_context_get_test_modem_hotplugged (void)
{
    return test_modem_hotplugged;
}

guint16
mm_context_get_test_modem_vendor (void)
{
    return test_modem_vendor;
}

guint16
mm_context_get_test_modem_product (void)
{
    return test_modem_product;
}

const gchar **
mm_context_get_test_modem_drivers (void)
{
    return test_modem_drivers;
}

void
mm_context_init (gint argc,
                 gchar **argv)
{
    GError *error = NULL;
    gchar *err_msg;
    GOptionContext *ctx;
    GOptionGroup *test_group;

    ctx = g_option_context_new (NULL);
    g_option_context_set_summary (ctx, "DBus system service to communicate with modems.");
    g_option_context_add_main_entries (ctx, entries, NULL);

    test_group = g_option_group_new (
        "test",
        "The intial discovery and plugin matching sequence is skipped in test mode. "
        "Instead, the ports exposed by the modem and the plugin to be used must be specified "
        "explicitly at the command line.",
        "Test mode options.",
        NULL,
        NULL);
    g_option_group_add_entries (test_group, test_entries);
    g_option_context_add_group (ctx, test_group);

    /* setup defaults */
    test_modem_hotplugged = FALSE;
    test_modem_vendor = 0;
    test_modem_product = 0;

    if (!g_option_context_parse (ctx, &argc, &argv, &error)) {
		g_warning ("%s\n", error->message);
		g_error_free (error);
        exit (1);
    }

    /* Additional setup to be done on debug mode */
    if (debug) {
        log_level = "DEBUG";
        if (!show_ts && !rel_ts)
            show_ts = TRUE;
    }

    /* Additional setup for test mode */
    if (test_mode) {
        /* --test-plugin and --test-ports are mandatory flags in this mode */
        if (test_plugin_name == NULL || test_at_ports == NULL || test_net_ports == NULL) {
            g_warning ("Insufficient arguments.\n");
            err_msg = g_option_context_get_help (ctx, FALSE, test_group);
            g_message ("%s\n", err_msg);
            free (err_msg);
            exit (1);
        }
    }

    g_option_context_free (ctx);
}
