/*
 * Geeqie
 * (C) 2004 John Ellis
 * Copyright (C) 2008 The Geeqie Team
 *
 * Author: John Ellis
 *
 * This software is released under the GNU General Public License (GNU GPL).
 * Please read the included file COPYING for more information.
 * This software comes with no warranty of any kind, use at your own risk!
 */

#include "main.h"
#include "view_file_list.h"

#include "cache_maint.h"
#include "dnd.h"
#include "editors.h"
#include "img-view.h"
#include "info.h"
#include "layout.h"
#include "layout_image.h"
#include "menu.h"
#include "thumb.h"
#include "utilops.h"
#include "ui_bookmark.h"
#include "ui_fileops.h"
#include "ui_menu.h"
#include "ui_tree_edit.h"
#include "view_file.h"

#include <gdk/gdkkeysyms.h> /* for keyboard values */


enum {
	FILE_COLUMN_POINTER = 0,
	FILE_COLUMN_VERSION,
	FILE_COLUMN_THUMB,
	FILE_COLUMN_NAME,
	FILE_COLUMN_SIDECARS,
	FILE_COLUMN_SIZE,
	FILE_COLUMN_DATE,
	FILE_COLUMN_COLOR,
	FILE_COLUMN_MARKS,
	FILE_COLUMN_MARKS_LAST = FILE_COLUMN_MARKS + FILEDATA_MARKS_SIZE - 1,
	FILE_COLUMN_COUNT
};


static gint vflist_row_is_selected(ViewFile *vf, FileData *fd);
static gint vflist_row_rename_cb(TreeEditData *td, const gchar *old, const gchar *new, gpointer data);
static void vflist_populate_view(ViewFile *vf);
static void vflist_notify_cb(FileData *fd, gpointer data);


/*
 *-----------------------------------------------------------------------------
 * misc
 *-----------------------------------------------------------------------------
 */
typedef struct {
	FileData *fd;
	GtkTreeIter *iter;
	gint found;
	gint row;
} ViewFileFindRowData;

static gboolean vflist_find_row_cb(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	ViewFileFindRowData *find = data;
	FileData *fd;
	gtk_tree_model_get(model, iter, FILE_COLUMN_POINTER, &fd, -1);
	if (fd == find->fd)
		{
		*find->iter = *iter;
		find->found = 1;
		return TRUE;
		}
	find->row++;
	return FALSE;
}

static gint vflist_find_row(ViewFile *vf, FileData *fd, GtkTreeIter *iter)
{
	GtkTreeModel *store;
	ViewFileFindRowData data = {fd, iter, 0, 0};

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview));
	gtk_tree_model_foreach(store, vflist_find_row_cb, &data);

	if (data.found)
		{
		return data.row;
		}

	return -1;
}


/*
static gint vflist_find_sidecar_list_idx(GList *work, FileData *fd)
{
	gint i = 0;
	while (work)
		{
		FileData *fd_p = work->data;
		if (fd == fd_p) return i;

		i++;

		GList *work2 = fd_p->sidecar_files;
		while (work2)
			{
			fd_p = work2->data;
			if (fd == fd_p) return i;

			i++;
			work2 = work2->next;
			}
		work = work->next;
		}
	return -1;
}
*/

static gint vflist_sidecar_list_count(GList *work)
{
	gint i = 0;
	while (work)
		{
		FileData *fd = work->data;
		i++;

		GList *work2 = fd->sidecar_files;
		while (work2)
			{
			i++;
			work2 = work2->next;
			}
		work = work->next;
		}
	return i;
}


void vflist_color_set(ViewFile *vf, FileData *fd, gint color_set)
{
	GtkTreeModel *store;
	GtkTreeIter iter;

	if (vflist_find_row(vf, fd, &iter) < 0) return;
	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview));
	gtk_tree_store_set(GTK_TREE_STORE(store), &iter, FILE_COLUMN_COLOR, color_set, -1);
}

static void vflist_move_cursor(ViewFile *vf, GtkTreeIter *iter)
{
	GtkTreeModel *store;
	GtkTreePath *tpath;

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview));

	tpath = gtk_tree_model_get_path(store, iter);
	gtk_tree_view_set_cursor(GTK_TREE_VIEW(vf->listview), tpath, NULL, FALSE);
	gtk_tree_path_free(tpath);
}


static gint vflist_column_idx(ViewFile *vf, gint store_idx)
{
	GList *columns, *work;
	gint i = 0;

	columns = gtk_tree_view_get_columns(GTK_TREE_VIEW(vf->listview));
	work = columns;
	while (work)
		{
		GtkTreeViewColumn *column = work->data;
		if (store_idx == GPOINTER_TO_INT(g_object_get_data (G_OBJECT(column), "column_store_idx")))
			break;
		work = work->next;
		i++;
		}

	g_list_free(columns);
	return i;
}


/*
 *-----------------------------------------------------------------------------
 * dnd
 *-----------------------------------------------------------------------------
 */

static void vflist_dnd_get(GtkWidget *widget, GdkDragContext *context,
			   GtkSelectionData *selection_data, guint info,
			   guint time, gpointer data)
{
	ViewFile *vf = data;
	GList *list = NULL;
	gchar *uri_text = NULL;
	gint total;

	if (!VFLIST_INFO(vf, click_fd)) return;

	if (vflist_row_is_selected(vf, VFLIST_INFO(vf, click_fd)))
		{
		list = vf_selection_get_list(vf);
		}
	else
		{
		list = g_list_append(NULL, file_data_ref(VFLIST_INFO(vf, click_fd)));
		}

	if (!list) return;

	uri_text = uri_text_from_filelist(list, &total, (info == TARGET_TEXT_PLAIN));
	filelist_free(list);

	DEBUG_1(uri_text);

	gtk_selection_data_set(selection_data, selection_data->target,
			       8, (guchar *)uri_text, total);
	g_free(uri_text);
}

static void vflist_dnd_begin(GtkWidget *widget, GdkDragContext *context, gpointer data)
{
	ViewFile *vf = data;

	vflist_color_set(vf, VFLIST_INFO(vf, click_fd), TRUE);

	if (VFLIST_INFO(vf, thumbs_enabled) &&
	    VFLIST_INFO(vf, click_fd) && VFLIST_INFO(vf, click_fd)->pixbuf)
		{
		guint items;

		if (vflist_row_is_selected(vf, VFLIST_INFO(vf, click_fd)))
			items = vf_selection_count(vf, NULL);
		else
			items = 1;

		dnd_set_drag_icon(widget, context, VFLIST_INFO(vf, click_fd)->pixbuf, items);
		}
}

static void vflist_dnd_end(GtkWidget *widget, GdkDragContext *context, gpointer data)
{
	ViewFile *vf = data;

	vflist_color_set(vf, VFLIST_INFO(vf, click_fd), FALSE);

	if (context->action == GDK_ACTION_MOVE)
		{
		vf_refresh(vf);
		}
}

void vflist_dnd_init(ViewFile *vf)
{
	gtk_drag_source_set(vf->listview, GDK_BUTTON1_MASK | GDK_BUTTON2_MASK,
			    dnd_file_drag_types, dnd_file_drag_types_count,
			    GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK);
	g_signal_connect(G_OBJECT(vf->listview), "drag_data_get",
			 G_CALLBACK(vflist_dnd_get), vf);
	g_signal_connect(G_OBJECT(vf->listview), "drag_begin",
			 G_CALLBACK(vflist_dnd_begin), vf);
	g_signal_connect(G_OBJECT(vf->listview), "drag_end",
			 G_CALLBACK(vflist_dnd_end), vf);
}

/*
 *-----------------------------------------------------------------------------
 * pop-up menu
 *-----------------------------------------------------------------------------
 */

GList *vflist_pop_menu_file_list(ViewFile *vf)
{
	if (!VFLIST_INFO(vf, click_fd)) return NULL;

	if (vflist_row_is_selected(vf, VFLIST_INFO(vf, click_fd)))
		{
		return vf_selection_get_list(vf);
		}

	return g_list_append(NULL, file_data_ref(VFLIST_INFO(vf, click_fd)));
}

void vflist_pop_menu_view_cb(GtkWidget *widget, gpointer data)
{
	ViewFile *vf = data;

	if (vflist_row_is_selected(vf, VFLIST_INFO(vf, click_fd)))
		{
		GList *list;

		list = vf_selection_get_list(vf);
		view_window_new_from_list(list);
		filelist_free(list);
		}
	else
		{
		view_window_new(VFLIST_INFO(vf, click_fd));
		}
}

void vflist_pop_menu_rename_cb(GtkWidget *widget, gpointer data)
{
	ViewFile *vf = data;
	GList *list;

	list = vf_pop_menu_file_list(vf);
	if (options->file_ops.enable_in_place_rename &&
	    list && !list->next && VFLIST_INFO(vf, click_fd))
		{
		GtkTreeModel *store;
		GtkTreeIter iter;

		filelist_free(list);

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview));
		if (vflist_find_row(vf, VFLIST_INFO(vf, click_fd), &iter) >= 0)
			{
			GtkTreePath *tpath;

			tpath = gtk_tree_model_get_path(store, &iter);
			tree_edit_by_path(GTK_TREE_VIEW(vf->listview), tpath,
					  vflist_column_idx(vf, FILE_COLUMN_NAME), VFLIST_INFO(vf, click_fd)->name,
					  vflist_row_rename_cb, vf);
			gtk_tree_path_free(tpath);
			}
		return;
		}

	file_util_rename(NULL, list, vf->listview);
}

void vflist_pop_menu_thumbs_cb(GtkWidget *widget, gpointer data)
{
	ViewFile *vf = data;

	vflist_color_set(vf, VFLIST_INFO(vf, click_fd), FALSE);
	if (vf->layout)
		{
		layout_thumb_set(vf->layout, !VFLIST_INFO(vf, thumbs_enabled));
		}
	else
		{
		vflist_thumb_set(vf, !VFLIST_INFO(vf, thumbs_enabled));
		}
}

void vflist_pop_menu_refresh_cb(GtkWidget *widget, gpointer data)
{
	ViewFile *vf = data;

	vflist_color_set(vf, VFLIST_INFO(vf, click_fd), FALSE);
	vf_refresh(vf);
}

void vflist_popup_destroy_cb(GtkWidget *widget, gpointer data)
{
	ViewFile *vf = data;
	vflist_color_set(vf, VFLIST_INFO(vf, click_fd), FALSE);
	VFLIST_INFO(vf, click_fd) = NULL;
	vf->popup = NULL;
}


/*
 *-----------------------------------------------------------------------------
 * callbacks
 *-----------------------------------------------------------------------------
 */

static gint vflist_row_rename_cb(TreeEditData *td, const gchar *old, const gchar *new, gpointer data)
{
	ViewFile *vf = data;
	gchar *old_path;
	gchar *new_path;

	if (strlen(new) == 0) return FALSE;

	old_path = g_build_filename(vf->dir_fd->path, old, NULL);
	new_path = g_build_filename(vf->dir_fd->path, new, NULL);

	if (strchr(new, G_DIR_SEPARATOR) != NULL)
		{
		gchar *text = g_strdup_printf(_("Invalid file name:\n%s"), new);
		file_util_warning_dialog(_("Error renaming file"), text, GTK_STOCK_DIALOG_ERROR, vf->listview);
		g_free(text);
		}
	else if (isfile(new_path))
		{
		gchar *text = g_strdup_printf(_("A file with name %s already exists."), new);
		file_util_warning_dialog(_("Error renaming file"), text, GTK_STOCK_DIALOG_ERROR, vf->listview);
		g_free(text);
		}
	else
		{
		gint row = vf_index_by_path(vf, old_path);
		if (row >= 0)
			{
			GList *work = g_list_nth(vf->list, row);
			FileData *fd = work->data;
			
			file_util_rename_simple(fd, new_path, vf->listview);

			}

		}
	g_free(old_path);
	g_free(new_path);

	return FALSE;
}

static void vflist_menu_position_cb(GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
	ViewFile *vf = data;
	GtkTreeModel *store;
	GtkTreeIter iter;
	GtkTreePath *tpath;
	gint cw, ch;

	if (vflist_find_row(vf, VFLIST_INFO(vf, click_fd), &iter) < 0) return;
	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview));
	tpath = gtk_tree_model_get_path(store, &iter);
	tree_view_get_cell_clamped(GTK_TREE_VIEW(vf->listview), tpath, FILE_COLUMN_NAME - 1, TRUE, x, y, &cw, &ch);
	gtk_tree_path_free(tpath);
	*y += ch;
	popup_menu_position_clamp(menu, x, y, 0);
}

gint vflist_press_key_cb(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
	ViewFile *vf = data;
	GtkTreePath *tpath;

	if (event->keyval != GDK_Menu) return FALSE;

	gtk_tree_view_get_cursor(GTK_TREE_VIEW(vf->listview), &tpath, NULL);
	if (tpath)
		{
		GtkTreeModel *store;
		GtkTreeIter iter;

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &VFLIST_INFO(vf, click_fd), -1);
		gtk_tree_path_free(tpath);
		}
	else
		{
		VFLIST_INFO(vf, click_fd) = NULL;
		}

	vf->popup = vf_pop_menu(vf);
	gtk_menu_popup(GTK_MENU(vf->popup), NULL, NULL, vflist_menu_position_cb, vf, 0, GDK_CURRENT_TIME);

	return TRUE;
}

gint vflist_press_cb(GtkWidget *widget, GdkEventButton *bevent, gpointer data)
{
	ViewFile *vf = data;
	GtkTreePath *tpath;
	GtkTreeIter iter;
	FileData *fd = NULL;
	GtkTreeViewColumn *column;
	
	vf->clicked_mark = 0;

	if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), bevent->x, bevent->y,
					  &tpath, &column, NULL, NULL))
		{
		GtkTreeModel *store;
		gint col_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "column_store_idx"));

		if (bevent->button == MOUSE_BUTTON_LEFT &&
		    col_idx >= FILE_COLUMN_MARKS && col_idx <= FILE_COLUMN_MARKS_LAST)
			return FALSE;

		if (col_idx >= FILE_COLUMN_MARKS && col_idx <= FILE_COLUMN_MARKS_LAST)
			vf->clicked_mark = 1 + (col_idx - FILE_COLUMN_MARKS);

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));

		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);
#if 0
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(widget), tpath, NULL, FALSE);
#endif
		gtk_tree_path_free(tpath);
		}

	VFLIST_INFO(vf, click_fd) = fd;

	if (bevent->button == MOUSE_BUTTON_RIGHT)
		{
		vf->popup = vf_pop_menu(vf);
		gtk_menu_popup(GTK_MENU(vf->popup), NULL, NULL, NULL, NULL,
				bevent->button, bevent->time);
		return TRUE;
		}

	if (!fd) return FALSE;

	if (bevent->button == MOUSE_BUTTON_MIDDLE)
		{
		if (!vflist_row_is_selected(vf, fd))
			{
			vflist_color_set(vf, fd, TRUE);
			}
		return TRUE;
		}


	if (bevent->button == MOUSE_BUTTON_LEFT && bevent->type == GDK_BUTTON_PRESS &&
	    !(bevent->state & GDK_SHIFT_MASK ) &&
	    !(bevent->state & GDK_CONTROL_MASK ) &&
	    vflist_row_is_selected(vf, fd))
		{
		GtkTreeSelection *selection;

		gtk_widget_grab_focus(widget);


		/* returning FALSE and further processing of the event is needed for 
		   correct operation of the expander, to show the sidecar files.
		   It however resets the selection of multiple files. With this condition
		   it should work for both cases */
		selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
		return (gtk_tree_selection_count_selected_rows(selection) > 1);
		}

#if 0
	if (bevent->button == MOUSE_BUTTON_LEFT && bevent->type == GDK_2BUTTON_PRESS)
		{
		if (vf->layout) layout_image_full_screen_start(vf->layout);
		}
#endif

	return FALSE;
}

gint vflist_release_cb(GtkWidget *widget, GdkEventButton *bevent, gpointer data)
{
	ViewFile *vf = data;
	GtkTreePath *tpath;
	GtkTreeIter iter;
	FileData *fd = NULL;

	if (bevent->button == MOUSE_BUTTON_MIDDLE)
		{
		vflist_color_set(vf, VFLIST_INFO(vf, click_fd), FALSE);
		}

	if (bevent->button != MOUSE_BUTTON_LEFT && bevent->button != MOUSE_BUTTON_MIDDLE)
		{
		return TRUE;
		}

	if ((bevent->x != 0 || bevent->y != 0) &&
	    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), bevent->x, bevent->y,
					  &tpath, NULL, NULL, NULL))
		{
		GtkTreeModel *store;

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(widget));
		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);
		gtk_tree_path_free(tpath);
		}

	if (bevent->button == MOUSE_BUTTON_MIDDLE)
		{
		if (fd && VFLIST_INFO(vf, click_fd) == fd)
			{
			GtkTreeSelection *selection;

			selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
			if (vflist_row_is_selected(vf, fd))
				{
				gtk_tree_selection_unselect_iter(selection, &iter);
				}
			else
				{
				gtk_tree_selection_select_iter(selection, &iter);
				}
			}
		return TRUE;
		}

	if (fd && VFLIST_INFO(vf, click_fd) == fd &&
	    !(bevent->state & GDK_SHIFT_MASK ) &&
	    !(bevent->state & GDK_CONTROL_MASK ) &&
	    vflist_row_is_selected(vf, fd))
		{
		GtkTreeSelection *selection;

		selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
		gtk_tree_selection_unselect_all(selection);
		gtk_tree_selection_select_iter(selection, &iter);
		vflist_move_cursor(vf, &iter);
//		return TRUE;// FIXME - expand
		}

	return FALSE;
}

static void vflist_select_image(ViewFile *vf, FileData *sel_fd)
{
	FileData *read_ahead_fd = NULL;
	gint row;
	FileData *cur_fd;
	if (!sel_fd) return;

	cur_fd = layout_image_get_fd(vf->layout);
	if (sel_fd == cur_fd) return; /* no change */

	row = g_list_index(vf->list, sel_fd);
	// FIXME sidecar data

	if (sel_fd && options->image.enable_read_ahead && row >= 0)
		{
		if (row > g_list_index(vf->list, cur_fd) &&
		    (guint) (row + 1) < vf_count(vf, NULL))
			{
			read_ahead_fd = vf_index_get_data(vf, row + 1);
			}
		else if (row > 0)
			{
			read_ahead_fd = vf_index_get_data(vf, row - 1);
			}
		}

	layout_image_set_with_ahead(vf->layout, sel_fd, read_ahead_fd);
}

static gint vflist_select_idle_cb(gpointer data)
{
	ViewFile *vf = data;

	if (!vf->layout)
		{
		VFLIST_INFO(vf, select_idle_id) = -1;
		return FALSE;
		}

	vf_send_update(vf);

	if (VFLIST_INFO(vf, select_fd))
		{
		vflist_select_image(vf, VFLIST_INFO(vf, select_fd));
		VFLIST_INFO(vf, select_fd) = NULL;
		}

	VFLIST_INFO(vf, select_idle_id) = -1;
	return FALSE;
}

static void vflist_select_idle_cancel(ViewFile *vf)
{
	if (VFLIST_INFO(vf, select_idle_id) != -1) g_source_remove(VFLIST_INFO(vf, select_idle_id));
	VFLIST_INFO(vf, select_idle_id) = -1;
}

static gboolean vflist_select_cb(GtkTreeSelection *selection, GtkTreeModel *store, GtkTreePath *tpath,
				 gboolean path_currently_selected, gpointer data)
{
	ViewFile *vf = data;
	GtkTreeIter iter;

	if (!path_currently_selected &&
	    gtk_tree_model_get_iter(store, &iter, tpath))
		{
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &VFLIST_INFO(vf, select_fd), -1);
		}
	else
		{
		VFLIST_INFO(vf, select_fd) = NULL;
		}

	if (vf->layout &&
	    VFLIST_INFO(vf, select_idle_id) == -1)
		{
		VFLIST_INFO(vf, select_idle_id) = g_idle_add(vflist_select_idle_cb, vf);
		}

	return TRUE;
}

/*
 *-----------------------------------------------------------------------------
 * misc
 *-----------------------------------------------------------------------------
 */

/*
static gboolean vflist_dummy_select_cb(GtkTreeSelection *selection, GtkTreeModel *store, GtkTreePath *tpath,
					gboolean path_currently_selected, gpointer data)
{
	return TRUE;
}
*/

static void vflist_setup_iter(ViewFile *vf, GtkTreeStore *store, GtkTreeIter *iter, FileData *fd)
{
	int i;
	gchar *size;
	gchar *sidecars = NULL;

	if (fd->sidecar_files)
		sidecars = file_data_sc_list_to_string(fd);
	size = text_from_size(fd->size);

	gtk_tree_store_set(store, iter, FILE_COLUMN_POINTER, fd,
					FILE_COLUMN_VERSION, fd->version,
					FILE_COLUMN_THUMB, (VFLIST_INFO(vf, thumbs_enabled)) ? fd->pixbuf : NULL,
					FILE_COLUMN_NAME, fd->name,
					FILE_COLUMN_SIDECARS, sidecars,
					FILE_COLUMN_SIZE, size,
					FILE_COLUMN_DATE, text_from_time(fd->date),
					FILE_COLUMN_COLOR, FALSE, -1);
	for (i = 0; i < FILEDATA_MARKS_SIZE; i++)
		gtk_tree_store_set(store, iter, FILE_COLUMN_MARKS + i, fd->marks[i], -1);

	g_free(size);
	if (sidecars)
		g_free(sidecars);
}

static void vflist_setup_iter_recursive(ViewFile *vf, GtkTreeStore *store, GtkTreeIter *parent_iter, GList *list, GList *selected)
{
	GList *work;
	GtkTreeIter iter;
	gint valid;

	valid = gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &iter, parent_iter);

	work = list;
	while (work)
		{
		gint match;
		FileData *fd = work->data;
		gint done = FALSE;

		while (!done)
			{
			FileData *old_fd = NULL;
			gint old_version = 0;

			if (valid)
				{
				gtk_tree_model_get(GTK_TREE_MODEL(store), &iter,
						   FILE_COLUMN_POINTER, &old_fd,
						   FILE_COLUMN_VERSION, &old_version,
						   -1);

				if (fd == old_fd)
					{
					match = 0;
					}
				else
					{
					if (parent_iter)
						match = filelist_sort_compare_filedata_full(fd, old_fd, SORT_NAME, TRUE); /* always sort sidecars by name */
					else
						match = filelist_sort_compare_filedata_full(fd, old_fd, vf->sort_method, vf->sort_ascend);

					if (match == 0) g_warning("multiple fd for the same path");
					}
					
				}
			else
				{
				match = -1;
				}

			if (match < 0)
				{
				GtkTreeIter new;

				if (valid)
					{
					gtk_tree_store_insert_before(store, &new, parent_iter, &iter);
					}
				else
					{
					gtk_tree_store_append(store, &new, parent_iter);
					}

				vflist_setup_iter(vf, store, &new, fd);
				vflist_setup_iter_recursive(vf, store, &new, fd->sidecar_files, selected);
				
				if (g_list_find(selected, fd))
					{
					/* renamed files - the same fd appears at different position - select it again*/
					GtkTreeSelection *selection;
					selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vf->listview));
					gtk_tree_selection_select_iter(selection, &new);
					}

				done = TRUE;
				}
			else if (match > 0)
				{
				valid = gtk_tree_store_remove(store, &iter);
				}
			else
				{
				if (fd->version != old_version)
					{
					vflist_setup_iter(vf, store, &iter, fd);
					vflist_setup_iter_recursive(vf, store, &iter, fd->sidecar_files, selected);
					}

				if (valid) valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);

				done = TRUE;
				}
			}
		work = work->next;
		}

	while (valid)
		{
		valid = gtk_tree_store_remove(store, &iter);
		}
}

void vflist_sort_set(ViewFile *vf, SortType type, gint ascend)
{
	gint i;
	GHashTable *fd_idx_hash = g_hash_table_new(NULL, NULL);
	gint *new_order;
	GtkTreeStore *store;
	GList *work;

	if (vf->sort_method == type && vf->sort_ascend == ascend) return;
	if (!vf->list) return;

	work = vf->list;
	i = 0;
	while (work)
		{
		FileData *fd = work->data;
		g_hash_table_insert(fd_idx_hash, fd, GINT_TO_POINTER(i));
		i++;
		work = work->next;
		}

	vf->sort_method = type;
	vf->sort_ascend = ascend;

	vf->list = filelist_sort(vf->list, vf->sort_method, vf->sort_ascend);

	new_order = g_malloc(i * sizeof(gint));

	work = vf->list;
	i = 0;
	while (work)
		{
		FileData *fd = work->data;
		new_order[i] = GPOINTER_TO_INT(g_hash_table_lookup(fd_idx_hash, fd));
		i++;
		work = work->next;
		}

	store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview)));
	gtk_tree_store_reorder(store, NULL, new_order);

	g_free(new_order);
	g_hash_table_destroy(fd_idx_hash);
}

/*
 *-----------------------------------------------------------------------------
 * thumb updates
 *-----------------------------------------------------------------------------
 */

static gint vflist_thumb_next(ViewFile *vf);

static void vflist_thumb_status(ViewFile *vf, gdouble val, const gchar *text)
{
	if (vf->func_thumb_status)
		{
		vf->func_thumb_status(vf, val, text, vf->data_thumb_status);
		}
}

static void vflist_thumb_cleanup(ViewFile *vf)
{
	vflist_thumb_status(vf, 0.0, NULL);

	vf->thumbs_count = 0;
	vf->thumbs_running = FALSE;

	thumb_loader_free(vf->thumbs_loader);
	vf->thumbs_loader = NULL;

	vf->thumbs_filedata = NULL;
}

static void vflist_thumb_stop(ViewFile *vf)
{
	if (vf->thumbs_running) vflist_thumb_cleanup(vf);
}

static void vflist_thumb_do(ViewFile *vf, ThumbLoader *tl, FileData *fd)
{
	GtkTreeStore *store;
	GtkTreeIter iter;

	if (!fd || vflist_find_row(vf, fd, &iter) < 0) return;

	if (fd->pixbuf) g_object_unref(fd->pixbuf);
	fd->pixbuf = thumb_loader_get_pixbuf(tl, TRUE);

	store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview)));
	gtk_tree_store_set(store, &iter, FILE_COLUMN_THUMB, fd->pixbuf, -1);

	vflist_thumb_status(vf, (gdouble)(vf->thumbs_count) / vflist_sidecar_list_count(vf->list), _("Loading thumbs..."));
}

static void vflist_thumb_error_cb(ThumbLoader *tl, gpointer data)
{
	ViewFile *vf = data;

	if (vf->thumbs_filedata && vf->thumbs_loader == tl)
		{
		vflist_thumb_do(vf, tl, vf->thumbs_filedata);
		}

	while (vflist_thumb_next(vf));
}

static void vflist_thumb_done_cb(ThumbLoader *tl, gpointer data)
{
	ViewFile *vf = data;

	if (vf->thumbs_filedata && vf->thumbs_loader == tl)
		{
		vflist_thumb_do(vf, tl, vf->thumbs_filedata);
		}

	while (vflist_thumb_next(vf));
}

static gint vflist_thumb_next(ViewFile *vf)
{
	GtkTreePath *tpath;
	FileData *fd = NULL;

	/* first check the visible files */

	if (GTK_WIDGET_REALIZED(vf->listview) &&
	    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(vf->listview), 0, 0, &tpath, NULL, NULL, NULL))
		{
		GtkTreeModel *store;
		GtkTreeIter iter;
		gint valid = TRUE;

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview));
		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_path_free(tpath);

		while (!fd && valid && tree_view_row_get_visibility(GTK_TREE_VIEW(vf->listview), &iter, FALSE) == 0)
			{
			gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);
			if (fd->pixbuf) fd = NULL;

			valid = gtk_tree_model_iter_next(store, &iter);
			}
		}

	/* then find first undone */

	if (!fd)
		{
		GList *work = vf->list;
		while (work && !fd)
			{
			FileData *fd_p = work->data;
			if (!fd_p->pixbuf)
				fd = fd_p;
			else
				{
				GList *work2 = fd_p->sidecar_files;

				while (work2 && !fd)
					{
					fd_p = work2->data;
					if (!fd_p->pixbuf) fd = fd_p;
					work2 = work2->next;
					}
				}
			work = work->next;
			}
		}

	if (!fd)
		{
		/* done */
		vflist_thumb_cleanup(vf);
		return FALSE;
		}

	vf->thumbs_count++;

	vf->thumbs_filedata = fd;

	thumb_loader_free(vf->thumbs_loader);

	vf->thumbs_loader = thumb_loader_new(options->thumbnails.max_width, options->thumbnails.max_height);
	thumb_loader_set_callbacks(vf->thumbs_loader,
				   vflist_thumb_done_cb,
				   vflist_thumb_error_cb,
				   NULL,
				   vf);

	if (!thumb_loader_start(vf->thumbs_loader, fd->path))
		{
		/* set icon to unknown, continue */
		DEBUG_1("thumb loader start failed %s", vf->thumbs_loader->path);
		vflist_thumb_do(vf, vf->thumbs_loader, fd);

		return TRUE;
		}

	return FALSE;
}

static void vflist_thumb_update(ViewFile *vf)
{
	vflist_thumb_stop(vf);
	if (!VFLIST_INFO(vf, thumbs_enabled)) return;

	vflist_thumb_status(vf, 0.0, _("Loading thumbs..."));
	vf->thumbs_running = TRUE;

	while (vflist_thumb_next(vf));
}

/*
 *-----------------------------------------------------------------------------
 * row stuff
 *-----------------------------------------------------------------------------
 */

FileData *vflist_index_get_data(ViewFile *vf, gint row)
{
	return g_list_nth_data(vf->list, row);
}

gint vflist_index_by_path(ViewFile *vf, const gchar *path)
{
	gint p = 0;
	GList *work;

	if (!path) return -1;

	work = vf->list;
	while (work)
		{
		FileData *fd = work->data;
		if (strcmp(path, fd->path) == 0) return p;
		
		work = work->next;
		p++;
		}

	return -1;
}

guint vflist_count(ViewFile *vf, gint64 *bytes)
{
	if (bytes)
		{
		gint64 b = 0;
		GList *work;

		work = vf->list;
		while (work)
			{
			FileData *fd = work->data;
			work = work->next;
			b += fd->size;
			}

		*bytes = b;
		}

	return g_list_length(vf->list);
}

GList *vflist_get_list(ViewFile *vf)
{
	GList *list = NULL;
	GList *work;

	work = vf->list;
	while (work)
		{
		FileData *fd = work->data;
		work = work->next;

		list = g_list_prepend(list, file_data_ref(fd));
		}

	return g_list_reverse(list);
}

/*
 *-----------------------------------------------------------------------------
 * selections
 *-----------------------------------------------------------------------------
 */

static gint vflist_row_is_selected(ViewFile *vf, FileData *fd)
{
	GtkTreeModel *store;
	GtkTreeSelection *selection;
	GList *slist;
	GList *work;
	gint found = FALSE;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vf->listview));
	slist = gtk_tree_selection_get_selected_rows(selection, &store);
	work = slist;
	while (!found && work)
		{
		GtkTreePath *tpath = work->data;
		FileData *fd_n;
		GtkTreeIter iter;

		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd_n, -1);
		if (fd_n == fd) found = TRUE;
		work = work->next;
		}
	g_list_foreach(slist, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(slist);

	return found;
}

gint vflist_index_is_selected(ViewFile *vf, gint row)
{
	FileData *fd;

	fd = vf_index_get_data(vf, row);
	return vflist_row_is_selected(vf, fd);
}

guint vflist_selection_count(ViewFile *vf, gint64 *bytes)
{
	GtkTreeModel *store;
	GtkTreeSelection *selection;
	GList *slist;
	guint count;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vf->listview));
	slist = gtk_tree_selection_get_selected_rows(selection, &store);

	if (bytes)
		{
		gint64 b = 0;
		GList *work;

		work = slist;
		while (work)
			{
			GtkTreePath *tpath = work->data;
			GtkTreeIter iter;
			FileData *fd;

			gtk_tree_model_get_iter(store, &iter, tpath);
			gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);
			b += fd->size;

			work = work->next;
			}

		*bytes = b;
		}

	count = g_list_length(slist);
	g_list_foreach(slist, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(slist);

	return count;
}

GList *vflist_selection_get_list(ViewFile *vf)
{
	GtkTreeModel *store;
	GtkTreeSelection *selection;
	GList *slist;
	GList *list = NULL;
	GList *work;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vf->listview));
	slist = gtk_tree_selection_get_selected_rows(selection, &store);
	work = slist;
	while (work)
		{
		GtkTreePath *tpath = work->data;
		FileData *fd;
		GtkTreeIter iter;

		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);

		list = g_list_prepend(list, file_data_ref(fd));

		work = work->next;
		}
	g_list_foreach(slist, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(slist);

	return g_list_reverse(list);
}

GList *vflist_selection_get_list_by_index(ViewFile *vf)
{
	GtkTreeModel *store;
	GtkTreeSelection *selection;
	GList *slist;
	GList *list = NULL;
	GList *work;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vf->listview));
	slist = gtk_tree_selection_get_selected_rows(selection, &store);
	work = slist;
	while (work)
		{
		GtkTreePath *tpath = work->data;
		FileData *fd;
		GtkTreeIter iter;

		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);

		list = g_list_prepend(list, GINT_TO_POINTER(g_list_index(vf->list, fd)));

		work = work->next;
		}
	g_list_foreach(slist, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(slist);

	return g_list_reverse(list);
}

void vflist_select_all(ViewFile *vf)
{
	GtkTreeSelection *selection;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vf->listview));
	gtk_tree_selection_select_all(selection);

	VFLIST_INFO(vf, select_fd) = NULL;
}

void vflist_select_none(ViewFile *vf)
{
	GtkTreeSelection *selection;

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vf->listview));
	gtk_tree_selection_unselect_all(selection);
}

static gboolean tree_model_iter_prev(GtkTreeModel *store, GtkTreeIter *iter)
{
	GtkTreePath *tpath;
	gboolean result;

	tpath = gtk_tree_model_get_path(store, iter);
	result = gtk_tree_path_prev(tpath);
	if (result)
		gtk_tree_model_get_iter(store, iter, tpath);

	gtk_tree_path_free(tpath);

	return result;
}

static gboolean tree_model_get_iter_last(GtkTreeModel *store, GtkTreeIter *iter)
{
	if (!gtk_tree_model_get_iter_first(store, iter))
		return FALSE;

	while (TRUE)
		{
		GtkTreeIter next = *iter;
		
		if (gtk_tree_model_iter_next(store, &next))
			*iter = next;
		else
			break;
		}
	
	return TRUE;
}

void vflist_select_invert(ViewFile *vf)
{
	GtkTreeIter iter;
	GtkTreeSelection *selection;
	GtkTreeModel *store;
	gboolean valid;

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview));
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vf->listview));

	/* Backward iteration prevents scrolling to the end of the list,
	 * it scrolls to the first selected row instead. */
	valid = tree_model_get_iter_last(store, &iter);

	while (valid)
		{
		gint selected = gtk_tree_selection_iter_is_selected(selection, &iter);

		if (selected)
			gtk_tree_selection_unselect_iter(selection, &iter);
		else
			gtk_tree_selection_select_iter(selection, &iter);
				
		valid = tree_model_iter_prev(store, &iter);
		}
}

void vflist_select_by_fd(ViewFile *vf, FileData *fd)
{
	GtkTreeIter iter;

	if (vflist_find_row(vf, fd, &iter) < 0) return;

	tree_view_row_make_visible(GTK_TREE_VIEW(vf->listview), &iter, TRUE);

	if (!vflist_row_is_selected(vf, fd))
		{
		GtkTreeSelection *selection;
		GtkTreeModel *store;
		GtkTreePath *tpath;

		selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vf->listview));
		gtk_tree_selection_unselect_all(selection);
		gtk_tree_selection_select_iter(selection, &iter);
		vflist_move_cursor(vf, &iter);

		store = gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview));
		tpath = gtk_tree_model_get_path(store, &iter);
		gtk_tree_view_set_cursor(GTK_TREE_VIEW(vf->listview), tpath, NULL, FALSE);
		gtk_tree_path_free(tpath);
		}
}

static void vflist_select_closest(ViewFile *vf, FileData *sel_fd)
{
	GList *work;
	
	if (sel_fd->parent) sel_fd = sel_fd->parent;
	work = vf->list;
	
	while (work)
		{
		gint match;
		FileData *fd = work->data;
		work = work->next;
		

		match = filelist_sort_compare_filedata_full(fd, sel_fd, vf->sort_method, vf->sort_ascend);
		
		if (match >= 0)
			{
			vflist_select_by_fd(vf, fd);
			break;
			}
		}

}

void vflist_mark_to_selection(ViewFile *vf, gint mark, MarkToSelectionMode mode)
{
	GtkTreeModel *store;
	GtkTreeIter iter;
	GtkTreeSelection *selection;
	gint valid;
	gint n = mark - 1;

	g_assert(mark >= 1 && mark <= FILEDATA_MARKS_SIZE);

	store = gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview));
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vf->listview));

	valid = gtk_tree_model_get_iter_first(store, &iter);
	while (valid)
		{
		FileData *fd;
		gboolean mark_val, selected;
		gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, FILE_COLUMN_POINTER, &fd, -1);

		mark_val = fd->marks[n];
		selected = gtk_tree_selection_iter_is_selected(selection, &iter);

		switch (mode)
			{
			case MTS_MODE_SET: selected = mark_val;
				break;
			case MTS_MODE_OR: selected = mark_val | selected;
				break;
			case MTS_MODE_AND: selected = mark_val & selected;
				break;
			case MTS_MODE_MINUS: selected = !mark_val & selected;
				break;
			}

		if (selected)
			gtk_tree_selection_select_iter(selection, &iter);
		else
			gtk_tree_selection_unselect_iter(selection, &iter);

		valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);
		}
}

void vflist_selection_to_mark(ViewFile *vf, gint mark, SelectionToMarkMode mode)
{
	GtkTreeModel *store;
	GtkTreeSelection *selection;
	GList *slist;
	GList *work;
	gint n = mark - 1;

	g_assert(mark >= 1 && mark <= FILEDATA_MARKS_SIZE);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vf->listview));
	slist = gtk_tree_selection_get_selected_rows(selection, &store);
	work = slist;
	while (work)
		{
		GtkTreePath *tpath = work->data;
		FileData *fd;
		GtkTreeIter iter;

		gtk_tree_model_get_iter(store, &iter, tpath);
		gtk_tree_model_get(store, &iter, FILE_COLUMN_POINTER, &fd, -1);

		switch (mode)
			{
			case STM_MODE_SET: fd->marks[n] = 1;
				break;
			case STM_MODE_RESET: fd->marks[n] = 0;
				break;
			case STM_MODE_TOGGLE: fd->marks[n] = !fd->marks[n];
				break;
			}
		
		file_data_unregister_notify_func(vflist_notify_cb, vf); /* we don't need the notification */
		file_data_increment_version(fd);
		file_data_register_notify_func(vflist_notify_cb, vf, NOTIFY_PRIORITY_MEDIUM);

		gtk_tree_store_set(GTK_TREE_STORE(store), &iter, FILE_COLUMN_MARKS + n, fd->marks[n], -1);

		work = work->next;
		}
	g_list_foreach(slist, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(slist);
}

/*
 *-----------------------------------------------------------------------------
 * core (population)
 *-----------------------------------------------------------------------------
 */

static void vflist_listview_set_height(GtkWidget *listview, gint thumb)
{
	GtkTreeViewColumn *column;
	GtkCellRenderer *cell;
	GList *list;

	column = gtk_tree_view_get_column(GTK_TREE_VIEW(listview), 0); /* first column is thumbnail */
	if (!column) return;

	gtk_tree_view_column_set_fixed_width(column, ((thumb) ? options->thumbnails.max_width : 4) + 10);

	list = gtk_tree_view_column_get_cell_renderers(column);
	if (!list) return;
	cell = list->data;
	g_list_free(list);

	g_object_set(G_OBJECT(cell), "height", (thumb) ? options->thumbnails.max_height : -1, NULL);
	gtk_tree_view_columns_autosize(GTK_TREE_VIEW(listview));
}

static void vflist_populate_view(ViewFile *vf)
{
	GtkTreeStore *store;
	gint thumbs;
	GtkTreeRowReference *visible_row = NULL;
	GtkTreePath *tpath;
	GList *selected;

	store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview)));
	thumbs = VFLIST_INFO(vf, thumbs_enabled);

	vflist_thumb_stop(vf);

	if (!vf->list)
		{
		gtk_tree_store_clear(store);
		vf_send_update(vf);
		return;
		}

	if (GTK_WIDGET_REALIZED(vf->listview) &&
	    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(vf->listview), 0, 0, &tpath, NULL, NULL, NULL))
		{
		visible_row = gtk_tree_row_reference_new(GTK_TREE_MODEL(store), tpath);
		gtk_tree_path_free(tpath);
		}

	vflist_listview_set_height(vf->listview, thumbs);

	selected = vflist_selection_get_list(vf);
	
	vflist_setup_iter_recursive(vf, store, NULL, vf->list, selected);

	if (selected && vflist_selection_count(vf, NULL) == 0)
		{
		/* all selected files disappeared */
		vflist_select_closest(vf, selected->data);
		}	

	filelist_free(selected);
	
	if (visible_row)
		{
		if (gtk_tree_row_reference_valid(visible_row))
			{
			tpath = gtk_tree_row_reference_get_path(visible_row);
			gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(vf->listview), tpath, NULL, TRUE, 0.0, 0.0);
			gtk_tree_path_free(tpath);
			}
		gtk_tree_row_reference_free(visible_row);
		}

	vf_send_update(vf);
	vflist_thumb_update(vf);
}

gint vflist_refresh(ViewFile *vf)
{
	GList *old_list;
	gint ret = TRUE;

	old_list = vf->list;
	vf->list = NULL;

	DEBUG_1("%s vflist_refresh: read dir", get_exec_time());
	if (vf->dir_fd)
		{
		file_data_unregister_notify_func(vflist_notify_cb, vf); /* we don't need the notification of changes detected by filelist_read */

		ret = filelist_read(vf->dir_fd, &vf->list, NULL);

		file_data_register_notify_func(vflist_notify_cb, vf, NOTIFY_PRIORITY_MEDIUM);

		DEBUG_1("%s vflist_refresh: sort", get_exec_time());
		vf->list = filelist_sort(vf->list, vf->sort_method, vf->sort_ascend);
		}

	DEBUG_1("%s vflist_refresh: populate view", get_exec_time());

	vflist_populate_view(vf);

	filelist_free(old_list);
	DEBUG_1("%s vflist_refresh: done", get_exec_time());

	return ret;
}

static gint vflist_refresh_idle_cb(gpointer data)
{
	ViewFile *vf = data;

	vflist_refresh(vf);
	vf->refresh_idle_id = -1;
	return FALSE;
}

static void vflist_refresh_idle_cancel(ViewFile *vf)
{
	if (vf->refresh_idle_id != -1) g_source_remove(vf->refresh_idle_id);
	vf->refresh_idle_id = -1;
}


/* this overrides the low default of a GtkCellRenderer from 100 to CELL_HEIGHT_OVERRIDE, something sane for our purposes */

#define CELL_HEIGHT_OVERRIDE 512

static void cell_renderer_height_override(GtkCellRenderer *renderer)
{
	GParamSpec *spec;

	spec = g_object_class_find_property(G_OBJECT_GET_CLASS(G_OBJECT(renderer)), "height");
	if (spec && G_IS_PARAM_SPEC_INT(spec))
		{
		GParamSpecInt *spec_int;

		spec_int = G_PARAM_SPEC_INT(spec);
		if (spec_int->maximum < CELL_HEIGHT_OVERRIDE) spec_int->maximum = CELL_HEIGHT_OVERRIDE;
		}
}

static GdkColor *vflist_listview_color_shifted(GtkWidget *widget)
{
	static GdkColor color;
	static GtkWidget *done = NULL;

	if (done != widget)
		{
		GtkStyle *style;

		style = gtk_widget_get_style(widget);
		memcpy(&color, &style->base[GTK_STATE_NORMAL], sizeof(color));
		shift_color(&color, -1, 0);
		done = widget;
		}

	return &color;
}

static void vflist_listview_color_cb(GtkTreeViewColumn *tree_column, GtkCellRenderer *cell,
				     GtkTreeModel *tree_model, GtkTreeIter *iter, gpointer data)
{
	ViewFile *vf = data;
	gboolean set;

	gtk_tree_model_get(tree_model, iter, FILE_COLUMN_COLOR, &set, -1);
	g_object_set(G_OBJECT(cell),
		     "cell-background-gdk", vflist_listview_color_shifted(vf->listview),
		     "cell-background-set", set, NULL);
}

static void vflist_listview_add_column(ViewFile *vf, gint n, const gchar *title, gint image, gint right_justify, gint expand)
{
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;

	column = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(column, title);
	gtk_tree_view_column_set_min_width(column, 4);

	if (!image)
		{
		gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_GROW_ONLY);
		renderer = gtk_cell_renderer_text_new();
		if (right_justify)
			{
			g_object_set(G_OBJECT(renderer), "xalign", 1.0, NULL);
			}
		gtk_tree_view_column_pack_start(column, renderer, TRUE);
		gtk_tree_view_column_add_attribute(column, renderer, "text", n);
		if (expand)
			gtk_tree_view_column_set_expand(column, TRUE);
		}
	else
		{
		gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
		renderer = gtk_cell_renderer_pixbuf_new();
		cell_renderer_height_override(renderer);
		gtk_tree_view_column_pack_start(column, renderer, TRUE);
		gtk_tree_view_column_add_attribute(column, renderer, "pixbuf", n);
		}

	gtk_tree_view_column_set_cell_data_func(column, renderer, vflist_listview_color_cb, vf, NULL);
	g_object_set_data(G_OBJECT(column), "column_store_idx", GUINT_TO_POINTER(n));
	g_object_set_data(G_OBJECT(renderer), "column_store_idx", GUINT_TO_POINTER(n));

	gtk_tree_view_append_column(GTK_TREE_VIEW(vf->listview), column);
}

static void vflist_listview_mark_toggled_cb(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
	ViewFile *vf = data;
	GtkTreeStore *store;
	GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
	GtkTreeIter iter;
	FileData *fd;
	gboolean mark;
	guint col_idx;

	store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview)));
	if (!path || !gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, path))
    		return;

	col_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "column_store_idx"));

	g_assert(col_idx >= FILE_COLUMN_MARKS && col_idx <= FILE_COLUMN_MARKS_LAST);

	gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, FILE_COLUMN_POINTER, &fd, col_idx, &mark, -1);
	mark = !mark;
	fd->marks[col_idx - FILE_COLUMN_MARKS] = mark;
	file_data_unregister_notify_func(vflist_notify_cb, vf); /* we don't need the notification */
	file_data_increment_version(fd);
	file_data_register_notify_func(vflist_notify_cb, vf, NOTIFY_PRIORITY_MEDIUM);

	gtk_tree_store_set(store, &iter, col_idx, mark, -1);
	gtk_tree_path_free(path);
}

static void vflist_listview_add_column_toggle(ViewFile *vf, gint n, const gchar *title)
{
	GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;
	GtkTreeStore *store;
	gint index;

	store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview)));

	renderer = gtk_cell_renderer_toggle_new();
	column = gtk_tree_view_column_new_with_attributes(title, renderer, "active", n, NULL);

	gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
	g_object_set_data(G_OBJECT(column), "column_store_idx", GUINT_TO_POINTER(n));
	g_object_set_data(G_OBJECT(renderer), "column_store_idx", GUINT_TO_POINTER(n));

	index = gtk_tree_view_append_column(GTK_TREE_VIEW(vf->listview), column);
	gtk_tree_view_column_set_fixed_width(column, 16);
	gtk_tree_view_column_set_visible(column, vf->marks_enabled);


	g_signal_connect(G_OBJECT(renderer), "toggled", G_CALLBACK(vflist_listview_mark_toggled_cb), vf);
}

/*
 *-----------------------------------------------------------------------------
 * base
 *-----------------------------------------------------------------------------
 */

gint vflist_set_fd(ViewFile *vf, FileData *dir_fd)
{
	GtkTreeStore *store;

	if (!dir_fd) return FALSE;
	if (vf->dir_fd == dir_fd) return TRUE;

	file_data_unref(vf->dir_fd);
	vf->dir_fd = file_data_ref(dir_fd);

	/* force complete reload */
	store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview)));
	gtk_tree_store_clear(store);

	filelist_free(vf->list);
	vf->list = NULL;

	return vf_refresh(vf);
}

void vflist_destroy_cb(GtkWidget *widget, gpointer data)
{
	ViewFile *vf = data;

	file_data_unregister_notify_func(vflist_notify_cb, vf);

	vflist_select_idle_cancel(vf);
	vflist_refresh_idle_cancel(vf);
	vflist_thumb_stop(vf);

	filelist_free(vf->list);
}

ViewFile *vflist_new(ViewFile *vf, FileData *dir_fd)
{
	GtkTreeStore *store;
	GtkTreeSelection *selection;

	GType flist_types[FILE_COLUMN_COUNT];
	int i;

	vf->info = g_new0(ViewFileInfoList, 1);
	
	VFLIST_INFO(vf, click_fd) = NULL;
	VFLIST_INFO(vf, select_fd) = NULL;
	VFLIST_INFO(vf, thumbs_enabled) = FALSE;

	VFLIST_INFO(vf, select_idle_id) = -1;
	vf->refresh_idle_id = -1;


	flist_types[FILE_COLUMN_POINTER] = G_TYPE_POINTER;
	flist_types[FILE_COLUMN_VERSION] = G_TYPE_INT;
	flist_types[FILE_COLUMN_THUMB] = GDK_TYPE_PIXBUF;
	flist_types[FILE_COLUMN_NAME] = G_TYPE_STRING;
	flist_types[FILE_COLUMN_SIDECARS] = G_TYPE_STRING;
	flist_types[FILE_COLUMN_SIZE] = G_TYPE_STRING;
	flist_types[FILE_COLUMN_DATE] = G_TYPE_STRING;
	flist_types[FILE_COLUMN_COLOR] = G_TYPE_BOOLEAN;
	for (i = FILE_COLUMN_MARKS; i < FILE_COLUMN_MARKS + FILEDATA_MARKS_SIZE; i++)
		flist_types[i] = G_TYPE_BOOLEAN;

	store = gtk_tree_store_newv(FILE_COLUMN_COUNT, flist_types);

	vf->listview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
	g_object_unref(store);

	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vf->listview));
	gtk_tree_selection_set_mode(GTK_TREE_SELECTION(selection), GTK_SELECTION_MULTIPLE);
	gtk_tree_selection_set_select_function(selection, vflist_select_cb, vf, NULL);

	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(vf->listview), FALSE);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(vf->listview), FALSE);

	vflist_listview_add_column(vf, FILE_COLUMN_THUMB, "", TRUE, FALSE, FALSE);

	for (i = 0; i < FILEDATA_MARKS_SIZE; i++)
    		vflist_listview_add_column_toggle(vf, i + FILE_COLUMN_MARKS, "");

	vflist_listview_add_column(vf, FILE_COLUMN_NAME, _("Name"), FALSE, FALSE, FALSE);
	vflist_listview_add_column(vf, FILE_COLUMN_SIDECARS, _("SC"), FALSE, FALSE, FALSE);

	vflist_listview_add_column(vf, FILE_COLUMN_SIZE, _("Size"), FALSE, TRUE, FALSE);
	vflist_listview_add_column(vf, FILE_COLUMN_DATE, _("Date"), FALSE, TRUE, FALSE);

	file_data_register_notify_func(vflist_notify_cb, vf, NOTIFY_PRIORITY_MEDIUM);
	return vf;
}

void vflist_thumb_set(ViewFile *vf, gint enable)
{
	if (VFLIST_INFO(vf, thumbs_enabled) == enable) return;

	VFLIST_INFO(vf, thumbs_enabled) = enable;
	if (vf->layout) vf_refresh(vf);
}

void vflist_marks_set(ViewFile *vf, gint enable)
{
	GList *columns, *work;

	if (vf->marks_enabled == enable) return;

	vf->marks_enabled = enable;

	columns = gtk_tree_view_get_columns(GTK_TREE_VIEW(vf->listview));

	work = columns;
	while (work)
		{
		GtkTreeViewColumn *column = work->data;
		gint col_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(column), "column_store_idx"));
		work = work->next;

		if (col_idx <= FILE_COLUMN_MARKS_LAST && col_idx >= FILE_COLUMN_MARKS)
			gtk_tree_view_column_set_visible(column, enable);
		}

	g_list_free(columns);
	//vf_refresh(vf);
}

/*
 *-----------------------------------------------------------------------------
 * maintenance (for rename, move, remove)
 *-----------------------------------------------------------------------------
 */

static void vflist_notify_cb(FileData *fd, gpointer data)
{
	ViewFile *vf = data;
	gboolean refresh;

	if (vf->refresh_idle_id != -1) return;
	
	refresh = (fd == vf->dir_fd);

	if (!refresh)
		{
		gchar *base = remove_level_from_path(fd->path);
		refresh = (strcmp(base, vf->dir_fd->path) == 0);
		g_free(base);
		}

	if (!refresh && fd->change && fd->change->dest)
		{
		gchar *dest_base = remove_level_from_path(fd->change->dest);
		refresh = (strcmp(dest_base, vf->dir_fd->path) == 0);
		g_free(dest_base);
		}

	if (!refresh && fd->change && fd->change->source)
		{
		gchar *source_base = remove_level_from_path(fd->change->source);
		refresh = (strcmp(source_base, vf->dir_fd->path) == 0);
		g_free(source_base);
		}
	
	if (refresh)
		{
		vf->refresh_idle_id = g_idle_add(vflist_refresh_idle_cb, vf);
		}
}

#if 0

gint vflist_maint_renamed(ViewFile *vf, FileData *fd)
{
	vflist_maint(vf, fd);
	return TRUE;
}
gint vflist_maint_removed(ViewFile *vf, FileData *fd, GList *ignore_list)
{
	vflist_maint(vf, fd);
	return TRUE;
}
gint vflist_maint_moved(ViewFile *vf, FileData *fd, GList *ignore_list)
{
	vflist_maint(vf, fd);
	return TRUE;
}


static gint vflist_maint_find_closest(ViewFile *vf, gint row, gint count, GList *ignore_list)
{
	GList *list = NULL;
	GList *work;
	gint rev = row - 1;
	row ++;

	work = ignore_list;
	while (work)
		{
		gint f = vf_index_by_path(vf, work->data);
		if (f >= 0) list = g_list_prepend(list, GINT_TO_POINTER(f));
		work = work->next;
		}

	while (list)
		{
		gint c = TRUE;
		work = list;
		while (work && c)
			{
			gpointer p = work->data;
			work = work->next;
			if (row == GPOINTER_TO_INT(p))
				{
				row++;
				c = FALSE;
				}
			if (rev == GPOINTER_TO_INT(p))
				{
				rev--;
				c = FALSE;
				}
			if (!c) list = g_list_remove(list, p);
			}
		if (c && list)
			{
			g_list_free(list);
			list = NULL;
			}
		}
	if (row > count - 1)
		{
		if (rev < 0)
			return -1;
		else
			return rev;
		}
	else
		{
		return row;
		}
}

gint vflist_maint_renamed(ViewFile *vf, FileData *fd)
{
	gint ret = FALSE;
	gchar *source_base;
	gchar *dest_base;

	DEBUG_1("%s vflist_maint_renamed: start", get_exec_time());

	if (g_list_index(vf->list, fd) < 0) return FALSE;

	source_base = remove_level_from_path(fd->change->source);
	dest_base = remove_level_from_path(fd->change->dest);


	if (strcmp(source_base, dest_base) == 0)
		{
		GtkTreeStore *store;
		GtkTreeIter iter;
		GtkTreeIter position;
		gint old_row;
		gint n;

		old_row = g_list_index(vf->list, fd);

		vf->list = g_list_remove(vf->list, fd);

		vf->list = filelist_insert_sort(vf->list, fd, vf->sort_method, vf->sort_ascend);
		n = g_list_index(vf->list, fd);

		store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview)));
		if (vflist_find_row(vf, fd, &iter) >= 0 &&
		    gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(store), &position, NULL, n))
			{
			if (old_row >= n)
				{
				gtk_tree_store_move_before(store, &iter, &position);
				}
			else
				{
				gtk_tree_store_move_after(store, &iter, &position);
				}
			}
		gtk_tree_store_set(store, &iter, FILE_COLUMN_NAME, fd->name, -1);

		ret = TRUE;
		}
	else
		{
		ret = vflist_maint_removed(vf, fd, NULL);
		}

	g_free(source_base);
	g_free(dest_base);

	DEBUG_1("%s vflist_maint_renamed: done", get_exec_time());

	return ret;
}

gint vflist_maint_removed(ViewFile *vf, FileData *fd, GList *ignore_list)
{
	GtkTreeIter iter;
	GList *list;
	gint row;
	gint new_row = -1;

	DEBUG_1("%s vflist_maint_removed: start", get_exec_time());

	row = g_list_index(vf->list, fd);
	if (row < 0) return FALSE;

	if (vflist_index_is_selected(vf, row) &&
	    layout_image_get_collection(vf->layout, NULL) == NULL)
		{
		gint n;

		n = vf_count(vf, NULL);
		if (ignore_list)
			{
			new_row = vflist_maint_find_closest(vf, row, n, ignore_list);
			DEBUG_1("row = %d, closest is %d", row, new_row);
			}
		else
			{
			if (row + 1 < n)
				{
				new_row = row + 1;
				}
			else if (row > 0)
				{
				new_row = row - 1;
				}
			}
		vf_select_none(vf);
		if (new_row >= 0)
			{
			fd = vf_index_get_data(vf, new_row);
			if (vflist_find_row(vf, fd, &iter) >= 0)
				{
				GtkTreeSelection *selection;

				selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(vf->listview));
				gtk_tree_selection_select_iter(selection, &iter);
				vflist_move_cursor(vf, &iter);
				}
			}
		}

	fd = vf_index_get_data(vf, row);
	if (vflist_find_row(vf, fd, &iter) >= 0)
		{
		GtkTreeStore *store;
		store = GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(vf->listview)));
		gtk_tree_store_remove(store, &iter);
		}
	list = g_list_nth(vf->list, row);
	fd = list->data;

	/* thumbnail loader check */
	if (fd == vf->thumbs_filedata) vf->thumbs_filedata = NULL;
	if (vf->thumbs_count > 0) vf->thumbs_count--;

	vf->list = g_list_remove(vf->list, fd);
	file_data_unref(fd);

	vf_send_update(vf);

	DEBUG_1("%s vflist_maint_removed: done", get_exec_time());

	return TRUE;
}

gint vflist_maint_moved(ViewFile *vf, FileData *fd, GList *ignore_list)
{
	gint ret = FALSE;
	gchar *buf;

	if (!fd->change->source || !vf->path) return FALSE;

	buf = remove_level_from_path(fd->change->source);

	if (strcmp(buf, vf->path) == 0)
		{
		ret = vflist_maint_removed(vf, fd, ignore_list);
		}

	g_free(buf);

	return ret;
}
#endif
