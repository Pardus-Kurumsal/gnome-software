/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015-2016 Canonical Ltd
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <config.h>

#include <json-glib/json-glib.h>
#include <snapd-glib/snapd-glib.h>
#include <gnome-software.h>

#include "gs-snapd.h"

struct GsPluginData {
	GsAuth		*auth;
};

void
gs_plugin_initialize (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_alloc_data (plugin, sizeof(GsPluginData));

	if (!gs_snapd_exists ()) {
		g_debug ("disabling '%s' as snapd not running",
			 gs_plugin_get_name (plugin));
		gs_plugin_set_enabled (plugin, FALSE);
	}

	priv->auth = gs_auth_new ("snapd");
	gs_auth_set_provider_name (priv->auth, "Snap Store");
	gs_auth_set_provider_schema (priv->auth, "com.ubuntu.UbuntuOne.GnomeSoftware");
	gs_plugin_add_auth (plugin, priv->auth);

	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "desktop-categories");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "ubuntu-reviews");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_BETTER_THAN, "packagekit");

	/* set name of MetaInfo file */
	gs_plugin_set_appstream_id (plugin, "org.gnome.Software.Plugin.Snap");
}

gboolean
gs_plugin_setup (GsPlugin *plugin, GCancellable *cancellable, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	/* load from disk */
	gs_auth_add_metadata (priv->auth, "macaroon", NULL);
	if (!gs_auth_store_load (priv->auth,
				 GS_AUTH_STORE_FLAG_USERNAME |
				 GS_AUTH_STORE_FLAG_METADATA,
				 cancellable, error))
		return FALSE;

	/* success */
	return TRUE;
}

static void
get_macaroon (GsPlugin *plugin, gchar **macaroon, gchar ***discharges)
{
	GsAuth *auth;
	const gchar *serialized_macaroon;
	g_autoptr(GVariant) macaroon_variant = NULL;
	g_autoptr (GError) error_local = NULL;

	*macaroon = NULL;
	*discharges = NULL;

	auth = gs_plugin_get_auth_by_id (plugin, "snapd");
	if (auth == NULL)
		return;
	serialized_macaroon = gs_auth_get_metadata_item (auth, "macaroon");
	if (serialized_macaroon == NULL)
		return;
	macaroon_variant = g_variant_parse (G_VARIANT_TYPE ("(sas)"),
					    serialized_macaroon,
					    NULL,
					    NULL,
					    NULL);
	if (macaroon_variant == NULL)
		return;
	g_variant_get (macaroon_variant, "(s^as)", macaroon, discharges);
}

static gboolean
gs_plugin_snap_set_app_pixbuf_from_data (GsApp *app, const gchar *buf, gsize count, GError **error)
{
	g_autoptr(GdkPixbufLoader) loader = NULL;
	g_autoptr(GError) error_local = NULL;

	loader = gdk_pixbuf_loader_new ();
	if (!gdk_pixbuf_loader_write (loader, buf, count, &error_local)) {
		g_debug ("icon_data[%" G_GSIZE_FORMAT "]=%s", count, buf);
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Failed to write: %s",
			     error_local->message);
		return FALSE;
	}
	if (!gdk_pixbuf_loader_close (loader, &error_local)) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Failed to close: %s",
			     error_local->message);
		return FALSE;
	}
	gs_app_set_pixbuf (app, gdk_pixbuf_loader_get_pixbuf (loader));
	return TRUE;
}

static gboolean
gs_plugin_snap_refine_app (GsPlugin *plugin,
			   GsApp *app,
			   JsonObject *package,
			   gboolean from_search,
			   GCancellable *cancellable,
			   GError **error)
{
	g_autofree gchar *macaroon = NULL;
	g_auto(GStrv) discharges = NULL;
	const gchar *status, *icon_url, *launch_name = NULL;
	const gchar *origin;
	g_autofree gchar *origin_hostname = NULL;
	gint64 size = -1;

	get_macaroon (plugin, &macaroon, &discharges);

	gs_app_set_kind (app, AS_APP_KIND_DESKTOP);
	status = json_object_get_string_member (package, "status");
	if (g_strcmp0 (status, "installed") == 0 || g_strcmp0 (status, "active") == 0) {
		const gchar *update_available;

		update_available = json_object_has_member (package, "update_available") ?
			json_object_get_string_member (package, "update_available") : NULL;
		if (update_available)
			gs_app_set_state (app, AS_APP_STATE_UPDATABLE);
		else {
			if (gs_app_get_state (app) == AS_APP_STATE_AVAILABLE)
				gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
			gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		}
	} else if (g_strcmp0 (status, "not installed") == 0 || g_strcmp0 (status, "available") == 0) {
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	}
	gs_app_set_name (app, GS_APP_QUALITY_HIGHEST,
			 json_object_get_string_member (package, "name"));
	gs_app_set_summary (app, GS_APP_QUALITY_HIGHEST,
			    json_object_get_string_member (package, "summary"));
	gs_app_set_description (app, GS_APP_QUALITY_HIGHEST,
				json_object_get_string_member (package, "description"));
	gs_app_set_version (app, json_object_get_string_member (package, "version"));
	if (json_object_has_member (package, "installed-size")) {
		size = json_object_get_int_member (package, "installed-size");
		if (size > 0)
			gs_app_set_size_installed (app, (guint64) size);
	}
	if (json_object_has_member (package, "download-size")) {
		size = json_object_get_int_member (package, "download-size");
		if (size > 0)
			gs_app_set_size_download (app, (guint64) size);
	}
	if (gs_plugin_check_distro_id (plugin, "ubuntu"))
		gs_app_add_quirk (app, AS_APP_QUIRK_PROVENANCE);

	/* icon is optional, either loaded from snapd or from a URL */
	icon_url = json_object_get_string_member (package, "icon");
	if (icon_url != NULL && g_strcmp0 (icon_url, "") != 0) {
		if (g_str_has_prefix (icon_url, "/")) {
			g_autofree gchar *icon_data = NULL;
			gsize icon_data_length;
			g_autoptr(GError) error_local = NULL;
			icon_data = gs_snapd_get_resource (macaroon,
							   discharges,
							   icon_url,
							   &icon_data_length,
							   cancellable,
							   error);
			if (icon_data == NULL)
				return FALSE;
			if (!gs_plugin_snap_set_app_pixbuf_from_data (app,
						(const gchar *) icon_data,
						icon_data_length,
						error)) {
				g_prefix_error (error, "Failed to load %s: ", icon_url);
				return FALSE;
			}
		} else {
			g_autoptr(SoupMessage) message = NULL;
			g_autoptr(GdkPixbufLoader) loader = NULL;
			message = soup_message_new (SOUP_METHOD_GET, icon_url);
			if (message == NULL) {
				g_set_error (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_NOT_SUPPORTED,
					     "Failed to parse icon URL: %s",
					     icon_url);
				return FALSE;
			}
			soup_session_send_message (gs_plugin_get_soup_session (plugin), message);
			if (!gs_plugin_snap_set_app_pixbuf_from_data (app,
						(const gchar *) message->response_body->data,
						message->response_body->length,
						error)) {
				g_prefix_error (error, "Failed to load %s: ", icon_url);
				return FALSE;
			}

			/* assume the origin is where the icon is coming from */
			origin_hostname = g_strdup (icon_url);
		}
	} else {
		g_autoptr(AsIcon) icon = as_icon_new ();
		as_icon_set_kind (icon, AS_ICON_KIND_STOCK);
		as_icon_set_name (icon, "package-x-generic");
		gs_app_add_icon (app, icon);
	}

	if (json_object_has_member (package, "screenshots") && gs_app_get_screenshots (app)->len <= 0) {
		JsonArray *screenshots;
		guint i;

		screenshots = json_object_get_array_member (package, "screenshots");
		for (i = 0; i < json_array_get_length (screenshots); i++) {
			JsonObject *screenshot = json_array_get_object_element (screenshots, i);
			const gchar *url = json_object_get_string_member (screenshot, "url");
			g_autoptr(AsScreenshot) ss = NULL;
			g_autoptr(AsImage) image = NULL;

			ss = as_screenshot_new ();
			as_screenshot_set_kind (ss, AS_SCREENSHOT_KIND_NORMAL);
			image = as_image_new ();
			as_image_set_url (image, url);
			as_image_set_kind (image, AS_IMAGE_KIND_SOURCE);
			as_screenshot_add_image (ss, image);
			gs_app_add_screenshot (app, ss);

			/* fall back to the screenshot */
			if (origin_hostname == NULL)
				origin_hostname = g_strdup (url);
		}
	}

	/* set the application origin */
	if (gs_app_get_origin_hostname (app) == NULL) {

		/* from the snap store */
		origin = json_object_get_string_member (package, "developer");
		if (g_strcmp0 (origin, "canonical") == 0)
			gs_app_set_origin_hostname (app, "myapps.developer.ubuntu.com");
		else if (origin_hostname != NULL)
			gs_app_set_origin_hostname (app, origin_hostname);

		/* derive this from the origin hosname */
		if (g_strcmp0 (gs_app_get_origin_hostname (app), "myapps.developer.ubuntu.com") == 0)
			gs_app_set_origin_ui (app, "The Ubuntu Store");
	}

	if (!from_search) {
		JsonArray *apps;

		apps = json_object_get_array_member (package, "apps");
		if (apps && json_array_get_length (apps) > 0)
			launch_name = json_object_get_string_member (json_array_get_object_element (apps, 0), "name");

		if (launch_name)
			gs_app_set_metadata (app, "snap::launch-name", launch_name);
		else
			gs_app_add_quirk (app, AS_APP_QUIRK_NOT_LAUNCHABLE);
	}
	return TRUE;
}

gboolean
gs_plugin_url_to_app (GsPlugin *plugin,
		      GsAppList *list,
		      const gchar *url,
		      GCancellable *cancellable,
		      GError **error)
{
	g_autofree gchar *scheme = NULL;
	g_autofree gchar *macaroon = NULL;
	g_auto(GStrv) discharges = NULL;
	g_autoptr(JsonArray) snaps = NULL;
	JsonObject *snap;
	g_autofree gchar *path = NULL;
	g_autoptr(GsApp) app = NULL;

	/* not us */
	scheme = gs_utils_get_url_scheme (url);
	if (g_strcmp0 (scheme, "snap") != 0)
		return TRUE;

	/* create app */
	path = gs_utils_get_url_path (url);
	get_macaroon (plugin, &macaroon, &discharges);
	snaps = gs_snapd_find_name (macaroon, discharges, path, cancellable, NULL);
	if (snaps == NULL || json_array_get_length (snaps) < 1)
		return TRUE;

	snap = json_array_get_object_element (snaps, 0);
	app = gs_app_new (json_object_get_string_member (snap, "name"));
	gs_app_set_scope (app, AS_APP_SCOPE_SYSTEM);
	gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_SNAP);
	gs_app_set_management_plugin (app, "snap");
	gs_app_add_quirk (app, AS_APP_QUIRK_NOT_REVIEWABLE);
	if (!gs_plugin_snap_refine_app (plugin, app, snap, TRUE, cancellable, error))
		return FALSE;
	gs_app_list_add (list, app);

	return TRUE;
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_clear_object (&priv->auth);
}

gboolean
gs_plugin_add_installed (GsPlugin *plugin,
			 GsAppList *list,
			 GCancellable *cancellable,
			 GError **error)
{
	g_autofree gchar *macaroon = NULL;
	g_auto(GStrv) discharges = NULL;
	g_autoptr(JsonArray) result = NULL;
	guint i;

	get_macaroon (plugin, &macaroon, &discharges);
	result = gs_snapd_list (macaroon, discharges, cancellable, error);
	if (result == NULL)
		return FALSE;

	for (i = 0; i < json_array_get_length (result); i++) {
		JsonObject *package = json_array_get_object_element (result, i);
		g_autoptr(GsApp) app = NULL;
		const gchar *status, *name;

		status = json_object_get_string_member (package, "status");
		if (g_strcmp0 (status, "active") != 0)
			continue;

		/* create a unique ID for deduplication, TODO: branch? */
		name = json_object_get_string_member (package, "name");
		app = gs_app_new (name);
		gs_app_set_scope (app, AS_APP_SCOPE_SYSTEM);
		gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_SNAP);
		gs_app_set_management_plugin (app, "snap");
		gs_app_add_quirk (app, AS_APP_QUIRK_NOT_REVIEWABLE);
		if (!gs_plugin_snap_refine_app (plugin, app, package, TRUE, cancellable, error))
			return FALSE;
		gs_app_list_add (list, app);
	}

	return TRUE;
}

gboolean
gs_plugin_add_search (GsPlugin *plugin,
		      gchar **values,
		      GsAppList *list,
		      GCancellable *cancellable,
		      GError **error)
{
	g_autofree gchar *macaroon = NULL;
	g_auto(GStrv) discharges = NULL;
	g_autoptr(JsonArray) result = NULL;
	guint i;

	get_macaroon (plugin, &macaroon, &discharges);
	result = gs_snapd_find (macaroon, discharges, values, cancellable, error);
	if (result == NULL)
		return FALSE;

	for (i = 0; i < json_array_get_length (result); i++) {
		JsonObject *package = json_array_get_object_element (result, i);
		g_autoptr(GsApp) app = NULL;
		const gchar *name;

		/* create a unique ID for deduplication, TODO: branch? */
		name = json_object_get_string_member (package, "name");
		app = gs_app_new (name);
		gs_app_set_scope (app, AS_APP_SCOPE_SYSTEM);
		gs_app_set_bundle_kind (app, AS_BUNDLE_KIND_SNAP);
		gs_app_set_management_plugin (app, "snap");
		gs_app_add_quirk (app, AS_APP_QUIRK_NOT_REVIEWABLE);
		if (!gs_plugin_snap_refine_app (plugin, app, package, TRUE, cancellable, error))
			return FALSE;
		gs_app_list_add (list, app);
	}

	return TRUE;
}

gboolean
gs_plugin_refine_app (GsPlugin *plugin,
		      GsApp *app,
		      GsPluginRefineFlags flags,
		      GCancellable *cancellable,
		      GError **error)
{
	g_autofree gchar *macaroon = NULL;
	g_auto(GStrv) discharges = NULL;
	g_autoptr(JsonObject) result = NULL;
	const gchar *id;

	/* not us */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snap") != 0)
		return TRUE;

	get_macaroon (plugin, &macaroon, &discharges);

	id = gs_app_get_id (app);
	if (id == NULL)
		id = gs_app_get_source_default (app);
	result = gs_snapd_list_one (macaroon, discharges, id, cancellable, error);
	if (result == NULL)
		return FALSE;
	if (!gs_plugin_snap_refine_app (plugin, app, result, FALSE, cancellable, error))
		return FALSE;

	return TRUE;
}

static void
progress_cb (JsonObject *result, gpointer user_data)
{
	GsApp *app = user_data;
	JsonArray *tasks;
	GList *task_list, *l;
	gint64 done = 0, total = 0;

	tasks = json_object_get_array_member (result, "tasks");
	task_list = json_array_get_elements (tasks);

	for (l = task_list; l != NULL; l = l->next) {
		JsonObject *task, *progress;
		gint64 task_done, task_total;

		task = json_node_get_object (l->data);
		progress = json_object_get_object_member (task, "progress");
		task_done = json_object_get_int_member (progress, "done");
		task_total = json_object_get_int_member (progress, "total");

		done += task_done;
		total += task_total;
	}

	gs_app_set_progress (app, (guint) (100 * done / total));

	g_list_free (task_list);
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	g_autofree gchar *macaroon = NULL;
	g_auto(GStrv) discharges = NULL;

	/* We can only install apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snap") != 0)
		return TRUE;

	get_macaroon (plugin, &macaroon, &discharges);

	gs_app_set_state (app, AS_APP_STATE_INSTALLING);
	get_macaroon (plugin, &macaroon, &discharges);
	if (!gs_snapd_install (macaroon, discharges, gs_app_get_id (app), progress_cb, app, cancellable, error)) {
		gs_app_set_state_recover (app);
		return FALSE;
	}
	gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	return TRUE;
}

// Check if an app is graphical by checking if it uses a known GUI interface.
// This doesn't necessarily mean that every binary uses this interfaces, but is probably true.
// https://bugs.launchpad.net/bugs/1595023
static gboolean
is_graphical (GsApp *app, GCancellable *cancellable)
{
	g_autoptr(JsonObject) result = NULL;
	JsonArray *plugs;
	guint i;
	g_autoptr(GError) error = NULL;

	result = gs_snapd_get_interfaces (NULL, NULL, cancellable, &error);
	if (result == NULL) {
		g_warning ("Failed to check interfaces: %s", error->message);
		return FALSE;
	}

	plugs = json_object_get_array_member (result, "plugs");
	for (i = 0; i < json_array_get_length (plugs); i++) {
		JsonObject *plug = json_array_get_object_element (plugs, i);
		const gchar *interface;

		// Only looks at the plugs for this snap
		if (g_strcmp0 (json_object_get_string_member (plug, "snap"), gs_app_get_id (app)) != 0)
			continue;

		interface = json_object_get_string_member (plug, "interface");
		if (interface == NULL)
			continue;

		if (g_strcmp0 (interface, "unity7") == 0 || g_strcmp0 (interface, "x11") == 0 || g_strcmp0 (interface, "mir") == 0)
			return TRUE;
	}

	return FALSE;
}

gboolean
gs_plugin_launch (GsPlugin *plugin,
		  GsApp *app,
		  GCancellable *cancellable,
		  GError **error)
{
	const gchar *launch_name;
	g_autofree gchar *binary_name = NULL;
	GAppInfoCreateFlags flags = G_APP_INFO_CREATE_NONE;
	g_autoptr(GAppInfo) info = NULL;

	/* We can only launch apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snap") != 0)
		return TRUE;

	launch_name = gs_app_get_metadata_item (app, "snap::launch-name");
	if (!launch_name)
		return TRUE;

	if (g_strcmp0 (launch_name, gs_app_get_id (app)) == 0)
		binary_name = g_strdup_printf ("/snap/bin/%s", launch_name);
	else
		binary_name = g_strdup_printf ("/snap/bin/%s.%s", gs_app_get_id (app), launch_name);

	if (!is_graphical (app, cancellable))
		flags |= G_APP_INFO_CREATE_NEEDS_TERMINAL;
	info = g_app_info_create_from_commandline (binary_name, NULL, flags, error);
	if (info == NULL)
		return FALSE;

	return g_app_info_launch (info, NULL, NULL, error);
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	g_autofree gchar *macaroon = NULL;
	g_auto(GStrv) discharges = NULL;

	/* We can only remove apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snap") != 0)
		return TRUE;

	get_macaroon (plugin, &macaroon, &discharges);

	gs_app_set_state (app, AS_APP_STATE_REMOVING);
	if (!gs_snapd_remove (macaroon, discharges, gs_app_get_id (app), progress_cb, app, cancellable, error)) {
		gs_app_set_state_recover (app);
		return FALSE;
	}
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	return TRUE;
}

gboolean
gs_plugin_auth_login (GsPlugin *plugin, GsAuth *auth,
		      GCancellable *cancellable, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autoptr(SnapdAuthData) auth_data = NULL;
	g_autoptr(GVariant) macaroon_variant = NULL;
	g_autofree gchar *serialized_macaroon = NULL;
	g_autoptr(GError) local_error = NULL;

	if (auth != priv->auth)
		return TRUE;

	auth_data = snapd_login_sync (gs_auth_get_username (auth), gs_auth_get_password (auth), gs_auth_get_pin (auth), NULL, &local_error);
	if (auth_data == NULL) {
		if (g_error_matches (local_error, SNAPD_ERROR, SNAPD_ERROR_TWO_FACTOR_REQUIRED)) {
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_PIN_REQUIRED,
					     local_error->message);
		} else if (g_error_matches (local_error, SNAPD_ERROR, SNAPD_ERROR_AUTH_DATA_INVALID) ||
			   g_error_matches (local_error, SNAPD_ERROR, SNAPD_ERROR_TWO_FACTOR_INVALID)) {
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_AUTH_INVALID,
					     local_error->message);
		} else {
			g_dbus_error_strip_remote_error (local_error);
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_NOT_SUPPORTED,
					     local_error->message);
		}
		return FALSE;
	}

	macaroon_variant = g_variant_new ("(s^as)",
					  snapd_auth_data_get_macaroon (auth_data),
					  snapd_auth_data_get_discharges (auth_data));
	serialized_macaroon = g_variant_print (macaroon_variant, FALSE);
	gs_auth_add_metadata (auth, "macaroon", serialized_macaroon);

	/* store */
	if (!gs_auth_store_save (auth,
				 GS_AUTH_STORE_FLAG_USERNAME |
				 GS_AUTH_STORE_FLAG_METADATA,
				 cancellable, error))
		return FALSE;

	gs_auth_add_flags (priv->auth, GS_AUTH_FLAG_VALID);

	return TRUE;
}

gboolean
gs_plugin_auth_lost_password (GsPlugin *plugin, GsAuth *auth,
			      GCancellable *cancellable, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	if (auth != priv->auth)
		return TRUE;

	// FIXME: snapd might not be using Ubuntu One accounts
	// https://bugs.launchpad.net/bugs/1598667
	g_set_error_literal (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_AUTH_INVALID,
			     "do online using @https://login.ubuntu.com/+forgot_password");
	return FALSE;
}

gboolean
gs_plugin_auth_register (GsPlugin *plugin, GsAuth *auth,
			 GCancellable *cancellable, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);

	if (auth != priv->auth)
		return TRUE;

	// FIXME: snapd might not be using Ubuntu One accounts
	// https://bugs.launchpad.net/bugs/1598667
	g_set_error_literal (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_AUTH_INVALID,
			     "do online using @https://login.ubuntu.com/+login");
	return FALSE;
}
