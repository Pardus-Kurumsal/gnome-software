/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013-2016 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
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

#ifndef __GS_CATEGORY_PRIVATE_H
#define __GS_CATEGORY_PRIVATE_H

#include "gs-category.h"

G_BEGIN_DECLS

void		 gs_category_sort_children	(GsCategory	*category);
void		 gs_category_set_size		(GsCategory	*category,
						 guint		 size);
gchar		*gs_category_to_string		(GsCategory	*category);

G_END_DECLS

#endif /* __GS_CATEGORY_PRIVATE_H */

/* vim: set noexpandtab: */
