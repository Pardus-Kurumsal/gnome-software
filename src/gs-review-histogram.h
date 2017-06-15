/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Canonical Ltd.
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

#ifndef GS_REVIEW_HISTOGRAM_H
#define GS_REVIEW_HISTOGRAM_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GS_TYPE_REVIEW_HISTOGRAM (gs_review_histogram_get_type ())

G_DECLARE_DERIVABLE_TYPE (GsReviewHistogram, gs_review_histogram, GS, REVIEW_HISTOGRAM, GtkBin)

struct _GsReviewHistogramClass
{
	GtkBinClass	 parent_class;
};

GtkWidget	*gs_review_histogram_new			(void);

void		 gs_review_histogram_set_ratings		(GsReviewHistogram *histogram,
								 GArray *review_ratings);

G_END_DECLS

#endif /* GS_REVIEW_HISTOGRAM_H */

/* vim: set noexpandtab: */
