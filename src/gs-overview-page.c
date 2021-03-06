/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013-2016 Richard Hughes <richard@hughsie.com>
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

#include "config.h"

#include <glib/gi18n.h>
#include <math.h>

#include "gs-shell.h"
#include "gs-overview-page.h"
#include "gs-app-list-private.h"
#include "gs-popular-tile.h"
#include "gs-feature-tile.h"
#include "gs-category-tile.h"
#include "gs-hiding-box.h"
#include "gs-common.h"

#define N_TILES 9

typedef struct
{
	GsPluginLoader		*plugin_loader;
	GtkBuilder		*builder;
	GCancellable		*cancellable;
	gboolean		 cache_valid;
	GsShell			*shell;
	gint			 action_cnt;
	gboolean		 loading_featured;
	gboolean		 loading_popular;
	gboolean		 loading_popular_rotating;
	gboolean		 loading_categories;
	gboolean		 empty;
	gchar			*category_of_day;
	GHashTable		*category_hash;		/* id : GsCategory */
	GSettings		*settings;

	GtkWidget		*infobar_proprietary;
	GtkWidget		*label_proprietary;
	GtkWidget		*bin_featured;
	GtkWidget		*box_overview;
	GtkWidget		*box_popular;
	GtkWidget		*featured_heading;
	GtkWidget		*category_heading;
	GtkWidget		*flowbox_categories;
	GtkWidget		*flowbox_categories2;
	GtkWidget		*popular_heading;
	GtkWidget		*scrolledwindow_overview;
	GtkWidget		*stack_overview;
	GtkWidget		*categories_expander_button_down;
	GtkWidget		*categories_expander_button_up;
	GtkWidget		*categories_expander_box;
	GtkWidget		*categories_more;
} GsOverviewPagePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (GsOverviewPage, gs_overview_page, GS_TYPE_PAGE)

enum {
	SIGNAL_REFRESHED,
	SIGNAL_LAST
};

static guint signals [SIGNAL_LAST] = { 0 };

typedef struct {
        GsCategory	*category;
        GsOverviewPage	*self;
        const gchar	*title;
} LoadData;

static void
load_data_free (LoadData *data)
{
        if (data->category != NULL)
                g_object_unref (data->category);
        if (data->self != NULL)
                g_object_unref (data->self);
        g_slice_free (LoadData, data);
}

static void
gs_overview_page_invalidate (GsOverviewPage *self)
{
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);

	priv->cache_valid = FALSE;
}

static void
app_tile_clicked (GsAppTile *tile, gpointer data)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (data);
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	GsApp *app;

	app = gs_app_tile_get_app (tile);
	gs_shell_show_app (priv->shell, app);
}

static gboolean
filter_category (GsApp *app, gpointer user_data)
{
	const gchar *category = (const gchar *) user_data;

	return !gs_app_has_category (app, category);
}

static void
gs_overview_page_decrement_action_cnt (GsOverviewPage *self)
{
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);

	/* every job increcements this */
	if (priv->action_cnt == 0) {
		g_warning ("action_cnt already zero!");
		return;
	}
	if (--priv->action_cnt > 0)
		return;

	/* all done */
	priv->cache_valid = TRUE;
	g_signal_emit (self, signals[SIGNAL_REFRESHED], 0);
	priv->loading_categories = FALSE;
	priv->loading_featured = FALSE;
	priv->loading_popular = FALSE;
	priv->loading_popular_rotating = FALSE;

	/* seems a good place */
	gs_shell_profile_dump (priv->shell);
}

static void
gs_overview_page_get_popular_cb (GObject *source_object,
                                 GAsyncResult *res,
                                 gpointer user_data)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (user_data);
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	guint i;
	GsApp *app;
	GtkWidget *tile;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	/* get popular apps */
	list = gs_plugin_loader_get_popular_finish (plugin_loader, res, &error);
	gtk_widget_set_visible (priv->box_popular, list != NULL);
	gtk_widget_set_visible (priv->popular_heading, list != NULL);
	if (list == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			g_warning ("failed to get popular apps: %s", error->message);
		goto out;
	}
	/* Don't show apps from the category that's currently featured as the category of the day */
	gs_app_list_filter (list, filter_category, priv->category_of_day);
	gs_app_list_randomize (list);

	gs_container_remove_all (GTK_CONTAINER (priv->box_popular));

	for (i = 0; i < gs_app_list_length (list) && i < N_TILES; i++) {
		app = gs_app_list_index (list, i);
		tile = gs_popular_tile_new (app);
		g_signal_connect (tile, "clicked",
			  G_CALLBACK (app_tile_clicked), self);
		gtk_container_add (GTK_CONTAINER (priv->box_popular), tile);
	}

	priv->empty = FALSE;

out:
	gs_overview_page_decrement_action_cnt (self);
}

static void
gs_overview_page_category_more_cb (GtkButton *button, GsOverviewPage *self)
{
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	GsCategory *cat;
	const gchar *id;

	id = g_object_get_data (G_OBJECT (button), "GnomeSoftware::CategoryId");
	if (id == NULL)
		return;
	cat = g_hash_table_lookup (priv->category_hash, id);
	if (cat == NULL)
		return;
	gs_shell_show_category (priv->shell, cat);
}

static void
gs_overview_page_get_category_apps_cb (GObject *source_object,
                                       GAsyncResult *res,
                                       gpointer user_data)
{
	LoadData *load_data = (LoadData *) user_data;
	GsOverviewPage *self = load_data->self;
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	guint i;
	GsApp *app;
	GtkWidget *box;
	GtkWidget *button;
	GtkWidget *headerbox;
	GtkWidget *label;
	GtkWidget *tile;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	/* get popular apps */
	list = gs_plugin_loader_get_category_apps_finish (plugin_loader, res, &error);
	if (list == NULL) {
		if (g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			goto out;
		g_warning ("failed to get category %s featured applications: %s",
			   gs_category_get_id (load_data->category),
			   error->message);
		goto out;
	} else if (gs_app_list_length (list) < N_TILES) {
		g_warning ("hiding category %s featured applications: "
			   "found only %u to show, need at least %d",
			   gs_category_get_id (load_data->category),
			   gs_app_list_length (list), N_TILES);
		goto out;
	}
	gs_app_list_randomize (list);

	/* add header */
	headerbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 9);
	gtk_widget_set_visible (headerbox, TRUE);

	/* add label */
	label = gtk_label_new (load_data->title);
	gtk_widget_set_visible (label, TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.f);
	gtk_widget_set_margin_top (label, 24);
	gtk_widget_set_margin_bottom (label, 6);
	gtk_widget_set_hexpand (label, TRUE);
	gtk_style_context_add_class (gtk_widget_get_style_context (label),
				     "index-title-alignment-software");
	gtk_container_add (GTK_CONTAINER (headerbox), label);

	/* add button */
	button = gtk_button_new_with_label (_("More…"));
	gtk_style_context_add_class (gtk_widget_get_style_context (button),
				     "overview-more-button");
	g_object_set_data_full (G_OBJECT (button), "GnomeSoftware::CategoryId",
				g_strdup (gs_category_get_id (load_data->category)),
				g_free);
	gtk_widget_set_visible (button, TRUE);
	gtk_widget_set_valign (button, GTK_ALIGN_END);
	gtk_widget_set_margin_bottom (button, 9);
	g_signal_connect (button, "clicked",
			  G_CALLBACK (gs_overview_page_category_more_cb), self);
	gtk_container_add (GTK_CONTAINER (headerbox), button);
	gtk_container_add (GTK_CONTAINER (priv->box_overview), headerbox);

	/* add hiding box */
	box = gs_hiding_box_new ();
	gs_hiding_box_set_spacing (GS_HIDING_BOX (box), 14);
	gtk_widget_set_visible (box, TRUE);
	gtk_widget_set_valign (box, GTK_ALIGN_START);
	gtk_container_add (GTK_CONTAINER (priv->box_overview), box);

	/* add all the apps */
	for (i = 0; i < gs_app_list_length (list) && i < N_TILES; i++) {
		app = gs_app_list_index (list, i);
		tile = gs_popular_tile_new (app);
		g_signal_connect (tile, "clicked",
			  G_CALLBACK (app_tile_clicked), self);
		gtk_container_add (GTK_CONTAINER (box), tile);
	}

	priv->empty = FALSE;

out:
	load_data_free (load_data);
	gs_overview_page_decrement_action_cnt (self);
}

static void
gs_overview_page_get_featured_cb (GObject *source_object,
                                  GAsyncResult *res,
                                  gpointer user_data)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (user_data);
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	GtkWidget *tile;
	GsApp *app;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;

	list = gs_plugin_loader_get_featured_finish (plugin_loader, res, &error);
	if (g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
		goto out;

	if (g_getenv ("GNOME_SOFTWARE_FEATURED") == NULL) {
		/* Don't show apps from the category that's currently featured as the category of the day */
		gs_app_list_filter (list, filter_category, priv->category_of_day);
		gs_app_list_randomize (list);
	}

	gtk_widget_hide (priv->featured_heading);
	gs_container_remove_all (GTK_CONTAINER (priv->bin_featured));
	if (list == NULL) {
		g_warning ("failed to get featured apps: %s",
			   error->message);
		goto out;
	}
	if (gs_app_list_length (list) == 0) {
		g_warning ("failed to get featured apps: "
			   "no apps to show");
		goto out;
	}

	/* at the moment, we only care about the first app */
	app = gs_app_list_index (list, 0);
	tile = gs_feature_tile_new (app);
	g_signal_connect (tile, "clicked",
			  G_CALLBACK (app_tile_clicked), self);

	gtk_container_add (GTK_CONTAINER (priv->bin_featured), tile);
	gtk_widget_show (priv->featured_heading);

	priv->empty = FALSE;

out:
	gs_overview_page_decrement_action_cnt (self);
}

static void
category_tile_clicked (GsCategoryTile *tile, gpointer data)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (data);
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	GsCategory *category;

	category = gs_category_tile_get_category (tile);
	gs_shell_show_category (priv->shell, category);
}

static void
gs_overview_page_get_categories_cb (GObject *source_object,
                                    GAsyncResult *res,
                                    gpointer user_data)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (user_data);
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	GsPluginLoader *plugin_loader = GS_PLUGIN_LOADER (source_object);
	guint i;
	GsCategory *cat;
	GtkFlowBox *flowbox;
	GtkWidget *tile;
	const guint MAX_CATS_PER_SECTION = 6;
	guint added_cnt = 0;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) list = NULL;

	list = gs_plugin_loader_get_categories_finish (plugin_loader, res, &error);
	if (list == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			g_warning ("failed to get categories: %s", error->message);
		goto out;
	}
	gs_container_remove_all (GTK_CONTAINER (priv->flowbox_categories));
	gs_container_remove_all (GTK_CONTAINER (priv->flowbox_categories2));

	/* add categories to the correct flowboxes, the second being hidden */
	for (i = 0; i < list->len; i++) {
		cat = GS_CATEGORY (g_ptr_array_index (list, i));
		if (gs_category_get_size (cat) == 0)
			continue;
		tile = gs_category_tile_new (cat);
		g_signal_connect (tile, "clicked",
				  G_CALLBACK (category_tile_clicked), self);
		gs_category_tile_set_colorful (GS_CATEGORY_TILE (tile), TRUE);
		if (added_cnt < MAX_CATS_PER_SECTION) {
			flowbox = GTK_FLOW_BOX (priv->flowbox_categories);
		} else {
			flowbox = GTK_FLOW_BOX (priv->flowbox_categories2);
		}
		gtk_flow_box_insert (flowbox, tile, -1);
		gtk_widget_set_can_focus (gtk_widget_get_parent (tile), FALSE);
		added_cnt++;

		/* we save these for the 'More...' buttons */
		g_hash_table_insert (priv->category_hash,
				     g_strdup (gs_category_get_id (cat)),
				     g_object_ref (cat));
	}

	/* show the expander if we have too many children */
	gtk_widget_set_visible (priv->categories_expander_box,
				added_cnt > MAX_CATS_PER_SECTION);
out:
	if (added_cnt > 0)
		priv->empty = FALSE;
	gtk_widget_set_visible (priv->category_heading, added_cnt > 0);

	gs_overview_page_decrement_action_cnt (self);
}

static const gchar *
gs_overview_page_get_category_label (const gchar *id)
{
	if (g_strcmp0 (id, "audio-video") == 0) {
		/* TRANSLATORS: this is a heading for audio applications which
		 * have been featured ('recommended') by the distribution */
		return _("Recommended Audio & Video Applications");
	}
	if (g_strcmp0 (id, "games") == 0) {
		/* TRANSLATORS: this is a heading for games which have been
		 * featured ('recommended') by the distribution */
		return _("Recommended Games");
	}
	if (g_strcmp0 (id, "graphics") == 0) {
		/* TRANSLATORS: this is a heading for graphics applications
		 * which have been featured ('recommended') by the distribution */
		return _("Recommended Graphics Applications");
	}
	if (g_strcmp0 (id, "productivity") == 0) {
		/* TRANSLATORS: this is a heading for office applications which
		 * have been featured ('recommended') by the distribution */
		return _("Recommended Productivity Applications");
	}
	return NULL;
}

static GPtrArray *
gs_overview_page_get_random_categories (void)
{
	GPtrArray *cats;
	guint i;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(GRand) rand = NULL;
	const gchar *ids[] = { "audio-video",
			       "games",
			       "graphics",
			       "productivity",
			       NULL };

	date = g_date_time_new_now_utc ();
	rand = g_rand_new_with_seed ((guint32) g_date_time_get_day_of_year (date));
	cats = g_ptr_array_new_with_free_func (g_free);
	for (i = 0; ids[i] != NULL; i++)
		g_ptr_array_add (cats, g_strdup (ids[i]));
	for (i = 0; i < powl (cats->len + 1, 2); i++) {
		gpointer tmp;
		guint rnd1 = (guint) g_rand_int_range (rand, 0, (gint32) cats->len);
		guint rnd2 = (guint) g_rand_int_range (rand, 0, (gint32) cats->len);
		if (rnd1 == rnd2)
			continue;
		tmp = cats->pdata[rnd1];
		cats->pdata[rnd1] = cats->pdata[rnd2];
		cats->pdata[rnd2] = tmp;
	}
	for (i = 0; i < cats->len; i++) {
		const gchar *tmp = g_ptr_array_index (cats, i);
		g_debug ("%u = %s", i + 1, tmp);
	}
	return cats;
}

static void
gs_overview_page_load (GsOverviewPage *self)
{
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	guint i;

	priv->empty = TRUE;

	if (!priv->loading_featured) {
		priv->loading_featured = TRUE;
		gs_plugin_loader_get_featured_async (priv->plugin_loader,
						     GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON,
						     GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
						     priv->cancellable,
						     gs_overview_page_get_featured_cb,
						     self);
		priv->action_cnt++;
	}

	if (!priv->loading_popular) {
		priv->loading_popular = TRUE;
		gs_plugin_loader_get_popular_async (priv->plugin_loader,
						    GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEW_RATINGS |
						    GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON,
						    GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
						    priv->cancellable,
						    gs_overview_page_get_popular_cb,
						    self);
		priv->action_cnt++;
	}

	if (!priv->loading_popular_rotating) {
		const guint MAX_CATS = 2;
		g_autoptr(GPtrArray) cats_random = NULL;
		cats_random = gs_overview_page_get_random_categories ();

		/* load all the categories */
		for (i = 0; i < cats_random->len && i < MAX_CATS; i++) {
			LoadData *load_data;
			const gchar *cat_id;
			g_autoptr(GsCategory) category = NULL;
			g_autoptr(GsCategory) featured_category = NULL;

			cat_id = g_ptr_array_index (cats_random, i);
			if (i == 0) {
				g_free (priv->category_of_day);
				priv->category_of_day = g_strdup (cat_id);
			}
			category = gs_category_new (cat_id);
			featured_category = gs_category_new ("featured");
			gs_category_add_child (category, featured_category);

			load_data = g_slice_new0 (LoadData);
			load_data->category = g_object_ref (category);
			load_data->self = g_object_ref (self);
			load_data->title = gs_overview_page_get_category_label (cat_id);
			gs_plugin_loader_get_category_apps_async (priv->plugin_loader,
								  featured_category,
								  GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEW_RATINGS |
								  GS_PLUGIN_REFINE_FLAGS_REQUIRE_ICON,
								  GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
								  priv->cancellable,
								  gs_overview_page_get_category_apps_cb,
								  load_data);
			priv->action_cnt++;
		}
		priv->loading_popular_rotating = TRUE;
	}

	if (!priv->loading_categories) {
		priv->loading_categories = TRUE;
		gs_plugin_loader_get_categories_async (priv->plugin_loader,
						       GS_PLUGIN_REFINE_FLAGS_DEFAULT,
						       GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
						       priv->cancellable,
						       gs_overview_page_get_categories_cb,
						       self);
		priv->action_cnt++;
	}
}

static void
gs_overview_page_reload (GsPage *page)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (page);
	gs_overview_page_invalidate (self);
	gs_overview_page_load (self);
}

static void
gs_overview_page_switch_to (GsPage *page, gboolean scroll_up)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (page);
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	GtkWidget *widget;
	GtkAdjustment *adj;

	if (gs_shell_get_mode (priv->shell) != GS_SHELL_MODE_OVERVIEW) {
		g_warning ("Called switch_to(overview) when in mode %s",
			   gs_shell_get_mode_string (priv->shell));
		return;
	}

	/* we hid the search bar */
	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "search_button"));
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), FALSE);

	widget = GTK_WIDGET (gtk_builder_get_object (priv->builder, "buttonbox_main"));
	gtk_widget_show (widget);

	/* hide the expander */
	gtk_revealer_set_transition_duration (GTK_REVEALER (priv->categories_more), 0);
	gtk_revealer_set_reveal_child (GTK_REVEALER (priv->categories_more), FALSE);

	if (scroll_up) {
		adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (priv->scrolledwindow_overview));
		gtk_adjustment_set_value (adj, gtk_adjustment_get_lower (adj));
	}

	gs_grab_focus_when_mapped (priv->scrolledwindow_overview);

	if (priv->cache_valid || priv->action_cnt > 0)
		return;
	gs_overview_page_load (self);
}

static void
categories_more_revealer_changed_cb (GtkRevealer *revealer,
				     GParamSpec *pspec,
				     GsOverviewPage *self)
{
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	gboolean child_revealed = gtk_revealer_get_child_revealed (revealer);

	gtk_widget_set_visible (priv->categories_expander_button_up,
				child_revealed);
	gtk_widget_set_visible (priv->categories_expander_button_down,
				!child_revealed);
}

static void
gs_overview_page_categories_expander_down_cb (GtkButton *button, GsOverviewPage *self)
{
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	gtk_revealer_set_transition_duration (GTK_REVEALER (priv->categories_more), 250);
	gtk_revealer_set_reveal_child (GTK_REVEALER (priv->categories_more), TRUE);
}

static void
gs_overview_page_categories_expander_up_cb (GtkButton *button, GsOverviewPage *self)
{
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	gtk_revealer_set_transition_duration (GTK_REVEALER (priv->categories_more), 250);
	gtk_revealer_set_reveal_child (GTK_REVEALER (priv->categories_more), FALSE);
}

static void
gs_overview_page_get_sources_cb (GsPluginLoader *plugin_loader,
                                 GAsyncResult *res,
                                 GsOverviewPage *self)
{
	guint i;
	g_auto(GStrv) nonfree_ids = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) list = NULL;
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);

	/* get the results */
	list = gs_plugin_loader_get_sources_finish (plugin_loader, res, &error);
	if (list == NULL) {
		if (g_error_matches (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_CANCELLED)) {
			g_debug ("get sources cancelled");
		} else {
			g_warning ("failed to get sources: %s", error->message);
		}
		return;
	}

	/* enable each */
	nonfree_ids = g_settings_get_strv (priv->settings, "nonfree-sources");
	for (i = 0; nonfree_ids[i] != NULL; i++) {
		GsApp *app;
		g_autofree gchar *unique_id = NULL;

		/* match the ID from GSettings to an actual GsApp */
		unique_id = gs_utils_build_unique_id_kind (AS_APP_KIND_SOURCE,
							   nonfree_ids[i]);
		app = gs_app_list_lookup (list, unique_id);
		if (app == NULL) {
			g_warning ("no source for %s", unique_id);
			continue;
		}

		/* depending on the new policy, add or remove the source */
		if (g_settings_get_boolean (priv->settings, "show-nonfree-software")) {
			if (gs_app_get_state (app) == AS_APP_STATE_AVAILABLE) {
				gs_page_install_app (GS_PAGE (self), app,
						     GS_SHELL_INTERACTION_FULL,
						     priv->cancellable);
			}
		} else {
			if (gs_app_get_state (app) == AS_APP_STATE_INSTALLED) {
				gs_page_remove_app (GS_PAGE (self), app,
						    priv->cancellable);
			}
		}
	}
}

static void
gs_overview_page_rescan_proprietary_sources (GsOverviewPage *self)
{
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	gs_plugin_loader_get_sources_async (priv->plugin_loader,
					    GS_PLUGIN_REFINE_FLAGS_REQUIRE_SETUP_ACTION,
					    GS_PLUGIN_FAILURE_FLAGS_USE_EVENTS,
					    priv->cancellable,
					    (GAsyncReadyCallback) gs_overview_page_get_sources_cb,
					    self);
}

static void
gs_overview_page_proprietary_response_cb (GtkInfoBar *info_bar,
                                          gint response_id,
                                          GsOverviewPage *self)
{
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	g_settings_set_boolean (priv->settings, "show-nonfree-prompt", FALSE);
	if (response_id == GTK_RESPONSE_CLOSE) {
		gtk_widget_hide (priv->infobar_proprietary);
		return;
	}
	if (response_id != GTK_RESPONSE_YES)
		return;
	g_settings_set_boolean (priv->settings, "show-nonfree-software", TRUE);

	/* actually call into the plugin loader and do the action */
	gs_overview_page_rescan_proprietary_sources (self);
}

static void
gs_overview_page_refresh_proprietary (GsOverviewPage *self)
{
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	g_auto(GStrv) nonfree_ids = NULL;

	/* only show if never prompted and have nonfree repos */
	nonfree_ids = g_settings_get_strv (priv->settings, "nonfree-sources");
	if (g_settings_get_boolean (priv->settings, "show-nonfree-prompt") &&
	    !g_settings_get_boolean (priv->settings, "show-nonfree-software") &&
	    g_strv_length (nonfree_ids) > 0) {
		g_autoptr(GString) str = g_string_new (NULL);
		g_autofree gchar *uri = NULL;

		/* get from GSettings, as some distros want to override this */
		uri = g_settings_get_string (priv->settings, "nonfree-software-uri");

		/* TRANSLATORS: this is the proprietary info bar */
		g_string_append (str, _("Provides access to additional software, "
					"including web browsers and games."));
		g_string_append (str, " ");
		/* TRANSLATORS: this is the proprietary info bar */
		g_string_append (str, _("Proprietary software has restrictions "
					"on use and access to source code."));
		if (uri != NULL && uri[0] != '\0') {
			g_string_append (str, "\n");
			g_string_append_printf (str, "<a href=\"%s\">%s</a>",
						/* TRANSLATORS: this is the clickable
						 * link on the proprietary info bar */
						uri, _("Find out more…"));
		}
		gtk_label_set_markup (GTK_LABEL (priv->label_proprietary), str->str);
		gtk_widget_set_visible (priv->infobar_proprietary, TRUE);
	} else {
		gtk_widget_set_visible (priv->infobar_proprietary, FALSE);
	}
}

static gboolean
gs_overview_page_setup (GsPage *page,
                        GsShell *shell,
                        GsPluginLoader *plugin_loader,
                        GtkBuilder *builder,
                        GCancellable *cancellable,
                        GError **error)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (page);
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	GtkAdjustment *adj;
	GtkWidget *tile;
	gint i;

	g_return_val_if_fail (GS_IS_OVERVIEW_PAGE (self), TRUE);

	priv->plugin_loader = g_object_ref (plugin_loader);
	priv->builder = g_object_ref (builder);
	priv->cancellable = g_object_ref (cancellable);
	priv->category_hash = g_hash_table_new_full (g_str_hash, g_str_equal,
						     g_free, (GDestroyNotify) g_object_unref);

	/* create info bar if not already dismissed in initial-setup */
	gs_overview_page_refresh_proprietary (self);
	gtk_info_bar_add_button (GTK_INFO_BAR (priv->infobar_proprietary),
				 /* TRANSLATORS: button to turn on proprietary software sources */
				 _("Enable"), GTK_RESPONSE_YES);
	g_signal_connect (priv->infobar_proprietary, "response",
			  G_CALLBACK (gs_overview_page_proprietary_response_cb), self);

	/* avoid a ref cycle */
	priv->shell = shell;

	adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (priv->scrolledwindow_overview));
	gtk_container_set_focus_vadjustment (GTK_CONTAINER (priv->box_overview), adj);

	tile = gs_feature_tile_new (NULL);
	gtk_container_add (GTK_CONTAINER (priv->bin_featured), tile);

	for (i = 0; i < N_TILES; i++) {
		tile = gs_popular_tile_new (NULL);
		gtk_container_add (GTK_CONTAINER (priv->box_popular), tile);
	}

	/* handle category expander */
	g_signal_connect (priv->categories_expander_button_down, "clicked",
			  G_CALLBACK (gs_overview_page_categories_expander_down_cb), self);
	g_signal_connect (priv->categories_expander_button_up, "clicked",
			  G_CALLBACK (gs_overview_page_categories_expander_up_cb), self);
	return TRUE;
}

static void
settings_changed_cb (GSettings *settings,
		     const gchar *key,
		     GsOverviewPage *self)
{
	if (g_strcmp0 (key, "show-nonfree-software") == 0 ||
	    g_strcmp0 (key, "show-nonfree-prompt") == 0 ||
	    g_strcmp0 (key, "nonfree-software-uri") == 0 ||
	    g_strcmp0 (key, "nonfree-sources") == 0) {
		gs_overview_page_refresh_proprietary (self);
	}
}

static void
gs_overview_page_init (GsOverviewPage *self)
{
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);
	gtk_widget_init_template (GTK_WIDGET (self));
	priv->settings = g_settings_new ("org.gnome.software");
	g_signal_connect (priv->settings, "changed",
			  G_CALLBACK (settings_changed_cb),
			  self);
	gtk_revealer_set_transition_duration (GTK_REVEALER (priv->categories_more), 250);
	g_signal_connect (priv->categories_more, "notify::child-revealed",
			  G_CALLBACK (categories_more_revealer_changed_cb),
			  self);
}

static void
gs_overview_page_dispose (GObject *object)
{
	GsOverviewPage *self = GS_OVERVIEW_PAGE (object);
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);

	g_clear_object (&priv->builder);
	g_clear_object (&priv->plugin_loader);
	g_clear_object (&priv->cancellable);
	g_clear_object (&priv->settings);
	g_clear_pointer (&priv->category_of_day, g_free);
	g_clear_pointer (&priv->category_hash, g_hash_table_unref);

	G_OBJECT_CLASS (gs_overview_page_parent_class)->dispose (object);
}

static void
gs_overview_page_refreshed (GsOverviewPage *self)
{
	GsOverviewPagePrivate *priv = gs_overview_page_get_instance_private (self);

	if (priv->empty) {
		gtk_stack_set_visible_child_name (GTK_STACK (priv->stack_overview), "no-results");
	} else {
		gtk_stack_set_visible_child_name (GTK_STACK (priv->stack_overview), "overview");
	}
}

static void
gs_overview_page_class_init (GsOverviewPageClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GsPageClass *page_class = GS_PAGE_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = gs_overview_page_dispose;
	page_class->switch_to = gs_overview_page_switch_to;
	page_class->reload = gs_overview_page_reload;
	page_class->setup = gs_overview_page_setup;
	klass->refreshed = gs_overview_page_refreshed;

	signals [SIGNAL_REFRESHED] =
		g_signal_new ("refreshed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (GsOverviewPageClass, refreshed),
			      NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);

	gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Software/gs-overview-page.ui");

	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, infobar_proprietary);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, label_proprietary);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, bin_featured);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, box_overview);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, box_popular);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, category_heading);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, featured_heading);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, flowbox_categories);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, flowbox_categories2);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, popular_heading);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, scrolledwindow_overview);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, stack_overview);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, categories_expander_button_down);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, categories_expander_button_up);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, categories_expander_box);
	gtk_widget_class_bind_template_child_private (widget_class, GsOverviewPage, categories_more);
}

GsOverviewPage *
gs_overview_page_new (void)
{
	return GS_OVERVIEW_PAGE (g_object_new (GS_TYPE_OVERVIEW_PAGE, NULL));
}

/* vim: set noexpandtab: */
