#include <stdlib.h>
#include <stdio.h>
#include <vector>
#include <utility>
#include <unistd.h>
#include <string>
#include <sstream>
#include <gtk/gtk.h>
#include "../KindleLib/ShakeWindow.h"
#include "FileData.h"

enum
{
    COL_ICON = 0,
    COL_NAME,
    COL_MARKUP,   /* Pango markup: filename + attrs on two lines */
    COL_AGE,
    COL_IS_DIR,
    NUM_COLS
};

/* Remove foreground='gray' from markup when the row is selected so the
   attributes line inverts correctly along with the filename. */
static void text_cell_data_func(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                                 GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    GtkTreeView      *tv  = GTK_TREE_VIEW(data);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
    gboolean selected     = gtk_tree_selection_iter_is_selected(sel, iter);

    gchar *markup = NULL;
    gtk_tree_model_get(model, iter, COL_MARKUP, &markup, -1);

    if (selected && markup)
    {
        /* Strip foreground='...' attribute from spans so selection colour takes over */
        std::string s(markup);
        std::size_t pos;
        const std::string attr_start = " foreground='";
        while ((pos = s.find(attr_start)) != std::string::npos)
        {
            std::size_t end = s.find('\'', pos + attr_start.size());
            if (end == std::string::npos) break;
            s.erase(pos, end - pos + 1);
        }
        g_object_set(renderer, "markup", s.c_str(), NULL);
    }
    else
    {
        g_object_set(renderer, "markup", markup ? markup : "", NULL);
    }
    if (markup) g_free(markup);
}

class FilesModel
{
public:
    void Assign(GtkWidget *view, GtkWidget *breadcrumb, GCallback nav_cb)
    {
        tree = view;
        breadcrumb_box = breadcrumb;
        nav_callback = nav_cb;
        GError *error = NULL;
        file = gdk_pixbuf_new_from_file(GetAppFile("res/file.png").c_str(), &error);
        if (error)
        {
            g_warning ("Could not load icon: %s\n", error->message);
            g_error_free(error);
            error = NULL;
        }
        folder = gdk_pixbuf_new_from_file(GetAppFile("res/folder.png").c_str(), &error);
        if (error)
        {
            g_warning ("Could not load icon: %s\n", error->message);
            g_error_free(error);
            error = NULL;
        }
        image_icon  = gdk_pixbuf_new_from_file(GetAppFile("res/image.png").c_str(),  NULL);
        script_icon = gdk_pixbuf_new_from_file(GetAppFile("res/script.png").c_str(), NULL);
        binary_icon = gdk_pixbuf_new_from_file(GetAppFile("res/binary.png").c_str(), NULL);
        ebook_icon  = gdk_pixbuf_new_from_file(GetAppFile("res/ebook.png").c_str(),  NULL);
        CreateColumns();
    }

    void CreateColumns()
    {
        GtkCellRenderer *renderer;
        /* --- Column --- */
        GtkTreeViewColumn *col = gtk_tree_view_column_new();
        gtk_tree_view_column_set_title(col, "Title");

        renderer = gtk_cell_renderer_pixbuf_new();
        gtk_tree_view_column_pack_start(col, renderer, FALSE);
        gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", COL_ICON, NULL);

        renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        gtk_tree_view_column_pack_start(col, renderer, TRUE);
        /* Use cell-data-func so selected rows drop the gray foreground */
        gtk_tree_view_column_set_cell_data_func(col, renderer,
                                                text_cell_data_func, tree, NULL);

        gtk_tree_view_column_set_expand(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

        /* --- Column --- */
        //renderer = gtk_cell_renderer_text_new();
        //gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tree), -1, "Name", renderer, "text", COL_NAME, NULL);
        /* --- Column --- */
        //renderer = gtk_cell_renderer_text_new();
        //gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tree), -1, "Size", renderer, "text", COL_AGE, NULL);
        UpdateList();
    }

    GtkTreeModel *GetModel()
    {
        return model;
    }

    void UpdateList()
    {
        model = CreateFilesModel();
        gtk_tree_view_set_model(GTK_TREE_VIEW(tree), model);
        g_object_unref(model);

        UpdateBreadcrumb();
    }

    bool IsItemSelected()
    {
        GtkTreeIter iter;
        GtkTreeModel *mdl;

        return gtk_tree_selection_get_selected(GTK_TREE_SELECTION(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree))), &mdl, &iter);
    }

    FileItem GetSelectedItem()
    {
        GtkTreeIter iter;
        char *name;
        gboolean dir;
        FileItem result;
        GtkTreeModel *mdl;

        gtk_tree_selection_get_selected(GTK_TREE_SELECTION(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree))), &mdl, &iter);
        {
            gtk_tree_model_get(mdl, &iter, COL_NAME, &name, COL_IS_DIR, &dir, -1);
            result.name = name;
            result.dir = dir;
            g_free(name);
        }
        return result;
    }

    void SetMultiSelectMode(bool enabled)
    {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
        gtk_tree_selection_set_mode(sel,
            enabled ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);
        if (!enabled)
        {
            gtk_tree_selection_unselect_all(sel);
            /* Detach and reattach the existing model to force GTK to fully
               reset its internal cursor/focus state.  Without this, GTK2
               occasionally leaves the tree unable to accept new selections
               after transitioning from MULTIPLE back to SINGLE mode. */
            GtkTreeModel *mdl = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
            if (mdl)
            {
                g_object_ref(mdl);
                gtk_tree_view_set_model(GTK_TREE_VIEW(tree), NULL);
                gtk_tree_view_set_model(GTK_TREE_VIEW(tree), mdl);
                g_object_unref(mdl);
            }
        }
    }

    int GetSelectedCount()
    {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
        return gtk_tree_selection_count_selected_rows(sel);
    }

    vector<FileItem> GetSelectedItems()
    {
        vector<FileItem> result;
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
        GtkTreeModel *mdl = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
        GList *rows = gtk_tree_selection_get_selected_rows(sel, &mdl);
        for (GList *l = rows; l; l = l->next)
        {
            GtkTreeIter iter;
            gtk_tree_model_get_iter(mdl, &iter, (GtkTreePath*)l->data);
            char *name; gboolean dir;
            gtk_tree_model_get(mdl, &iter, COL_NAME, &name, COL_IS_DIR, &dir, -1);
            FileItem fi;
            fi.name = name;
            fi.dir  = dir;
            g_free(name);
            result.push_back(fi);
        }
        g_list_foreach(rows, (GFunc)gtk_tree_path_free, NULL);
        g_list_free(rows);
        return result;
    }

private:
    void UpdateBreadcrumb()
    {
        /* Clear all existing children */
        GList *children = gtk_container_get_children(GTK_CONTAINER(breadcrumb_box));
        for (GList *l = children; l; l = l->next)
            gtk_widget_destroy(GTK_WIDGET(l->data));
        g_list_free(children);

        /* Get current path */
        char cur[2048];
        GetCurrentDir(cur, sizeof(cur));
        string path_str(cur);

        /* Build segment list: (display_name, full_path) */
        vector<pair<string,string> > segments;
        segments.push_back(make_pair(string("[ROOT]"), string("/")));

        if (path_str != "/")
        {
            string acc = "";
            size_t pos = 1; /* skip leading '/' */
            while (pos <= path_str.size())
            {
                size_t next = path_str.find('/', pos);
                if (next == string::npos) next = path_str.size();
                if (next > pos)
                {
                    string part = path_str.substr(pos, next - pos);
                    acc = acc + "/" + part;
                    segments.push_back(make_pair(part, acc));
                }
                pos = next + 1;
            }
        }

        /* Apply collapse rule: collapse [1..n-4] when n > 5,
           resulting in: ROOT, [...], seg[n-3], seg[n-2], seg[n-1] */
        int n = (int)segments.size();
        vector<pair<string,string> > visible;
        if (n > 5)
        {
            visible.push_back(segments[0]);
            visible.push_back(make_pair(string("[...]"), string(""))); /* no navigation */
            for (int i = n - 3; i < n; i++)
                visible.push_back(segments[i]);
        }
        else
        {
            visible = segments;
        }

        /* Render buttons */
        PangoFontDescription *small_font = pango_font_description_from_string("Tahoma 10");
        bool first = true;
        for (int i = 0; i < (int)visible.size(); i++)
        {
            /* Separator */
            if (!first)
            {
                GtkWidget *sep = gtk_label_new(" > ");
                if (small_font) gtk_widget_modify_font(sep, small_font);
                gtk_widget_show(sep);
                gtk_box_pack_start(GTK_BOX(breadcrumb_box), sep, FALSE, FALSE, 0);
            }
            first = false;

            /* Truncate long names */
            string label = visible[i].first;
            if ((int)label.size() > 15)
                label = label.substr(0, 14) + "\xe2\x80\xa6"; /* UTF-8 ellipsis */

            GtkWidget *btn = gtk_button_new_with_label(label.c_str());
            /* Apply small font to the label inside the button */
            if (small_font)
            {
                GtkWidget *lbl = GTK_BIN(btn)->child;
                if (lbl) gtk_widget_modify_font(lbl, small_font);
            }
            gtk_widget_show(btn);

            /* Connect click only if there is a navigation target */
            string nav_target = visible[i].second;
            if (!nav_target.empty() && nav_callback != NULL)
            {
                char *path_copy = g_strdup(nav_target.c_str());
                g_object_set_data_full(G_OBJECT(btn), "nav_path", path_copy, g_free);
                g_signal_connect(btn, "clicked", nav_callback, NULL);
            }

            gtk_box_pack_start(GTK_BOX(breadcrumb_box), btn, FALSE, FALSE, 0);
        }
        if (small_font) pango_font_description_free(small_font);
    }

    GtkTreeModel *CreateFilesModel()
    {
        GtkListStore *store;
        GtkTreeIter iter;
        store = gtk_list_store_new(NUM_COLS,
                                   GDK_TYPE_PIXBUF,  /* COL_ICON   */
                                   G_TYPE_STRING,    /* COL_NAME   */
                                   G_TYPE_STRING,    /* COL_MARKUP */
                                   G_TYPE_UINT,      /* COL_AGE    */
                                   G_TYPE_BOOLEAN);  /* COL_IS_DIR */
        gtk_tree_sortable_set_default_sort_func (GTK_TREE_SORTABLE(store), NULL, NULL, NULL);
        gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE(store), GTK_TREE_SORTABLE_DEFAULT_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
        vector<FileItem> files = GetFiles();
        for(vector<FileItem>::iterator it = files.begin(); it != files.end(); ++it)
        {
            GdkPixbuf* icon;
            switch (it->file_type) {
                case FT_DIR:    icon = folder; break;
                case FT_IMAGE:  icon = image_icon  ? image_icon  : file; break;
                case FT_SCRIPT: icon = script_icon ? script_icon : file; break;
                case FT_BINARY: icon = binary_icon ? binary_icon : file; break;
                case FT_EBOOK:  icon = ebook_icon  ? ebook_icon  : file; break;
                default:        icon = file; break;
            }

            /* ---- build attrs line ---- */
            string perms = format_perms(it->mode);
            string info;
            if (it->is_symlink && it->symlink_broken)
            {
                info = "Deadlink";
            }
            else if (it->dir)
            {
                char buf[32];
                if (it->dir_count >= 9999)
                    snprintf(buf, sizeof(buf), "9999+ items");
                else if (it->dir_count >= 0)
                    snprintf(buf, sizeof(buf), "%d items", it->dir_count);
                else
                    snprintf(buf, sizeof(buf), "\xe2\x80\x94"); /* em dash */
                info = string(buf);
            }
            else
            {
                info = format_size_auto(it->size);
            }

            /* Append symlink target arrow for all symlinks */
            string link_suffix;
            if (it->is_symlink && !it->symlink_target.empty())
            {
                gchar *esc_tgt = g_markup_escape_text(it->symlink_target.c_str(), -1);
                link_suffix = string("  ->") + esc_tgt;
                g_free(esc_tgt);
            }

            /* ---- escape filename for Pango markup ---- */
            gchar *esc = g_markup_escape_text(it->name.c_str(), -1);
            const char *attrs_color = (it->is_symlink && it->symlink_broken) ? "#cc3333" : "#777777";
            string markup = string(esc)
                + "\n<span size='small' foreground='" + attrs_color + "'>"
                + perms + "  " + info + link_suffix + "</span>";
            g_free(esc);

            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter,
                               COL_ICON,   icon,
                               COL_NAME,   it->name.c_str(),
                               COL_MARKUP, markup.c_str(),
                               COL_AGE,    51,
                               COL_IS_DIR, it->dir,
                               -1);
        }

        return GTK_TREE_MODEL (store);
    }

private:
    GtkWidget *tree;
    GtkWidget *breadcrumb_box;
    GCallback nav_callback;
    GtkTreeModel *model;
    GdkPixbuf* file;
    GdkPixbuf* folder;
    GdkPixbuf* image_icon;
    GdkPixbuf* script_icon;
    GdkPixbuf* binary_icon;
    GdkPixbuf* ebook_icon;
};
