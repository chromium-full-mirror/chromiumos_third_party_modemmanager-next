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
 * Copyright (C) 2022 Fibocom Wireless Inc.
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "ModemManager.h"
#include "mm-log-object.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-broadband-modem-mbim-xmm-fibocom.h"
#include "mm-modem-helpers-mbim.h"
#include "mm-shared-fibocom.h"

static void iface_modem_init          (MMIfaceModemInterface         *iface);
static void iface_modem_3gpp_init     (MMIfaceModem3gppInterface     *iface);
static void iface_modem_firmware_init (MMIfaceModemFirmwareInterface *iface);
static void shared_fibocom_init       (MMSharedFibocomInterface      *iface);

static MMIfaceModemInterface     *iface_modem_parent;
static MMIfaceModem3gppInterface *iface_modem_3gpp_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemMbimXmmFibocom, mm_broadband_modem_mbim_xmm_fibocom, MM_TYPE_BROADBAND_MODEM_MBIM_XMM, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_FIRMWARE, iface_modem_firmware_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_SHARED_FIBOCOM,  shared_fibocom_init))

struct _MMBroadbandModemMbimXmmFibocomPrivate {
    gboolean custom_l850_att_310280_attach_required;
};

/******************************************************************************/

static gboolean
peek_device (gpointer              self,
             MbimDevice          **o_device,
             GAsyncReadyCallback   callback,
             gpointer              user_data)
{
    MMPortMbim *port;

    port = mm_broadband_modem_mbim_peek_port_mbim (MM_BROADBAND_MODEM_MBIM (self));
    if (!port) {
        g_task_report_new_error (self, callback, user_data, peek_device,
                                 MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "Couldn't peek MBIM port");
        return FALSE;
    }

    *o_device = mm_port_mbim_peek_device (port);
    return TRUE;
}

/******************************************************************************/

#define L850_MR8_VERSION 18500, 5001, 0, 7

static inline gboolean
compare_l850_version (guint A1, guint A2, guint A3,
                      guint A4, guint A5, guint A6,
                      guint B1, guint B2, guint B3, guint B4)
{
    return ((A1 == B1) && (A2 == B2) && (A4 >= B4));
}

static void
process_version_features (MMBroadbandModemMbimXmmFibocom *self,
                          const gchar                    *revision)
{
    g_auto(GStrv) split = NULL;
    guint         A1;
    guint         A2;
    guint         A3;
    guint         A4;
    guint         A5;
    guint         A6;

    /* Exit early if not L850 */
    if (!(mm_base_modem_get_vendor_id (MM_BASE_MODEM (self)) == 0x2cb7 &&
          mm_base_modem_get_product_id (MM_BASE_MODEM (self)) == 0x0007)) {
        return;
    }

    split = g_strsplit_set (revision, "._", -1);
    if (!split || g_strv_length (split) < 6) {
        mm_obj_warn (self, "failed to process firmware version string: splitting failed");
        return;
    }

    if (!mm_get_uint_from_str (split[0], &A1) ||
        !mm_get_uint_from_str (split[1], &A2) ||
        !mm_get_uint_from_str (split[2], &A3) ||
        !mm_get_uint_from_str (split[3], &A4) ||
        !mm_get_uint_from_str (split[4], &A5) ||
        !mm_get_uint_from_str (split[5], &A6)) {
        mm_obj_warn (self, "failed to process firmware version string: failed to convert to integer");
        return;
    }

    /* Check if fix for ATT attach APN for 310/280 is supported on L850 */
    self->priv->custom_l850_att_310280_attach_required = !compare_l850_version (A1, A2, A3, A4, A5, A6, L850_MR8_VERSION);
    mm_obj_info (self, "custom attach logic for AT&T 310280 %s needed",
                 self->priv->custom_l850_att_310280_attach_required ? "is" : "not");
}

/******************************************************************************/

static gchar *
load_revision_finish (MMIfaceModem  *self,
                      GAsyncResult  *res,
                      GError       **error)
{
    return g_task_propagate_pointer (G_TASK (res), error);
}

static void
parent_load_revision_ready (MMIfaceModem *self,
                            GAsyncResult *res,
                            GTask        *task)
{
    GError *error = NULL;
    gchar  *revision;

    revision = iface_modem_parent->load_revision_finish (self, res, &error);
    if (!revision) {
        g_task_return_error (task, error);
    } else {
        process_version_features (MM_BROADBAND_MODEM_MBIM_XMM_FIBOCOM (self), revision);
        g_task_return_pointer (task, revision, g_free);
    }
    g_object_unref (task);
}

static void
load_revision (MMIfaceModem        *self,
               GAsyncReadyCallback  callback,
               gpointer             user_data)
{
    g_assert (iface_modem_parent->load_revision);
    g_assert (iface_modem_parent->load_revision_finish);
    iface_modem_parent->load_revision (self,
                                       (GAsyncReadyCallback)parent_load_revision_ready,
                                       g_task_new (self, NULL, callback, user_data));
}

/******************************************************************************/
/* Hack for AT&T 310280 where the "Set LTE Attach Configuration" message is given
 * only with 2 items instead of 3 (i.e. removing the "partner" profile) */

static gboolean
att_hack_set_initial_eps_bearer_settings_finish (MMIfaceModem3gpp  *self,
                                                 GAsyncResult      *res,
                                                 GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
att_hack_set_lte_attach_configuration_set_ready (MbimDevice   *device,
                                                 GAsyncResult *res,
                                                 GTask        *task)
{
    g_autoptr(MbimMessage)  response = NULL;
    GError                 *error = NULL;

    response = mbim_device_command_finish (device, res, &error);
    if (!response || !mbim_message_response_get_result (response, MBIM_MESSAGE_TYPE_COMMAND_DONE, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

/* This function is almost identical to before_set_lte_attach_configuration_query_ready in mm-broadband-modem-mbim.c
 * The only difference is the code related to ptr_home/ptr_partner/ptr_non_partner and the flow that is executed after this function. */
static void
att_hack_before_set_lte_attach_configuration_query_ready (MbimDevice   *device,
                                                          GAsyncResult *res,
                                                          GTask        *task)
{
    MMBroadbandModemMbim                       *self;
    g_autoptr(MbimMessage)                      request = NULL;
    g_autoptr(MbimMessage)                      response = NULL;
    g_autoptr(MbimLteAttachConfigurationArray)  configurations = NULL;
    GError                                     *error = NULL;
    MMBearerProperties                         *config;
    guint32                                     n_configurations = 0;
    guint32                                     i;
    MbimLteAttachConfiguration                 *ptr_home = NULL;
    MbimLteAttachConfiguration                 *ptr_partner = NULL;
    MbimLteAttachConfiguration                 *ptr_non_partner = NULL;

    self   = g_task_get_source_object (task);
    config = g_task_get_task_data (task);

    response = mbim_device_command_finish (device, res, &error);
    if (!response ||
        !mbim_message_response_get_result (response, MBIM_MESSAGE_TYPE_COMMAND_DONE, &error) ||
        !mbim_message_ms_basic_connect_extensions_lte_attach_configuration_response_parse (
            response,
            &n_configurations,
            &configurations,
            &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* We should always receive 3 configurations but the MBIM API doesn't force
     * that so we'll just assume we don't get always the same fixed number */
    for (i = 0; i < n_configurations; i++) {
        MMBearerIpFamily ip_family;
        MMBearerAllowedAuth auth;

        /* We only support configuring the HOME settings */
        if (configurations[i]->roaming != MBIM_LTE_ATTACH_CONTEXT_ROAMING_CONTROL_HOME)
            continue;

        ip_family = mm_bearer_properties_get_ip_type (config);
        if (ip_family == MM_BEARER_IP_FAMILY_NONE || ip_family == MM_BEARER_IP_FAMILY_ANY)
            configurations[i]->ip_type = MBIM_CONTEXT_IP_TYPE_DEFAULT;
        else {
            configurations[i]->ip_type = mm_bearer_ip_family_to_mbim_context_ip_type (ip_family, &error);
            if (error) {
                configurations[i]->ip_type = MBIM_CONTEXT_IP_TYPE_DEFAULT;
                mm_obj_warn (self, "unexpected IP type settings requested: %s", error->message);
                g_clear_error (&error);
            }
        }

        g_clear_pointer (&(configurations[i]->access_string), g_free);
        configurations[i]->access_string = g_strdup (mm_bearer_properties_get_apn (config));

        g_clear_pointer (&(configurations[i]->user_name), g_free);
        configurations[i]->user_name = g_strdup (mm_bearer_properties_get_user (config));

        g_clear_pointer (&(configurations[i]->password), g_free);
        configurations[i]->password = g_strdup (mm_bearer_properties_get_password (config));

        auth = mm_bearer_properties_get_allowed_auth (config);
        if ((auth != MM_BEARER_ALLOWED_AUTH_UNKNOWN) || configurations[i]->user_name || configurations[i]->password) {
            configurations[i]->auth_protocol = mm_bearer_allowed_auth_to_mbim_auth_protocol (auth, self, &error);
            if (error) {
                configurations[i]->auth_protocol = MBIM_AUTH_PROTOCOL_NONE;
                mm_obj_warn (self, "unexpected auth settings requested: %s", error->message);
                g_clear_error (&error);
            }
        } else {
            configurations[i]->auth_protocol = MBIM_AUTH_PROTOCOL_NONE;
        }

        configurations[i]->source = MBIM_CONTEXT_SOURCE_USER;
        configurations[i]->compression = MBIM_COMPRESSION_NONE;
        break;
    }

    /* Code customization start b/224986971 */
    for (i = 0; i < n_configurations; i++) {
        if (configurations[i]->roaming == MBIM_LTE_ATTACH_CONTEXT_ROAMING_CONTROL_HOME)
            ptr_home = configurations[i];
        else if (configurations[i]->roaming == MBIM_LTE_ATTACH_CONTEXT_ROAMING_CONTROL_NON_PARTNER)
            ptr_non_partner = configurations[i];
        else
            ptr_partner = configurations[i];
    }
    /* Sort the profiles in the order home, non_partner and partner, so we can easily remove the last one(partner). */
    if (n_configurations == 3 && ptr_home && ptr_partner && ptr_non_partner) {
        mm_obj_info (self, "removing partner profile");
        configurations[0] = ptr_home;
        configurations[1] = ptr_non_partner;
        configurations[2] = ptr_partner;
        n_configurations = 2;
    }
    /* Code customization end b/224986971 */

    request = mbim_message_ms_basic_connect_extensions_lte_attach_configuration_set_new (
                  MBIM_LTE_ATTACH_CONTEXT_OPERATION_DEFAULT,
                  n_configurations,
                  (const MbimLteAttachConfiguration *const *)configurations,
                  &error);
    if (!request) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mbim_device_command (device,
                         request,
                         10,
                         NULL,
                         (GAsyncReadyCallback)att_hack_set_lte_attach_configuration_set_ready,
                         task);
}

/* This function is functionally identical to set_initial_eps_bearer_settings in mm-broadband-modem-mbim.c */
static void
att_hack_set_initial_eps_bearer_settings (MMIfaceModem3gpp    *self,
                                          MMBearerProperties  *config,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data)
{
    GTask                  *task;
    MbimDevice             *device;
    g_autoptr(MbimMessage)  message = NULL;

    if (!peek_device (MM_BROADBAND_MODEM_MBIM (self), &device, callback, user_data))
        return;

    task = g_task_new (self, NULL, callback, user_data);

    if (!mm_broadband_modem_mbim_get_is_lte_attach_info_supported (MM_BROADBAND_MODEM_MBIM (self))) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
                                 "LTE attach configuration is unsupported");
        g_object_unref (task);
        return;
    }

    g_task_set_task_data (task, g_object_ref (config), g_object_unref);

    message = mbim_message_ms_basic_connect_extensions_lte_attach_configuration_query_new (NULL);
    mbim_device_command (device,
                         message,
                         10,
                         NULL,
                         (GAsyncReadyCallback)att_hack_before_set_lte_attach_configuration_query_ready,
                         task);
}

/******************************************************************************/

static gboolean
set_initial_eps_bearer_settings_finish (MMIfaceModem3gpp  *self,
                                        GAsyncResult      *res,
                                        GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void
att_hack_set_initial_eps_bearer_settings_ready (MMIfaceModem3gpp *self,
                                                GAsyncResult     *res,
                                                GTask            *task)
{
    GError *error = NULL;

    if (!att_hack_set_initial_eps_bearer_settings_finish (self, res, &error)) {
        mm_obj_warn (self, "failed to update APN settings using custom attach logic for AT&T 310280: %s", error->message);
        g_task_return_error (task, error);
    } else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
parent_set_initial_eps_bearer_settings_ready (MMIfaceModem3gpp *self,
                                              GAsyncResult     *res,
                                              GTask            *task)
{
    GError *error = NULL;

    if (!iface_modem_3gpp_parent->set_initial_eps_bearer_settings_finish (self, res, &error)) {
        mm_obj_warn (self, "failed to update APN settings: %s", error->message);
        g_task_return_error (task, error);
    } else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
set_initial_eps_bearer_settings (MMIfaceModem3gpp    *_self,
                                 MMBearerProperties  *config,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
    MMBroadbandModemMbimXmmFibocom     *self = MM_BROADBAND_MODEM_MBIM_XMM_FIBOCOM (_self);
    GTask                              *task;
    g_autoptr(MMBaseSim)                modem_sim = NULL;
    const gchar                        *operator_identifier = NULL;

    task = g_task_new (self, NULL, callback, user_data);

    g_object_get (self,
                  MM_IFACE_MODEM_SIM, &modem_sim,
                  NULL);
    if (modem_sim)
        operator_identifier = mm_gdbus_sim_get_operator_identifier (MM_GDBUS_SIM (modem_sim));

    /* Fix for attach APN issue with ATT SIM cards MCC/MNC 310/280
     * is available on L850 MR8. Execute custom attach logic only
     * for old versions */
    if (mm_base_modem_get_vendor_id (MM_BASE_MODEM (self)) == 0x2cb7 &&
        mm_base_modem_get_product_id (MM_BASE_MODEM (self)) == 0x0007 &&
        g_strcmp0 (operator_identifier, "310280") == 0 &&
        self->priv->custom_l850_att_310280_attach_required) {
        mm_obj_info (self, "executing custom attach logic for AT&T 310280");
        att_hack_set_initial_eps_bearer_settings (_self,
                                                  config,
                                                  (GAsyncReadyCallback)att_hack_set_initial_eps_bearer_settings_ready,
                                                  task);
        return;
    }

    /* Run parent implementation without changes */
    g_assert (iface_modem_3gpp_parent->set_initial_eps_bearer_settings);
    g_assert (iface_modem_3gpp_parent->set_initial_eps_bearer_settings_finish);
    iface_modem_3gpp_parent->set_initial_eps_bearer_settings (_self,
                                                              config,
                                                              (GAsyncReadyCallback)parent_set_initial_eps_bearer_settings_ready,
                                                              task);
}

/******************************************************************************/

MMBroadbandModemMbimXmmFibocom *
mm_broadband_modem_mbim_xmm_fibocom_new (const gchar  *device,
                                         const gchar  *physdev,
                                         const gchar **drivers,
                                         const gchar  *plugin,
                                         guint16       vendor_id,
                                         guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_MBIM_XMM_FIBOCOM,
                         MM_BASE_MODEM_DEVICE,     device,
                         MM_BASE_MODEM_PHYSDEV,    physdev,
                         MM_BASE_MODEM_DRIVERS,    drivers,
                         MM_BASE_MODEM_PLUGIN,     plugin,
                         MM_BASE_MODEM_VENDOR_ID,  vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         /* MBIM bearer supports NET only */
                         MM_BASE_MODEM_DATA_NET_SUPPORTED, TRUE,
                         MM_BASE_MODEM_DATA_TTY_SUPPORTED, FALSE,
                         MM_IFACE_MODEM_SIM_HOT_SWAP_SUPPORTED, TRUE,
                         MM_IFACE_MODEM_PERIODIC_SIGNAL_CHECK_DISABLED, TRUE,
#if defined WITH_QMI && QMI_MBIM_QMUX_SUPPORTED
                         MM_BROADBAND_MODEM_MBIM_QMI_UNSUPPORTED, TRUE,
#endif
                         NULL);
}

static void
mm_broadband_modem_mbim_xmm_fibocom_init (MMBroadbandModemMbimXmmFibocom *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_BROADBAND_MODEM_MBIM_XMM_FIBOCOM,
                                              MMBroadbandModemMbimXmmFibocomPrivate);
    self->priv->custom_l850_att_310280_attach_required = TRUE;
}

static void
iface_modem_init (MMIfaceModemInterface *iface)
{
    iface_modem_parent = g_type_interface_peek_parent (iface);
    iface->load_revision = load_revision;
    iface->load_revision_finish = load_revision_finish;
}

static void
iface_modem_3gpp_init (MMIfaceModem3gppInterface *iface)
{
    iface_modem_3gpp_parent = g_type_interface_peek_parent (iface);
    iface->set_initial_eps_bearer_settings = set_initial_eps_bearer_settings;
    iface->set_initial_eps_bearer_settings_finish = set_initial_eps_bearer_settings_finish;
}

static void
iface_modem_firmware_init (MMIfaceModemFirmwareInterface *iface)
{
    iface->load_update_settings = mm_shared_fibocom_firmware_load_update_settings;
    iface->load_update_settings_finish = mm_shared_fibocom_firmware_load_update_settings_finish;
}

static MMBaseModemClass *
peek_parent_class (MMSharedFibocom *self)
{
    return MM_BASE_MODEM_CLASS (mm_broadband_modem_mbim_xmm_fibocom_parent_class);
}

static void
shared_fibocom_init (MMSharedFibocomInterface *iface)
{
    iface->peek_parent_class = peek_parent_class;
}

static void
mm_broadband_modem_mbim_xmm_fibocom_class_init (MMBroadbandModemMbimXmmFibocomClass *klass)
{
    MMBaseModemClass      *base_modem_class = MM_BASE_MODEM_CLASS (klass);
    MMBroadbandModemClass *broadband_modem_class = MM_BROADBAND_MODEM_CLASS (klass);

    g_type_class_add_private (G_OBJECT_CLASS (klass),
                              sizeof (MMBroadbandModemMbimXmmFibocomPrivate));

    base_modem_class->create_usbmisc_port = mm_shared_fibocom_create_usbmisc_port;
    base_modem_class->create_wwan_port = mm_shared_fibocom_create_wwan_port;
    broadband_modem_class->setup_ports = mm_shared_fibocom_setup_ports;
}
