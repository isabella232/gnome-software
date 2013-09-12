/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007-2013 Richard Hughes <richard@hughsie.com>
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

#ifndef __GS_PLUGIN_LOADER_H
#define __GS_PLUGIN_LOADER_H

#include <glib-object.h>

#include "gs-app.h"
#include "gs-category.h"
#include "gs-plugin.h"

G_BEGIN_DECLS

#define GS_TYPE_PLUGIN_LOADER		(gs_plugin_loader_get_type ())
#define GS_PLUGIN_LOADER(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), GS_TYPE_PLUGIN_LOADER, GsPluginLoader))
#define GS_PLUGIN_LOADER_CLASS(k)	(G_TYPE_CHECK_CLASS_CAST((k), GS_TYPE_PLUGIN_LOADER, GsPluginLoaderClass))
#define GS_IS_PLUGIN_LOADER(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), GS_TYPE_PLUGIN_LOADER))
#define GS_IS_PLUGIN_LOADER_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), GS_TYPE_PLUGIN_LOADER))
#define GS_PLUGIN_LOADER_GET_CLASS(o)	(G_TYPE_INSTANCE_GET_CLASS ((o), GS_TYPE_PLUGIN_LOADER, GsPluginLoaderClass))
#define GS_PLUGIN_LOADER_ERROR		(gs_plugin_loader_error_quark ())

typedef struct GsPluginLoaderPrivate GsPluginLoaderPrivate;

typedef struct
{
	 GObject		 parent;
	 GsPluginLoaderPrivate	*priv;
} GsPluginLoader;

typedef struct
{
	GObjectClass		 parent_class;
	void			(*status_changed)	(GsPluginLoader	*plugin_loader,
							 GsApp		*app,
							 GsPluginStatus	 status);
	void			(*pending_apps_changed)	(GsPluginLoader	*plugin_loader);
} GsPluginLoaderClass;

typedef enum
{
	GS_PLUGIN_LOADER_ERROR_FAILED,
	GS_PLUGIN_LOADER_ERROR_LAST
} GsPluginLoaderError;

typedef enum {
	GS_PLUGIN_LOADER_FLAGS_NONE = 0,
	GS_PLUGIN_LOADER_FLAGS_USE_HISTORY = 1,
	GS_PLUGIN_LOADER_FLAGS_LAST
} GsPluginLoaderFlags;

typedef void	 (*GsPluginLoaderFinishedFunc)		(GsPluginLoader	*plugin_loader,
							 GsApp		*app,
							 gpointer	 user_data);

GQuark		 gs_plugin_loader_error_quark		(void);
GType		 gs_plugin_loader_get_type		(void);

GsPluginLoader	*gs_plugin_loader_new			(void);
void		 gs_plugin_loader_get_installed_async	(GsPluginLoader	*plugin_loader,
							 GsPluginLoaderFlags flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GList		*gs_plugin_loader_get_installed_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_get_updates_async	(GsPluginLoader	*plugin_loader,
							 GsPluginLoaderFlags flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GList		*gs_plugin_loader_get_updates_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_get_popular_async	(GsPluginLoader	*plugin_loader,
							 GsPluginLoaderFlags flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GList		*gs_plugin_loader_get_popular_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_get_featured_async	(GsPluginLoader	*plugin_loader,
							 GsPluginLoaderFlags flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GList		*gs_plugin_loader_get_featured_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_get_categories_async	(GsPluginLoader	*plugin_loader,
							 GsPluginLoaderFlags flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GList		*gs_plugin_loader_get_categories_finish	(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_get_category_apps_async (GsPluginLoader	*plugin_loader,
							 GsCategory	*category,
							 GsPluginLoaderFlags flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GList		*gs_plugin_loader_get_category_apps_finish (GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
void		 gs_plugin_loader_search_async		(GsPluginLoader	*plugin_loader,
							 const gchar	*value,
							 GsPluginLoaderFlags flags,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
GList		*gs_plugin_loader_search_finish		(GsPluginLoader	*plugin_loader,
							 GAsyncResult	*res,
							 GError		**error);
gboolean	 gs_plugin_loader_setup			(GsPluginLoader	*plugin_loader,
							 GError		**error);
void		 gs_plugin_loader_dump_state		(GsPluginLoader	*plugin_loader);
gboolean	 gs_plugin_loader_set_enabled		(GsPluginLoader	*plugin_loader,
							 const gchar	*plugin_name,
							 gboolean	 enabled);
void		 gs_plugin_loader_set_location		(GsPluginLoader	*plugin_loader,
							 const gchar	*location);
gboolean	 gs_plugin_loader_app_refine		(GsPluginLoader	*plugin_loader,
							 GsApp		*app,
							 GsPluginLoaderFlags flags,
							 GCancellable	*cancellable,
							 GError		**error);
void		 gs_plugin_loader_app_install		(GsPluginLoader	*plugin_loader,
							 GsApp		*app,
							 GsPluginLoaderFlags flags,
							 GCancellable	*cancellable,
							 GsPluginLoaderFinishedFunc func,
							 gpointer	 user_data);
void		 gs_plugin_loader_app_remove		(GsPluginLoader	*plugin_loader,
							 GsApp		*app,
							 GsPluginLoaderFlags flags,
							 GCancellable	*cancellable,
							 GsPluginLoaderFinishedFunc func,
							 gpointer	 user_data);
gboolean	 gs_plugin_loader_app_set_rating	(GsPluginLoader	*plugin_loader,
							 GsApp		*app,
							 GsPluginLoaderFlags flags,
							 GCancellable	*cancellable,
							 GError		**error);
GsAppState	 gs_plugin_loader_get_state_for_app	(GsPluginLoader	*plugin_loader,
							 GsApp		*app);
GPtrArray	*gs_plugin_loader_get_pending		(GsPluginLoader	*plugin_loader);
GsApp		*gs_plugin_loader_dedupe		(GsPluginLoader	*plugin_loader,
							 GsApp		*app);

G_END_DECLS

#endif /* __GS_PLUGIN_LOADER_H */

/* vim: set noexpandtab: */
