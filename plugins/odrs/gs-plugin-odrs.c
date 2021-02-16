/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 * vi:set noexpandtab tabstop=8 shiftwidth=8:
 *
 * Copyright (C) 2016 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2016-2018 Kalev Lember <klember@redhat.com>
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#include <config.h>

#include <glib/gi18n.h>
#include <gnome-software.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <math.h>

/*
 * SECTION:
 * Provides review data from the Open Desktop Ratings Service.
 *
 * To test this plugin locally you will probably want to build and run the
 * `odrs-web` container, following the instructions in the
 * [`odrs-web` repository](https://gitlab.gnome.org/Infrastructure/odrs-web/-/blob/master/README.md),
 * and then get gnome-software to use your local review server by running:
 * ```
 * gsettings set org.gnome.software review-server 'http://127.0.0.1:5000/1.0/reviews/api'
 * ```
 *
 * When you are done with development, run the following command to use the real
 * ODRS server again:
 * ```
 * gsettings reset org.gnome.software review-server
 * ```
 */

#if !GLIB_CHECK_VERSION(2, 62, 0)
typedef struct
{
  guint8 *data;
  guint   len;
  guint   alloc;
  guint   elt_size;
  guint   zero_terminated : 1;
  guint   clear : 1;
  gatomicrefcount ref_count;
  GDestroyNotify clear_func;
} GRealArray;

static gboolean
g_array_binary_search (GArray        *array,
                       gconstpointer  target,
                       GCompareFunc   compare_func,
                       guint         *out_match_index)
{
  gboolean result = FALSE;
  GRealArray *_array = (GRealArray *) array;
  guint left, middle, right;
  gint val;

  g_return_val_if_fail (_array != NULL, FALSE);
  g_return_val_if_fail (compare_func != NULL, FALSE);

  if (G_LIKELY(_array->len))
    {
      left = 0;
      right = _array->len - 1;

      while (left <= right)
        {
          middle = left + (right - left) / 2;

          val = compare_func (_array->data + (_array->elt_size * middle), target);
          if (val == 0)
            {
              result = TRUE;
              break;
            }
          else if (val < 0)
            left = middle + 1;
          else if (/* val > 0 && */ middle > 0)
            right = middle - 1;
          else
            break;  /* element not found */
        }
    }

  if (result && out_match_index != NULL)
    *out_match_index = middle;

  return result;
}
#endif  /* glib < 2.62.0 */

#define ODRS_REVIEW_CACHE_AGE_MAX		237000 /* 1 week */
#define ODRS_REVIEW_NUMBER_RESULTS_MAX		20

/* Element in priv->ratings, all allocated in one big block and sorted
 * alphabetically to reduce the number of allocations and fragmentation. */
typedef struct {
	gchar *app_id;  /* (owned) */
	guint32 n_star_ratings[6];
} GsOdrsRating;

static int
rating_compare (const GsOdrsRating *a, const GsOdrsRating *b)
{
	return g_strcmp0 (a->app_id, b->app_id);
}

static void
rating_clear (GsOdrsRating *rating)
{
	g_free (rating->app_id);
}

struct GsPluginData {
	GSettings		*settings;
	gchar			*distro;
	gchar			*user_hash;
	gchar			*review_server;
	GMutex			 review_server_lock;
	gulong			 review_server_changed_id;
	GArray			*ratings;  /* (element-type GsOdrsRating) (mutex ratings_mutex) (owned) (nullable) */
	GMutex			 ratings_mutex;
	GsApp			*cached_origin;
};

static void
gs_plugin_odrs_review_server_changed_cb (GSettings *settings,
					 const gchar *key,
					 GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autofree gchar *review_server = NULL;
	g_autoptr(GMutexLocker) locker = NULL;

	locker = g_mutex_locker_new (&priv->review_server_lock);
	review_server = g_settings_get_string (settings, "review-server");
	if (g_strcmp0 (review_server, priv->review_server) != 0) {
		g_free (priv->review_server);
		priv->review_server = g_steal_pointer (&review_server);

		gs_plugin_set_enabled (plugin, priv->review_server && priv->review_server[0] != '\0');
		gs_app_set_origin_hostname (priv->cached_origin, priv->review_server);
	}
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_alloc_data (plugin, sizeof(GsPluginData));
	g_autoptr(GError) error = NULL;
	g_autoptr(GsOsRelease) os_release = NULL;

	g_mutex_init (&priv->ratings_mutex);
	g_mutex_init (&priv->review_server_lock);
	priv->settings = g_settings_new ("org.gnome.software");
	priv->review_server_changed_id =
		g_signal_connect (priv->settings, "changed::review-server",
			G_CALLBACK (gs_plugin_odrs_review_server_changed_cb), plugin);
	priv->review_server = NULL;
	priv->ratings = NULL;  /* until first refreshed */

	/* get the machine+user ID hash value */
	priv->user_hash = gs_utils_get_user_hash (&error);
	if (priv->user_hash == NULL) {
		g_warning ("Failed to get machine+user hash: %s", error->message);
		return;
	}

	/* get the distro name (e.g. 'Fedora') but allow a fallback */
	os_release = gs_os_release_new (&error);
	if (os_release != NULL) {
		priv->distro = g_strdup (gs_os_release_get_name (os_release));
		if (priv->distro == NULL) {
			g_warning ("no distro name specified");
			priv->distro = g_strdup ("Unknown");
		}
	} else {
		g_warning ("failed to get distro name: %s", error->message);
		priv->distro = g_strdup ("Unknown");
	}

	/* add source */
	priv->cached_origin = gs_app_new (gs_plugin_get_name (plugin));
	gs_app_set_kind (priv->cached_origin, AS_COMPONENT_KIND_REPOSITORY);

	gs_plugin_odrs_review_server_changed_cb (priv->settings, NULL, plugin);

	/* add the source to the plugin cache which allows us to match the
	 * unique ID to a GsApp when creating an event */
	gs_plugin_cache_add (plugin,
			     gs_app_get_unique_id (priv->cached_origin),
			     priv->cached_origin);

	/* need application IDs and version */
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "appstream");
	gs_plugin_add_rule (plugin, GS_PLUGIN_RULE_RUN_AFTER, "flatpak");

	/* set name of MetaInfo file */
	gs_plugin_set_appstream_id (plugin, "org.gnome.Software.Plugin.Odrs");
}

static gboolean
gs_plugin_odrs_load_ratings_for_app (JsonObject *json_app, const gchar *app_id, GsOdrsRating *rating_out)
{
	guint i;
	const gchar *names[] = { "star0", "star1", "star2", "star3",
				 "star4", "star5", NULL };

	for (i = 0; names[i] != NULL; i++) {
		if (!json_object_has_member (json_app, names[i]))
			return FALSE;
		rating_out->n_star_ratings[i] = (guint64) json_object_get_int_member (json_app, names[i]);
	}

	rating_out->app_id = g_strdup (app_id);

	return TRUE;
}

static gboolean
gs_plugin_odrs_load_ratings (GsPlugin *plugin, const gchar *fn, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	JsonNode *json_root;
	JsonObject *json_item;
	g_autoptr(JsonParser) json_parser = NULL;
	const gchar *app_id;
	JsonNode *json_app_node;
	JsonObjectIter iter;
	g_autoptr(GArray) new_ratings = NULL;
	g_autoptr(GMutexLocker) locker = NULL;

	/* parse the data and find the success */
	json_parser = json_parser_new_immutable ();
#if JSON_CHECK_VERSION(1, 6, 0)
	if (!json_parser_load_from_mapped_file (json_parser, fn, error)) {
#else
	if (!json_parser_load_from_file (json_parser, fn, error)) {
#endif
		gs_utils_error_convert_json_glib (error);
		return FALSE;
	}
	json_root = json_parser_get_root (json_parser);
	if (json_root == NULL) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_INVALID_FORMAT,
				     "no ratings root");
		return FALSE;
	}
	if (json_node_get_node_type (json_root) != JSON_NODE_OBJECT) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_INVALID_FORMAT,
				     "no ratings array");
		return FALSE;
	}

	json_item = json_node_get_object (json_root);

	new_ratings = g_array_sized_new (FALSE,  /* don’t zero-terminate */
					 FALSE,  /* don’t clear */
					 sizeof (GsOdrsRating),
					 json_object_get_size (json_item));
	g_array_set_clear_func (new_ratings, (GDestroyNotify) rating_clear);

	/* parse each app */
	json_object_iter_init (&iter, json_item);
	while (json_object_iter_next (&iter, &app_id, &json_app_node)) {
		GsOdrsRating rating;
		JsonObject *json_app;

		if (!JSON_NODE_HOLDS_OBJECT (json_app_node))
			continue;
		json_app = json_node_get_object (json_app_node);

		if (gs_plugin_odrs_load_ratings_for_app (json_app, app_id, &rating))
			g_array_append_val (new_ratings, rating);
	}

	/* Allow for binary searches later. */
	g_array_sort (new_ratings, (GCompareFunc) rating_compare);

	/* Update the shared state */
	locker = g_mutex_locker_new (&priv->ratings_mutex);
	g_clear_pointer (&priv->ratings, g_array_unref);
	priv->ratings = g_steal_pointer (&new_ratings);

	return TRUE;
}

gboolean
gs_plugin_refresh (GsPlugin *plugin,
		   guint cache_age,
		   GCancellable *cancellable,
		   GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autofree gchar *cache_filename = NULL;
	g_autofree gchar *uri = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GsApp) app_dl = gs_app_new (gs_plugin_get_name (plugin));
	g_autoptr(GMutexLocker) locker = NULL;

	locker = g_mutex_locker_new (&priv->review_server_lock);
	if (priv->review_server == NULL || priv->review_server[0] == '\0')
		return TRUE;
	uri = g_strdup_printf ("%s/ratings", priv->review_server);
	g_clear_pointer (&locker, g_mutex_locker_free);

	/* check cache age */
	cache_filename = gs_utils_get_cache_filename ("odrs",
						      "ratings.json",
						      GS_UTILS_CACHE_FLAG_WRITEABLE,
						      error);
	if (cache_filename == NULL)
		return FALSE;
	if (cache_age > 0) {
		guint tmp;
		g_autoptr(GFile) file = NULL;
		file = g_file_new_for_path (cache_filename);
		tmp = gs_utils_get_file_age (file);
		if (tmp < cache_age) {
			g_debug ("%s is only %u seconds old, so ignoring refresh",
				 cache_filename, tmp);
			return gs_plugin_odrs_load_ratings (plugin, cache_filename, error);
		}
	}

	/* download the complete file */
	g_debug ("Updating ODRS cache from %s to %s", uri, cache_filename);
	gs_app_set_summary_missing (app_dl,
				    /* TRANSLATORS: status text when downloading */
				    _("Downloading application ratings…"));
	if (!gs_plugin_download_file (plugin, app_dl, uri, cache_filename, cancellable, &error_local)) {
		g_autoptr(GsPluginEvent) event = gs_plugin_event_new ();

		gs_plugin_event_set_error (event, error_local);
		gs_plugin_event_set_action (event, GS_PLUGIN_ACTION_DOWNLOAD);
		gs_plugin_event_set_origin (event, priv->cached_origin);
		if (gs_plugin_has_flags (plugin, GS_PLUGIN_FLAGS_INTERACTIVE))
			gs_plugin_event_add_flag (event, GS_PLUGIN_EVENT_FLAG_INTERACTIVE);
		else
			gs_plugin_event_add_flag (event, GS_PLUGIN_EVENT_FLAG_WARNING);
		gs_plugin_report_event (plugin, event);

		/* don't fail updates if the ratings server is unavailable */
		return TRUE;
	}
	return gs_plugin_odrs_load_ratings (plugin, cache_filename, error);
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_free (priv->user_hash);
	g_free (priv->distro);
	g_free (priv->review_server);
	g_clear_pointer (&priv->ratings, g_array_unref);
	if (priv->review_server_changed_id)
		g_signal_handler_disconnect (priv->settings, priv->review_server_changed_id);
	g_object_unref (priv->settings);
	g_object_unref (priv->cached_origin);
	g_mutex_clear (&priv->review_server_lock);
	g_mutex_clear (&priv->ratings_mutex);
}

static AsReview *
gs_plugin_odrs_parse_review_object (GsPlugin *plugin, JsonObject *item)
{
	AsReview *rev = as_review_new ();

	/* date */
	if (json_object_has_member (item, "date_created")) {
		gint64 timestamp;
		g_autoptr(GDateTime) dt = NULL;
		timestamp = json_object_get_int_member (item, "date_created");
		dt = g_date_time_new_from_unix_utc (timestamp);
		as_review_set_date (rev, dt);
	}

	/* assemble review */
	if (json_object_has_member (item, "rating"))
		as_review_set_rating (rev, (gint) json_object_get_int_member (item, "rating"));
	if (json_object_has_member (item, "score")) {
		as_review_set_priority (rev, (gint) json_object_get_int_member (item, "score"));
	} else if (json_object_has_member (item, "karma_up") &&
		   json_object_has_member (item, "karma_down")) {
		gdouble ku = (gdouble) json_object_get_int_member (item, "karma_up");
		gdouble kd = (gdouble) json_object_get_int_member (item, "karma_down");
		gdouble wilson = 0.f;

		/* from http://www.evanmiller.org/how-not-to-sort-by-average-rating.html */
		if (ku > 0 || kd > 0) {
			wilson = ((ku + 1.9208) / (ku + kd) -
				  1.96 * sqrt ((ku * kd) / (ku + kd) + 0.9604) /
				  (ku + kd)) / (1 + 3.8416 / (ku + kd));
			wilson *= 100.f;
		}
		as_review_set_priority (rev, (gint) wilson);
	}
	if (json_object_has_member (item, "user_hash"))
		as_review_set_reviewer_id (rev, json_object_get_string_member (item, "user_hash"));
	if (json_object_has_member (item, "user_display"))
		as_review_set_reviewer_name (rev, json_object_get_string_member (item, "user_display"));
	if (json_object_has_member (item, "summary"))
		as_review_set_summary (rev, json_object_get_string_member (item, "summary"));
	if (json_object_has_member (item, "description"))
		as_review_set_description (rev, json_object_get_string_member (item, "description"));
	if (json_object_has_member (item, "version"))
		as_review_set_version (rev, json_object_get_string_member (item, "version"));

	/* add extra metadata for the plugin */
	if (json_object_has_member (item, "user_skey")) {
		as_review_add_metadata (rev, "user_skey",
					json_object_get_string_member (item, "user_skey"));
	}
	if (json_object_has_member (item, "app_id")) {
		as_review_add_metadata (rev, "app_id",
					json_object_get_string_member (item, "app_id"));
	}
	if (json_object_has_member (item, "review_id")) {
		g_autofree gchar *review_id = NULL;
		review_id = g_strdup_printf ("%" G_GINT64_FORMAT,
					json_object_get_int_member (item, "review_id"));
		as_review_set_id (rev, review_id);
	}

	/* don't allow multiple votes */
	if (json_object_has_member (item, "vote_id"))
		as_review_add_flags (rev, AS_REVIEW_FLAG_VOTED);

	return rev;
}

static GPtrArray *
gs_plugin_odrs_parse_reviews (GsPlugin *plugin,
			      const gchar *data,
			      gssize data_len,
			      GError **error)
{
	JsonArray *json_reviews;
	JsonNode *json_root;
	guint i;
	g_autoptr(JsonParser) json_parser = NULL;
	g_autoptr(GHashTable) reviewer_ids = NULL;
	g_autoptr(GPtrArray) reviews = NULL;

	/* nothing */
	if (data == NULL) {
		if (!gs_plugin_get_network_available (plugin))
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_NO_NETWORK,
					     "server couldn't be reached");
		else
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_INVALID_FORMAT,
					     "server returned no data");
		return NULL;
	}

	/* parse the data and find the array or ratings */
	json_parser = json_parser_new_immutable ();
	if (!json_parser_load_from_data (json_parser, data, data_len, error)) {
		gs_utils_error_convert_json_glib (error);
		return NULL;
	}
	json_root = json_parser_get_root (json_parser);
	if (json_root == NULL) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_INVALID_FORMAT,
				     "no root");
		return NULL;
	}
	if (json_node_get_node_type (json_root) != JSON_NODE_ARRAY) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_INVALID_FORMAT,
				     "no array");
		return NULL;
	}

	/* parse each rating */
	reviews = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	json_reviews = json_node_get_array (json_root);
	reviewer_ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	for (i = 0; i < json_array_get_length (json_reviews); i++) {
		JsonNode *json_review;
		JsonObject *json_item;
		const gchar *reviewer_id;
		g_autoptr(AsReview) review = NULL;

		/* extract the data */
		json_review = json_array_get_element (json_reviews, i);
		if (json_node_get_node_type (json_review) != JSON_NODE_OBJECT) {
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_INVALID_FORMAT,
					     "no object type");
			return NULL;
		}
		json_item = json_node_get_object (json_review);
		if (json_item == NULL) {
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_INVALID_FORMAT,
					     "no object");
			return NULL;
		}

		/* create review */
		review = gs_plugin_odrs_parse_review_object (plugin,
							     json_item);

		reviewer_id = as_review_get_reviewer_id (review);
		if (reviewer_id == NULL)
			continue;

		/* dedupe each on the user_hash */
		if (g_hash_table_lookup (reviewer_ids, reviewer_id) != NULL) {
			g_debug ("duplicate review %s, skipping", reviewer_id);
			continue;
		}
		g_hash_table_add (reviewer_ids, g_strdup (reviewer_id));
		g_ptr_array_add (reviews, g_object_ref (review));
	}
	return g_steal_pointer (&reviews);
}

static gboolean
gs_plugin_odrs_parse_success (GsPlugin *plugin, const gchar *data, gssize data_len, GError **error)
{
	JsonNode *json_root;
	JsonObject *json_item;
	const gchar *msg = NULL;
	g_autoptr(JsonParser) json_parser = NULL;

	/* nothing */
	if (data == NULL) {
		if (!gs_plugin_get_network_available (plugin))
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_NO_NETWORK,
					     "server couldn't be reached");
		else
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_INVALID_FORMAT,
					     "server returned no data");
		return FALSE;
	}

	/* parse the data and find the success */
	json_parser = json_parser_new_immutable ();
	if (!json_parser_load_from_data (json_parser, data, data_len, error)) {
		gs_utils_error_convert_json_glib (error);
		return FALSE;
	}
	json_root = json_parser_get_root (json_parser);
	if (json_root == NULL) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_INVALID_FORMAT,
				     "no error root");
		return FALSE;
	}
	if (json_node_get_node_type (json_root) != JSON_NODE_OBJECT) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_INVALID_FORMAT,
				     "no error object");
		return FALSE;
	}
	json_item = json_node_get_object (json_root);
	if (json_item == NULL) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_INVALID_FORMAT,
				     "no error object");
		return FALSE;
	}

	/* failed? */
	if (json_object_has_member (json_item, "msg"))
		msg = json_object_get_string_member (json_item, "msg");
	if (!json_object_get_boolean_member (json_item, "success")) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_INVALID_FORMAT,
				     msg != NULL ? msg : "unknown failure");
		return FALSE;
	}

	/* just for the console */
	if (msg != NULL)
		g_debug ("success: %s", msg);
	return TRUE;
}

static gboolean
gs_plugin_odrs_json_post (GsPlugin *plugin,
			  SoupSession *session,
			  const gchar *uri,
			  const gchar *data,
			  GError **error)
{
	guint status_code;
	g_autoptr(SoupMessage) msg = NULL;

	/* create the GET data */
	g_debug ("Sending ODRS request to %s: %s", uri, data);
	msg = soup_message_new (SOUP_METHOD_POST, uri);
	soup_message_set_request (msg, "application/json; charset=utf-8",
				  SOUP_MEMORY_COPY, data, strlen (data));

	/* set sync request */
	status_code = soup_session_send_message (session, msg);
	g_debug ("ODRS server returned status %u: %s", status_code, msg->response_body->data);
	if (status_code != SOUP_STATUS_OK) {
		g_warning ("Failed to set rating on ODRS: %s",
			   soup_status_get_phrase (status_code));
		g_set_error (error,
                             GS_PLUGIN_ERROR,
                             GS_PLUGIN_ERROR_FAILED,
                             "Failed to submit review to ODRS: %s", soup_status_get_phrase (status_code));
		return FALSE;
	}

	/* process returned JSON */
	return gs_plugin_odrs_parse_success (plugin,
					     msg->response_body->data,
					     msg->response_body->length,
					     error);
}

static GPtrArray *
_gs_app_get_reviewable_ids (GsApp *app)
{
	GPtrArray *ids = g_ptr_array_new_with_free_func (g_free);
	GPtrArray *provided = gs_app_get_provided (app);

	/* add the main component id */
	g_ptr_array_add (ids, g_strdup (gs_app_get_id (app)));

	/* add any ID provides */
	for (guint i = 0; i < provided->len; i++) {
		GPtrArray *items;
		AsProvided *prov = g_ptr_array_index (provided, i);
		if (as_provided_get_kind (prov) != AS_PROVIDED_KIND_ID)
			continue;

		items = as_provided_get_items (prov);
		for (guint j = 0; j < items->len; j++) {
			const gchar *value = (const gchar *) g_ptr_array_index (items, j);
			if (value == NULL)
				continue;
			g_ptr_array_add (ids, g_strdup (value));
		}
	}
	return ids;
}

static gboolean
gs_plugin_odrs_refine_ratings (GsPlugin *plugin,
			       GsApp *app,
			       GCancellable *cancellable,
			       GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	gint rating;
	guint32 ratings_raw[6] = { 0, 0, 0, 0, 0, 0 };
	guint cnt = 0;
	g_autoptr(GArray) review_ratings = NULL;
	g_autoptr(GPtrArray) reviewable_ids = NULL;
	g_autoptr(GMutexLocker) locker = NULL;

	locker = g_mutex_locker_new (&priv->review_server_lock);
	if (priv->review_server == NULL || priv->review_server[0] == '\0')
		return TRUE;

	g_clear_pointer (&locker, g_mutex_locker_free);

	/* get ratings for each reviewable ID */
	reviewable_ids = _gs_app_get_reviewable_ids (app);

	locker = g_mutex_locker_new (&priv->ratings_mutex);

	if (!priv->ratings) {
		g_autofree gchar *cache_filename = NULL;

		g_clear_pointer (&locker, g_mutex_locker_free);

		/* Load from the local cache, if available, when in offline or
		   when refresh/download disabled on start */
		cache_filename = gs_utils_get_cache_filename ("odrs",
							      "ratings.json",
							      GS_UTILS_CACHE_FLAG_WRITEABLE,
							      error);

		if (!cache_filename ||
		    !gs_plugin_odrs_load_ratings (plugin, cache_filename, NULL))
			return TRUE;

		locker = g_mutex_locker_new (&priv->ratings_mutex);

		if (!priv->ratings)
			return TRUE;
	}

	for (guint i = 0; i < reviewable_ids->len; i++) {
		const gchar *id = g_ptr_array_index (reviewable_ids, i);
		const GsOdrsRating search_rating = { (gchar *) id, { 0, }};
		guint found_index;
		const GsOdrsRating *found_rating;

		if (!g_array_binary_search (priv->ratings, &search_rating,
					    (GCompareFunc) rating_compare, &found_index))
			continue;

		found_rating = &g_array_index (priv->ratings, GsOdrsRating, found_index);

		/* copy into accumulator array */
		for (guint j = 0; j < 6; j++)
			ratings_raw[j] += found_rating->n_star_ratings[j];
		cnt++;
	}
	if (cnt == 0)
		return TRUE;

	/* Done with priv->ratings now */
	g_clear_pointer (&locker, g_mutex_locker_free);

	/* merge to accumulator array back to one GArray blob */
	review_ratings = g_array_sized_new (FALSE, TRUE, sizeof(guint32), 6);
	for (guint i = 0; i < 6; i++)
		g_array_append_val (review_ratings, ratings_raw[i]);
	gs_app_set_review_ratings (app, review_ratings);

	/* find the wilson rating */
	rating = gs_utils_get_wilson_rating (g_array_index (review_ratings, guint32, 1),
					     g_array_index (review_ratings, guint32, 2),
					     g_array_index (review_ratings, guint32, 3),
					     g_array_index (review_ratings, guint32, 4),
					     g_array_index (review_ratings, guint32, 5));
	if (rating > 0)
		gs_app_set_rating (app, rating);
	return TRUE;
}

static JsonNode *
gs_plugin_odrs_get_compat_ids (GsApp *app)
{
	GPtrArray *provided = gs_app_get_provided (app);
	g_autoptr(GHashTable) ids = NULL;
	g_autoptr(JsonArray) json_array = json_array_new ();
	g_autoptr(JsonNode) json_node = json_node_new (JSON_NODE_ARRAY);

	ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
	for (guint i = 0; i < provided->len; i++) {
		GPtrArray *items;
		AsProvided *prov = g_ptr_array_index (provided, i);

		if (as_provided_get_kind (prov) != AS_PROVIDED_KIND_ID)
			continue;

		items = as_provided_get_items (prov);
		for (guint j = 0; j < items->len; j++) {
			const gchar *value = g_ptr_array_index (items, j);
			if (value == NULL)
				continue;

			if (g_hash_table_lookup (ids, value) != NULL)
				continue;
			g_hash_table_add (ids, g_strdup (value));
			json_array_add_string_element (json_array, value);
		}
	}
	if (json_array_get_length (json_array) == 0)
		return NULL;
	json_node_set_array (json_node, json_array);
	return g_steal_pointer (&json_node);
}

static GPtrArray *
gs_plugin_odrs_fetch_for_app (GsPlugin *plugin, GsApp *app, GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	JsonNode *json_compat_ids;
	const gchar *version;
	guint status_code;
	g_autofree gchar *cachefn_basename = NULL;
	g_autofree gchar *cachefn = NULL;
	g_autofree gchar *data = NULL;
	g_autofree gchar *uri = NULL;
	g_autoptr(GFile) cachefn_file = NULL;
	g_autoptr(GPtrArray) reviews = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonGenerator) json_generator = NULL;
	g_autoptr(JsonNode) json_root = NULL;
	g_autoptr(SoupMessage) msg = NULL;
	g_autoptr(GMutexLocker) locker = NULL;

	locker = g_mutex_locker_new (&priv->review_server_lock);

	if (priv->review_server == NULL || priv->review_server[0] == '\0')
		return g_ptr_array_new ();

	/* look in the cache */
	cachefn_basename = g_strdup_printf ("%s.json", gs_app_get_id (app));
	cachefn = gs_utils_get_cache_filename ("odrs",
					       cachefn_basename,
					       GS_UTILS_CACHE_FLAG_WRITEABLE,
					       error);
	if (cachefn == NULL)
		return NULL;
	cachefn_file = g_file_new_for_path (cachefn);
	if (gs_utils_get_file_age (cachefn_file) < ODRS_REVIEW_CACHE_AGE_MAX) {
		g_autoptr(GMappedFile) mapped_file = NULL;

		mapped_file = g_mapped_file_new (cachefn, FALSE, error);
		if (mapped_file == NULL)
			return NULL;

		g_clear_pointer (&locker, g_mutex_locker_free);

		g_debug ("got review data for %s from %s",
			 gs_app_get_id (app), cachefn);
		return gs_plugin_odrs_parse_reviews (plugin,
						     g_mapped_file_get_contents (mapped_file),
						     g_mapped_file_get_length (mapped_file),
						     error);
	}

	/* not always available */
	version = gs_app_get_version (app);
	if (version == NULL)
		version = "unknown";

	/* create object with review data */
	builder = json_builder_new ();
	json_builder_begin_object (builder);
	json_builder_set_member_name (builder, "user_hash");
	json_builder_add_string_value (builder, priv->user_hash);
	json_builder_set_member_name (builder, "app_id");
	json_builder_add_string_value (builder, gs_app_get_id (app));
	json_builder_set_member_name (builder, "locale");
	json_builder_add_string_value (builder, gs_plugin_get_locale (plugin));
	json_builder_set_member_name (builder, "distro");
	json_builder_add_string_value (builder, priv->distro);
	json_builder_set_member_name (builder, "version");
	json_builder_add_string_value (builder, version);
	json_builder_set_member_name (builder, "limit");
	json_builder_add_int_value (builder, ODRS_REVIEW_NUMBER_RESULTS_MAX);
	json_compat_ids = gs_plugin_odrs_get_compat_ids (app);
	if (json_compat_ids != NULL) {
		json_builder_set_member_name (builder, "compat_ids");
		json_builder_add_value (builder, json_compat_ids);
	}
	json_builder_end_object (builder);

	/* export as a string */
	json_root = json_builder_get_root (builder);
	json_generator = json_generator_new ();
	json_generator_set_pretty (json_generator, TRUE);
	json_generator_set_root (json_generator, json_root);
	data = json_generator_to_data (json_generator, NULL);
	if (data == NULL)
		return NULL;
	uri = g_strdup_printf ("%s/fetch", priv->review_server);
	g_debug ("Updating ODRS cache for %s from %s to %s; request %s", gs_app_get_id (app),
		 uri, cachefn, data);

	g_clear_pointer (&locker, g_mutex_locker_free);

	msg = soup_message_new (SOUP_METHOD_POST, uri);
	soup_message_set_request (msg, "application/json; charset=utf-8",
				  SOUP_MEMORY_COPY, data, strlen (data));
	status_code = soup_session_send_message (gs_plugin_get_soup_session (plugin), msg);
	if (status_code != SOUP_STATUS_OK) {
		if (!gs_plugin_odrs_parse_success (plugin,
						   msg->response_body->data,
						   msg->response_body->length,
						   error))
			return NULL;
		/* not sure what to do here */
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_DOWNLOAD_FAILED,
				     "status code invalid");
		gs_utils_error_add_origin_id (error, priv->cached_origin);
		return NULL;
	}
	reviews = gs_plugin_odrs_parse_reviews (plugin,
						msg->response_body->data,
						msg->response_body->length,
						error);
	if (reviews == NULL)
		return NULL;

	/* save to the cache */
	if (!g_file_set_contents (cachefn,
				  msg->response_body->data,
				  msg->response_body->length,
				  error))
		return NULL;

	/* success */
	return g_steal_pointer (&reviews);
}

static gboolean
gs_plugin_odrs_refine_reviews (GsPlugin *plugin,
			       GsApp *app,
			       GCancellable *cancellable,
			       GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	AsReview *review;
	g_autoptr(GPtrArray) reviews = NULL;

	/* get from server */
	reviews = gs_plugin_odrs_fetch_for_app (plugin, app, error);
	if (reviews == NULL)
		return FALSE;
	for (guint i = 0; i < reviews->len; i++) {
		review = g_ptr_array_index (reviews, i);

		/* save this on the application object so we can use it for
		 * submitting a new review */
		if (i == 0) {
			gs_app_set_metadata (app, "ODRS::user_skey",
					     as_review_get_metadata_item (review, "user_skey"));
		}

		/* ignore invalid reviews */
		if (as_review_get_rating (review) == 0)
			continue;

		/* the user_hash matches, so mark this as our own review */
		if (g_strcmp0 (as_review_get_reviewer_id (review),
			       priv->user_hash) == 0) {
			as_review_set_flags (review, AS_REVIEW_FLAG_SELF);
		}
		gs_app_add_review (app, review);
	}
	return TRUE;
}

static gboolean
refine_app (GsPlugin             *plugin,
	    GsApp                *app,
	    GsPluginRefineFlags   flags,
	    GCancellable         *cancellable,
	    GError              **error)
{
	/* not valid */
	if (gs_app_get_kind (app) == AS_COMPONENT_KIND_ADDON)
		return TRUE;
	if (gs_app_get_id (app) == NULL)
		return TRUE;

	/* add reviews if possible */
	if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEWS) {
		if (gs_app_get_reviews(app)->len > 0)
			return TRUE;
		if (!gs_plugin_odrs_refine_reviews (plugin, app,
						    cancellable, error))
			return FALSE;
	}

	/* add ratings if possible */
	if (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEW_RATINGS ||
	    flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING) {
		if (gs_app_get_review_ratings(app) != NULL)
			return TRUE;
		if (!gs_plugin_odrs_refine_ratings (plugin, app,
						    cancellable, error))
			return FALSE;
	}

	return TRUE;
}

gboolean
gs_plugin_refine (GsPlugin             *plugin,
		  GsAppList            *list,
		  GsPluginRefineFlags   flags,
		  GCancellable         *cancellable,
		  GError              **error)
{
	/* nothing to do here */
	if ((flags & (GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEWS |
		      GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEW_RATINGS |
		      GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING)) == 0)
		return TRUE;

	for (guint i = 0; i < gs_app_list_length (list); i++) {
		GsApp *app = gs_app_list_index (list, i);
		g_autoptr(GError) local_error = NULL;
		if (!refine_app (plugin, app, flags, cancellable, &local_error)) {
			if (g_error_matches (local_error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NO_NETWORK)) {
				g_debug ("failed to refine app %s: %s",
					 gs_app_get_unique_id (app), local_error->message);
			} else {
				g_prefix_error (&local_error, "failed to refine app: ");
				g_propagate_error (error, g_steal_pointer (&local_error));
				return FALSE;
			}
		}
	}

	return TRUE;
}

static gchar *
gs_plugin_odrs_sanitize_version (const gchar *version)
{
	gchar *str;
	gchar *tmp;

	/* nothing set */
	if (version == NULL)
		return g_strdup ("unknown");

	/* remove epoch */
	str = g_strrstr (version, ":");
	if (str != NULL)
		version = str + 1;

	/* remove release */
	tmp = g_strdup (version);
	g_strdelimit (tmp, "-", '\0');

	/* remove '+dfsg' suffix */
	str = g_strstr_len (tmp, -1, "+dfsg");
	if (str != NULL)
		*str = '\0';

	return tmp;
}

static gboolean
gs_plugin_odrs_invalidate_cache (AsReview *review, GError **error)
{
	g_autofree gchar *cachefn_basename = NULL;
	g_autofree gchar *cachefn = NULL;
	g_autoptr(GFile) cachefn_file = NULL;

	/* look in the cache */
	cachefn_basename = g_strdup_printf ("%s.json",
					    as_review_get_metadata_item (review, "app_id"));
	cachefn = gs_utils_get_cache_filename ("odrs",
					       cachefn_basename,
					       GS_UTILS_CACHE_FLAG_WRITEABLE,
					       error);
	if (cachefn == NULL)
		return FALSE;
	cachefn_file = g_file_new_for_path (cachefn);
	if (!g_file_query_exists (cachefn_file, NULL))
		return TRUE;
	return g_file_delete (cachefn_file, NULL, error);
}

gboolean
gs_plugin_review_submit (GsPlugin *plugin,
			 GsApp *app,
			 AsReview *review,
			 GCancellable *cancellable,
			 GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	g_autofree gchar *data = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *version = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonGenerator) json_generator = NULL;
	g_autoptr(JsonNode) json_root = NULL;
	g_autoptr(GMutexLocker) locker = NULL;

	locker = g_mutex_locker_new (&priv->review_server_lock);

	if (priv->review_server == NULL || priv->review_server[0] == '\0') {
		g_set_error_literal (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NOT_SUPPORTED,
			"The ODRS plugin is disabled");
		return FALSE;
	}

	/* save as we don't re-request the review from the server */
	as_review_add_flags (review, AS_REVIEW_FLAG_SELF);
	as_review_set_reviewer_name (review, g_get_real_name ());
	as_review_add_metadata (review, "app_id", gs_app_get_id (app));
	as_review_add_metadata (review, "user_skey",
				gs_app_get_metadata_item (app, "ODRS::user_skey"));

	/* create object with review data */
	builder = json_builder_new ();
	json_builder_begin_object (builder);
	json_builder_set_member_name (builder, "user_hash");
	json_builder_add_string_value (builder, priv->user_hash);
	json_builder_set_member_name (builder, "user_skey");
	json_builder_add_string_value (builder,
				       as_review_get_metadata_item (review, "user_skey"));
	json_builder_set_member_name (builder, "app_id");
	json_builder_add_string_value (builder,
				       as_review_get_metadata_item (review, "app_id"));
	json_builder_set_member_name (builder, "locale");
	json_builder_add_string_value (builder, gs_plugin_get_locale (plugin));
	json_builder_set_member_name (builder, "distro");
	json_builder_add_string_value (builder, priv->distro);
	json_builder_set_member_name (builder, "version");
	version = gs_plugin_odrs_sanitize_version (as_review_get_version (review));
	json_builder_add_string_value (builder, version);
	json_builder_set_member_name (builder, "user_display");
	json_builder_add_string_value (builder, as_review_get_reviewer_name (review));
	json_builder_set_member_name (builder, "summary");
	json_builder_add_string_value (builder, as_review_get_summary (review));
	json_builder_set_member_name (builder, "description");
	json_builder_add_string_value (builder, as_review_get_description (review));
	json_builder_set_member_name (builder, "rating");
	json_builder_add_int_value (builder, as_review_get_rating (review));
	json_builder_end_object (builder);

	/* export as a string */
	json_root = json_builder_get_root (builder);
	json_generator = json_generator_new ();
	json_generator_set_pretty (json_generator, TRUE);
	json_generator_set_root (json_generator, json_root);
	data = json_generator_to_data (json_generator, NULL);

	/* clear cache */
	if (!gs_plugin_odrs_invalidate_cache (review, error))
		return FALSE;

	/* POST */
	uri = g_strdup_printf ("%s/submit", priv->review_server);
	g_clear_pointer (&locker, g_mutex_locker_free);
	return gs_plugin_odrs_json_post (plugin, gs_plugin_get_soup_session (plugin),
						    uri, data, error);
}

static gboolean
gs_plugin_odrs_vote (GsPlugin *plugin,
		     AsReview *review,
		     const gchar *path,
		     GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	const gchar *tmp;
	g_autofree gchar *data = NULL;
	g_autofree gchar *uri = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonGenerator) json_generator = NULL;
	g_autoptr(JsonNode) json_root = NULL;
	g_autoptr(GMutexLocker) locker = NULL;

	locker = g_mutex_locker_new (&priv->review_server_lock);

	if (priv->review_server == NULL || priv->review_server[0] == '\0') {
		g_set_error_literal (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NOT_SUPPORTED,
			"The ODRS plugin is disabled");
		return FALSE;
	}

	uri = g_strdup_printf ("%s/%s", priv->review_server, path);

	g_clear_pointer (&locker, g_mutex_locker_free);

	/* create object with vote data */
	builder = json_builder_new ();
	json_builder_begin_object (builder);

	json_builder_set_member_name (builder, "user_hash");
	json_builder_add_string_value (builder, priv->user_hash);
	json_builder_set_member_name (builder, "user_skey");
	json_builder_add_string_value (builder,
				       as_review_get_metadata_item (review, "user_skey"));
	json_builder_set_member_name (builder, "app_id");
	json_builder_add_string_value (builder,
				       as_review_get_metadata_item (review, "app_id"));
	tmp = as_review_get_id (review);
	if (tmp != NULL) {
		gint64 review_id;
		json_builder_set_member_name (builder, "review_id");
		review_id = g_ascii_strtoll (tmp, NULL, 10);
		json_builder_add_int_value (builder, review_id);
	}
	json_builder_end_object (builder);

	/* export as a string */
	json_root = json_builder_get_root (builder);
	json_generator = json_generator_new ();
	json_generator_set_pretty (json_generator, TRUE);
	json_generator_set_root (json_generator, json_root);
	data = json_generator_to_data (json_generator, NULL);
	if (data == NULL)
		return FALSE;

	/* clear cache */
	if (!gs_plugin_odrs_invalidate_cache (review, error))
		return FALSE;

	/* send to server */
	if (!gs_plugin_odrs_json_post (plugin, gs_plugin_get_soup_session (plugin),
						  uri, data, error))
		return FALSE;

	/* mark as voted */
	as_review_add_flags (review, AS_REVIEW_FLAG_VOTED);

	/* success */
	return TRUE;
}

gboolean
gs_plugin_review_report (GsPlugin *plugin,
			 GsApp *app,
			 AsReview *review,
			 GCancellable *cancellable,
			 GError **error)
{
	return gs_plugin_odrs_vote (plugin, review, "report", error);
}

gboolean
gs_plugin_review_upvote (GsPlugin *plugin,
			 GsApp *app,
			 AsReview *review,
			 GCancellable *cancellable,
			 GError **error)
{
	return gs_plugin_odrs_vote (plugin, review, "upvote", error);
}

gboolean
gs_plugin_review_downvote (GsPlugin *plugin,
			   GsApp *app,
			   AsReview *review,
			   GCancellable *cancellable,
			   GError **error)
{
	return gs_plugin_odrs_vote (plugin, review, "downvote", error);
}

gboolean
gs_plugin_review_dismiss (GsPlugin *plugin,
			  GsApp *app,
			  AsReview *review,
			  GCancellable *cancellable,
			  GError **error)
{
	return gs_plugin_odrs_vote (plugin, review, "dismiss", error);
}

gboolean
gs_plugin_review_remove (GsPlugin *plugin,
			 GsApp *app,
			 AsReview *review,
			 GCancellable *cancellable,
			 GError **error)
{
	return gs_plugin_odrs_vote (plugin, review, "remove", error);
}

static GsApp *
gs_plugin_create_app_dummy (const gchar *id)
{
	GsApp *app = gs_app_new (id);
	g_autoptr(GString) str = NULL;
	str = g_string_new (id);
	as_gstring_replace (str, ".desktop", "");
	g_string_prepend (str, "No description is available for ");
	gs_app_set_name (app, GS_APP_QUALITY_LOWEST, "Unknown Application");
	gs_app_set_summary (app, GS_APP_QUALITY_LOWEST, "Application not found");
	gs_app_set_description (app, GS_APP_QUALITY_LOWEST, str->str);
	return app;
}

gboolean
gs_plugin_add_unvoted_reviews (GsPlugin *plugin,
			       GsAppList *list,
			       GCancellable *cancellable,
			       GError **error)
{
	GsPluginData *priv = gs_plugin_get_data (plugin);
	guint status_code;
	guint i;
	g_autofree gchar *uri = NULL;
	g_autoptr(GHashTable) hash = NULL;
	g_autoptr(GPtrArray) reviews = NULL;
	g_autoptr(SoupMessage) msg = NULL;

	g_autoptr(GMutexLocker) locker = NULL;

	locker = g_mutex_locker_new (&priv->review_server_lock);

	if (priv->review_server == NULL || priv->review_server[0] == '\0') {
		g_set_error_literal (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_NOT_SUPPORTED,
			"The ODRS plugin is disabled");
		return FALSE;
	}

	/* create the GET data *with* the machine hash so we can later
	 * review the application ourselves */
	uri = g_strdup_printf ("%s/moderate/%s/%s",
			       priv->review_server,
			       priv->user_hash,
			       gs_plugin_get_locale (plugin));

	g_clear_pointer (&locker, g_mutex_locker_free);

	msg = soup_message_new (SOUP_METHOD_GET, uri);
	status_code = soup_session_send_message (gs_plugin_get_soup_session (plugin), msg);
	if (status_code != SOUP_STATUS_OK) {
		if (!gs_plugin_odrs_parse_success (plugin,
						   msg->response_body->data,
						   msg->response_body->length,
						   error))
			return FALSE;
		/* not sure what to do here */
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_DOWNLOAD_FAILED,
				     "status code invalid");
		gs_utils_error_add_origin_id (error, priv->cached_origin);
		return FALSE;
	}
	g_debug ("odrs returned: %s", msg->response_body->data);
	reviews = gs_plugin_odrs_parse_reviews (plugin,
						msg->response_body->data,
						msg->response_body->length,
						error);
	if (reviews == NULL)
		return FALSE;

	/* look at all the reviews; faking application objects */
	hash = g_hash_table_new_full (g_str_hash, g_str_equal,
				      g_free, (GDestroyNotify) g_object_unref);
	for (i = 0; i < reviews->len; i++) {
		GsApp *app;
		AsReview *review;
		const gchar *app_id;

		/* same app? */
		review = g_ptr_array_index (reviews, i);
		app_id = as_review_get_metadata_item (review, "app_id");
		app = g_hash_table_lookup (hash, app_id);
		if (app == NULL) {
			app = gs_plugin_create_app_dummy (app_id);
			gs_app_list_add (list, app);
			g_hash_table_insert (hash, g_strdup (app_id), app);
		}
		gs_app_add_review (app, review);
	}

	return TRUE;
}
