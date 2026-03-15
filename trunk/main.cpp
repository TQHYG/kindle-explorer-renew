#include <stdlib.h>
#include <stdio.h>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <time.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <pwd.h>
#include <grp.h>
#include <pthread.h>
#include <sys/statvfs.h>

#include "../KindleLib/ShakeWindow.h"
#include "FSModel.h"
#include "timer.h"

#define KE_VERSION "2.0"
#define EDITOR "/mnt/us/extensions/leafpad/bin/leafpad"
#define TERMINAL "/mnt/us/extensions/kterm/bin/kterm"

using namespace std;

ShakeWindow *win;
GtkWidget *lstFiles, *hboxBreadcrumb, *hboxCopyBar;
FilesModel *fs;
Timer timer; string lastSel;
vector<string> copy_list;
bool multiselect_mode = false;
static string text_viewer_filename;
const char* imageFormats = "bmp;png;gif;ico;jpg;jpeg;wmf;tga";
GdkPixbuf *ms_icon_normal   = NULL;
GdkPixbuf *ms_icon_inverted = NULL;

/* Sort-bar widget pointers — initialised in main() after win->Show() */
static GtkWidget *lblFileCount = NULL;
static GtkWidget *pbDiskUsage  = NULL;
static GtkWidget *lblDiskUsage = NULL;

/* Return a copy of src with RGB channels inverted; alpha is preserved. */
static GdkPixbuf* invert_pixbuf(GdkPixbuf *src)
{
    if (!src) return NULL;
    GdkPixbuf *dst = gdk_pixbuf_copy(src);
    if (!dst)  return NULL;

    int      width      = gdk_pixbuf_get_width(dst);
    int      height     = gdk_pixbuf_get_height(dst);
    int      rowstride  = gdk_pixbuf_get_rowstride(dst);
    int      n_channels = gdk_pixbuf_get_n_channels(dst);
    guchar  *pixels     = gdk_pixbuf_get_pixels(dst);

    for (int y = 0; y < height; y++)
    {
        guchar *row = pixels + y * rowstride;
        for (int x = 0; x < width; x++)
        {
            guchar *p = row + x * n_channels;
            p[0] = 255 - p[0]; /* R */
            p[1] = 255 - p[1]; /* G */
            p[2] = 255 - p[2]; /* B */
            /* channel 3 (alpha, if present) is left unchanged */
        }
    }
    return dst;
}

/* Image viewer zoom state — valid while the image viewer window is open */
struct {
    GdkPixbuf  *original;
    GtkWidget  *image_widget;
    double      zoom;
    double      fit_zoom;
} img_state = {NULL, NULL, 1.0, 1.0};


void UpdateButtons()
{
    bool sel = multiselect_mode ? (fs->GetSelectedCount() > 0) : fs->IsItemSelected();

    win->Enable("btnCopy",       sel);
    win->Enable("btnDelete",     sel);
    win->Enable("btnRename",     !multiselect_mode && sel);
    win->Enable("btnProperties", !multiselect_mode && sel);
    win->Enable("btnTerminal",   true);
}

/* ---- Sort bar helpers --------------------------------------------- */

/* Custom draw for disk usage progress bar: black border, white bg, black fill.
   Bypasses GTK theme so the appearance is consistent on all Kindle GTK2 themes. */
static gboolean pb_draw_cb(GtkWidget *widget, GdkEventExpose * /*event*/, gpointer /*data*/)
{
    double frac = gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(widget));
    gint w = widget->allocation.width;
    gint h = widget->allocation.height;
    GdkWindow *gwin = widget->window;
    if (!gwin) return FALSE;

    /* White background */
    gdk_draw_rectangle(gwin, widget->style->white_gc, TRUE, 0, 0, w, h);

    /* Black fill for used portion (inset 1px from border) */
    gint fill_w = (gint)(frac * (w - 2));
    if (fill_w > 0)
        gdk_draw_rectangle(gwin, widget->style->black_gc, TRUE, 1, 1, fill_w, h - 2);

    /* Black 1px border */
    gdk_draw_rectangle(gwin, widget->style->black_gc, FALSE, 0, 0, w - 1, h - 1);

    return TRUE; /* stop default theme drawing */
}

static void UpdateSortBar()
{
    if (!lblFileCount || !pbDiskUsage || !lblDiskUsage) return;

    /* File/directory counts from the last listing */
    char count_buf[64];
    snprintf(count_buf, sizeof(count_buf), "%d 个目录  %d 个文件",
             fs->GetDirCount(), fs->GetFileCount());
    gtk_label_set_text(GTK_LABEL(lblFileCount), count_buf);

    /* Disk usage via statvfs on the current directory */
    struct statvfs vfs;
    char cwd[2048];
    if (getcwd(cwd, sizeof(cwd)) && statvfs(cwd, &vfs) == 0)
    {
        unsigned long long total = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
        unsigned long long avail = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
        unsigned long long used  = total > avail ? total - avail : 0;
        double fraction = (total > 0) ? (double)used / total : 0.0;
        if (fraction > 1.0) fraction = 1.0;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pbDiskUsage), fraction);
        string text = format_size_auto((off_t)used) + "/" + format_size_auto((off_t)total);
        gtk_label_set_text(GTK_LABEL(lblDiskUsage), text.c_str());
    }
}

void CycleSortField(GtkWidget *widget, gpointer /*data*/)
{
    SortField next;
    switch (fs->GetSortField()) {
        case SORT_NAME:  next = SORT_SIZE;  break;
        case SORT_SIZE:  next = SORT_TYPE;  break;
        case SORT_TYPE:  next = SORT_MTIME; break;
        default:         next = SORT_NAME;  break;
    }
    const char *labels[] = {"名称", "大小", "类型", "修改时间"};
    gtk_button_set_label(GTK_BUTTON(widget), labels[next]);
    fs->SetSort(next, fs->GetSortDir()); /* triggers UpdateList → on_list_updated */
}

void ToggleSortDir(GtkWidget *widget, gpointer /*data*/)
{
    SortDir nd = (fs->GetSortDir() == SORT_ASC) ? SORT_DESC : SORT_ASC;
    gtk_button_set_label(GTK_BUTTON(widget), nd == SORT_ASC ? "正序" : "逆序");
    fs->SetSort(fs->GetSortField(), nd);
}

/* -------------------------------------------------------------------- */

void DirUp(GtkWidget *widget, gpointer data)
{
    chdir("..");
    fs->UpdateList();
}

void UpdateCopyBar()
{
    if (copy_list.empty())
    {
        gtk_widget_hide(hboxCopyBar);
    }
    else
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "  已复制 %d 个项目", (int)copy_list.size());
        gtk_label_set_text(GTK_LABEL(win->GetWidget("lblCopyStatus")), buf);
        gtk_widget_show(hboxCopyBar);
    }
}

void RunTerminal(GtkWidget *widget, gpointer data)
{
    if (!FileExist(TERMINAL))
    {
        win->MessageBox(string("找不到终端应用：Kterm"));
        return;
    }

    Execute(string(TERMINAL), GetFullLocalFilePath(""));
}

void RenameFile(GtkWidget *widget, gpointer data)
{
    if (!fs->IsItemSelected())
        return;
    FileItem sel = fs->GetSelectedItem();

    OpenKeyboard();
    InputDialogBox* input = new InputDialogBox(win, "\n  重命名\n\n  请输入新名称：                                           ", "", sel.name);
    if (input->ShowDialog() == GTK_RESPONSE_OK)
    {
        string to = GetFullLocalFilePath(input->GetValue());
        rename(GetFullLocalFilePath(sel.name).c_str(), to.c_str());
        fs->UpdateList();
    }
    CloseKeyboard();
    delete input;
}

void DeleteFile(GtkWidget *widget, gpointer data)
{
    vector<FileItem> items;
    if (multiselect_mode)
        items = fs->GetSelectedItems();
    else if (fs->IsItemSelected())
        items.push_back(fs->GetSelectedItem());

    if (items.empty()) return;

    string msg;
    if (items.size() == 1)
    {
        std::string display_name = truncate_filename(items[0].name, 25);
        if (items[0].dir)
            msg = "\n  删除\n\n  确定要删除以下目录及其内部所有文件吗？\n                                                                        \n   \"" + display_name + "\"    \n";
        else
            msg = "\n  删除\n\n  确定要删除以下文件吗？\n                                                             \n   \"" + display_name + "\"    \n";
    }
    else
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "\n  删除\n\n  删除选中的 %d 个项目？\n", (int)items.size());
        msg = string(buf);
    }

    gint res = win->MessageBox(msg, GTK_BUTTONS_YES_NO);
    if (res == GTK_RESPONSE_YES)
    {
        WaitWindow* wait = new WaitWindow(win);
        wait->Show();
        for (size_t i = 0; i < items.size(); i++)
            RemovePath(GetFullLocalFilePath(items[i].name));
        fs->UpdateList();
        delete wait;
    }
}

void CopyFile(GtkWidget *widget, gpointer data)
{
    vector<FileItem> items;
    if (multiselect_mode)
        items = fs->GetSelectedItems();
    else if (fs->IsItemSelected())
        items.push_back(fs->GetSelectedItem());

    if (items.empty()) return;

    copy_list.clear();
    for (size_t i = 0; i < items.size(); i++)
        copy_list.push_back(GetFullLocalFilePath(items[i].name));

    fs->UpdateList();
    UpdateCopyBar();
}

void PasteFile(GtkWidget *widget, gpointer data)
{
    if (copy_list.empty())
        return;

    WaitWindow* wait = new WaitWindow(win);
    wait->Show();

    for (size_t i = 0; i < copy_list.size(); i++)
    {
        string from = copy_list[i];
        string to   = GetFullLocalFilePath(GetFileName(from));
        if (to == from)
            to += "_copy";
        else if (to.find(from) != string::npos)
            continue; /* skip: destination is inside source */
        CopyPath(from, to);
    }

    copy_list.clear();
    fs->UpdateList();
    UpdateCopyBar();
    delete wait;
}

void CancelCopy(GtkWidget *widget, gpointer data)
{
    copy_list.clear();
    UpdateCopyBar();
}

/* ---- New file/folder dialog ---------------------------------------- */
#define NEW_RESP_MKDIR 1
#define NEW_RESP_TOUCH 2

static void on_create_btn_clicked(GtkWidget *btn, gpointer dlg)
{
    gint resp = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "resp-id"));
    gtk_dialog_response(GTK_DIALOG(dlg), resp);
}

void CreateNewItem(GtkWidget *widget, gpointer data)
{
    OpenKeyboard();

    GtkWidget *dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "L:D_N:dialog_ID:net.tqhyg.explorer.New");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg),
                                 GTK_WINDOW(win->GetWidget("wndMain")));
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dlg), TRUE);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 15);

    GtkWidget *lbl_prompt = gtk_label_new("\n     请输入名称：");
    gtk_misc_set_alignment(GTK_MISC(lbl_prompt), 0.0, 0.5);

    GtkWidget *entry = gtk_entry_new();
    gtk_widget_set_size_request(entry, -1, 50);

    GtkWidget *sep = gtk_hseparator_new();

    /* Button row: [创建文件夹] [创建空文件]  <expand spacer>  [取消] */
    GtkWidget *hbox_btns  = gtk_hbox_new(FALSE, 5);
    GtkWidget *btn_mkdir  = gtk_button_new_with_label("创建文件夹");
    GtkWidget *btn_touch  = gtk_button_new_with_label("创建空文件");
    GtkWidget *spacer     = gtk_label_new("");
    GtkWidget *btn_cancel = gtk_button_new_with_label("取消");

    gtk_widget_set_size_request(btn_mkdir,  -1, 64);
    gtk_widget_set_size_request(btn_touch,  -1, 64);
    gtk_widget_set_size_request(btn_cancel, -1, 64);

    gtk_box_pack_start(GTK_BOX(hbox_btns), btn_mkdir,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_btns), btn_touch,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_btns), spacer,     TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(hbox_btns), btn_cancel, FALSE, FALSE, 0);

    g_object_set_data(G_OBJECT(btn_mkdir),  "resp-id", GINT_TO_POINTER(NEW_RESP_MKDIR));
    g_object_set_data(G_OBJECT(btn_touch),  "resp-id", GINT_TO_POINTER(NEW_RESP_TOUCH));
    g_object_set_data(G_OBJECT(btn_cancel), "resp-id",
                      GINT_TO_POINTER(GTK_RESPONSE_CANCEL));

    g_signal_connect(btn_mkdir,  "clicked", G_CALLBACK(on_create_btn_clicked), dlg);
    g_signal_connect(btn_touch,  "clicked", G_CALLBACK(on_create_btn_clicked), dlg);
    g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_create_btn_clicked), dlg);

    gtk_box_pack_start(GTK_BOX(content), lbl_prompt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), entry,      FALSE, FALSE, 5);
    gtk_box_pack_start(GTK_BOX(content), sep,        FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(content), hbox_btns,  FALSE, FALSE, 5);
    gtk_widget_show_all(content);

    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));

    if (resp == NEW_RESP_MKDIR || resp == NEW_RESP_TOUCH)
    {
        string name = gtk_entry_get_text(GTK_ENTRY(entry));
        /* trim leading/trailing spaces */
        while (!name.empty() && name[0] == ' ') name.erase(0, 1);
        while (!name.empty() && name[name.size()-1] == ' ')
            name.erase(name.size()-1);

        if (!name.empty())
        {
            string fullpath = GetFullLocalFilePath(name);
            if (resp == NEW_RESP_MKDIR)
            {
                if (mkdir(fullpath.c_str(), 0755) != 0)
                    win->MessageBox("\n  创建文件夹失败！\n  \"" + name + "\"    \n");
            }
            else
            {
                int fd = open(fullpath.c_str(), O_CREAT | O_WRONLY, 0644);
                if (fd >= 0)
                    close(fd);
                else
                    win->MessageBox("\n  创建文件失败！\n  \"" + name + "\"    \n");
            }
            fs->UpdateList();
        }
    }

    gtk_widget_destroy(dlg);
    CloseKeyboard();
}
/* -------------------------------------------------------------------- */

void ToggleMultiSelect(GtkWidget *widget, gpointer data)
{
    multiselect_mode = !multiselect_mode;
    fs->SetMultiSelectMode(multiselect_mode);
    if (multiselect_mode)
    {
        GdkColor black = {0, 0x0000, 0x0000, 0x0000};
        gtk_button_set_relief(GTK_BUTTON(widget), GTK_RELIEF_NORMAL);
        gtk_widget_modify_bg(widget, GTK_STATE_NORMAL,   &black);
        gtk_widget_modify_bg(widget, GTK_STATE_PRELIGHT, &black);
        gtk_widget_modify_bg(widget, GTK_STATE_ACTIVE,   &black);
        if (ms_icon_inverted)
            gtk_button_set_image(GTK_BUTTON(widget),
                                 gtk_image_new_from_pixbuf(ms_icon_inverted));
    }
    else
    {
        /* Restore flat white style — identical to all other toolbar buttons */
        GdkColor white = {0, 0xffff, 0xffff, 0xffff};
        gtk_button_set_relief(GTK_BUTTON(widget), GTK_RELIEF_NONE);
        gtk_widget_modify_bg(widget, GTK_STATE_NORMAL,   &white);
        gtk_widget_modify_bg(widget, GTK_STATE_PRELIGHT, &white);
        gtk_widget_modify_bg(widget, GTK_STATE_ACTIVE,   &white);
        if (ms_icon_normal)
            gtk_button_set_image(GTK_BUTTON(widget),
                                 gtk_image_new_from_pixbuf(ms_icon_normal));
    }
    UpdateButtons();
}

/* ---- Directory size calculation (async, cancellable) --------------- */

struct DirSizeCtx {
    string        dir_path;
    volatile bool cancelled;
    off_t         result;
    GtkWidget    *lbl_size;
    GtkWidget    *btn_calc;
};

static void dir_size_count(const string& path, DirSizeCtx *ctx)
{
    if (ctx->cancelled) return;
    tinydir_dir td;
    if (tinydir_open(&td, path.c_str()) == -1) return;
    while (td.has_next && !ctx->cancelled)
    {
        tinydir_file f;
        if (tinydir_readfile(&td, &f) == 0)
        {
            if (strcmp(f.name, ".") != 0 && strcmp(f.name, "..") != 0)
            {
                if (f.is_dir && !f.is_symlink)
                    dir_size_count(string(f.path), ctx);
                else if (!f.is_dir)
                    ctx->result += f._s.st_size;
            }
        }
        tinydir_next(&td);
    }
    tinydir_close(&td);
}

/* Runs in the main thread via g_idle_add after the worker thread finishes */
static gboolean dir_size_done_cb(gpointer data)
{
    DirSizeCtx *ctx = (DirSizeCtx*)data;
    if (!ctx->cancelled)
    {
        if (GTK_IS_WIDGET(ctx->lbl_size))
            gtk_label_set_text(GTK_LABEL(ctx->lbl_size),
                               format_size_auto(ctx->result).c_str());
        if (GTK_IS_WIDGET(ctx->btn_calc))
        {
            /* Clear stored pointer so future destroy signal is a no-op */
            g_object_set_data(G_OBJECT(ctx->btn_calc), "current-ctx", NULL);
            gtk_widget_set_sensitive(ctx->btn_calc, TRUE);
        }
    }
    delete ctx;
    return FALSE;
}

static void* dir_size_thread_func(void *data)
{
    DirSizeCtx *ctx = (DirSizeCtx*)data;
    dir_size_count(ctx->dir_path, ctx);
    g_idle_add(dir_size_done_cb, ctx); /* schedule UI update in main thread */
    return NULL;
}

/* Connected once to the dialog's "destroy" signal — cancels any running thread */
static void on_props_dlg_destroy(GtkWidget * /*widget*/, gpointer btn)
{
    DirSizeCtx *ctx = (DirSizeCtx*)g_object_get_data(G_OBJECT(btn), "current-ctx");
    if (ctx) ctx->cancelled = true;
}

static void on_calc_dir_size_clicked(GtkWidget *btn, gpointer /*data*/)
{
    GtkWidget  *lbl = GTK_WIDGET(g_object_get_data(G_OBJECT(btn), "size-label"));
    const char *dir = (const char*)g_object_get_data(G_OBJECT(btn), "dir-path");
    if (!lbl || !dir) return;

    gtk_widget_set_sensitive(btn, FALSE);
    gtk_label_set_text(GTK_LABEL(lbl), "计算中...");

    DirSizeCtx *ctx = new DirSizeCtx();
    ctx->dir_path  = string(dir);
    ctx->cancelled = false;
    ctx->result    = 0;
    ctx->lbl_size  = lbl;
    ctx->btn_calc  = btn;
    g_object_set_data(G_OBJECT(btn), "current-ctx", ctx);

    pthread_t tid;
    pthread_create(&tid, NULL, dir_size_thread_func, ctx);
    pthread_detach(tid); /* fire-and-forget; result delivered via g_idle_add */
}

/* -------------------------------------------------------------------- */

void ShowProperties(GtkWidget *widget, gpointer data)
{
    if (!fs->IsItemSelected()) return;
    FileItem sel = fs->GetSelectedItem();

    struct stat st;
    if (lstat(sel.name.c_str(), &st) != 0) return;

    /* type info */
    FileItem tmp_fi;
    tmp_fi.name = sel.name;
    tmp_fi.dir  = S_ISDIR(st.st_mode) != 0;
    tmp_fi.mode = st.st_mode;
    FileType ft = detect_file_type(tmp_fi);
    const char* type_str = "文件";
    const char* icon_name = "file.png";
    switch (ft) {
        case FT_DIR:    type_str = "目录";      icon_name = "folder.png"; break;
        case FT_IMAGE:  type_str = "图片";      icon_name = "image.png";  break;
        case FT_SCRIPT: type_str = "脚本";      icon_name = "script.png"; break;
        case FT_BINARY: type_str = "可执行程序"; icon_name = "binary.png"; break;
        case FT_EBOOK:  type_str = "电子书";    icon_name = "ebook.png";  break;
        default:        type_str = "文件";  icon_name = "file.png";   break;
    }

    /* owner / group */
    char owner_buf[64] = "?";
    char group_buf[64] = "?";
    struct passwd *pw = getpwuid(st.st_uid);
    if (pw) snprintf(owner_buf, sizeof(owner_buf), "%s", pw->pw_name);
    struct group  *gr = getgrgid(st.st_gid);
    if (gr) snprintf(group_buf, sizeof(group_buf), "%s", gr->gr_name);

    /* time strings */
    char atime_buf[32], mtime_buf[32];
    strftime(atime_buf, sizeof(atime_buf), "%Y-%m-%d %H:%M", localtime(&st.st_atime));
    strftime(mtime_buf, sizeof(mtime_buf), "%Y-%m-%d %H:%M", localtime(&st.st_mtime));

    string perms_str = format_perms(st.st_mode);
    string size_str  = format_size_auto(st.st_size);

    /* build dialog */
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "属性",
        GTK_WINDOW(win->GetWidget("wndMain")),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "确定", GTK_RESPONSE_CLOSE,
        NULL);
    gtk_window_set_title(GTK_WINDOW(dlg), "L:D_N:dialog_ID:net.tqhyg.explorer.Properties");

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *hbox_top = gtk_hbox_new(FALSE, 10);
    gtk_container_set_border_width(GTK_CONTAINER(hbox_top), 10);

    /* icon */
    GtkWidget *img = gtk_image_new_from_file(GetResFile(icon_name).c_str());
    gtk_box_pack_start(GTK_BOX(hbox_top), img, FALSE, FALSE, 0);

    /* info table */
    GtkWidget *table = gtk_table_new(8, 2, FALSE);
    gtk_table_set_col_spacings(GTK_TABLE(table), 12);
    gtk_table_set_row_spacings(GTK_TABLE(table), 4);

    /* rows 0-4: name, type, owner, group, perms */
    struct { const char* label; const char* value; } head_rows[] = {
        {"名称：",   sel.name.c_str()},
        {"类型：",   type_str},
        {"所有者：", owner_buf},
        {"所属组：", group_buf},
        {"权限：",   perms_str.c_str()},
    };
    for (int i = 0; i < 5; i++)
    {
        GtkWidget *lbl_key = gtk_label_new(head_rows[i].label);
        gtk_misc_set_alignment(GTK_MISC(lbl_key), 1.0, 0.5);
        GtkWidget *lbl_val = gtk_label_new(head_rows[i].value);
        gtk_misc_set_alignment(GTK_MISC(lbl_val), 0.0, 0.5);
        gtk_label_set_selectable(GTK_LABEL(lbl_val), TRUE);
        if (i == 0) /* 文件名可能很长，允许换行 */
        {
            gtk_label_set_line_wrap(GTK_LABEL(lbl_val), TRUE);
            gtk_widget_set_size_request(lbl_val, 200, -1);
        }
        gtk_table_attach_defaults(GTK_TABLE(table), lbl_key, 0, 1, i, i+1);
        gtk_table_attach_defaults(GTK_TABLE(table), lbl_val, 1, 2, i, i+1);
    }

    /* row 5: size — plain label for files, calc button for directories */
    GtkWidget *dir_calc_btn = NULL; /* remembered so we can wire cancel signal */
    {
        GtkWidget *lbl_key = gtk_label_new("大小：");
        gtk_misc_set_alignment(GTK_MISC(lbl_key), 1.0, 0.5);
        GtkWidget *size_widget;
        if (ft == FT_DIR)
        {
            GtkWidget *hbox_size = gtk_hbox_new(FALSE, 8);
            GtkWidget *lbl_size  = gtk_label_new("--");
            gtk_misc_set_alignment(GTK_MISC(lbl_size), 0.0, 0.5);
            GtkWidget *btn_calc  = gtk_button_new_with_label("计算大小");
            gtk_widget_set_size_request(btn_calc, -1, 44);
            g_object_set_data_full(G_OBJECT(btn_calc), "dir-path",
                                   g_strdup(GetFullLocalFilePath(sel.name).c_str()),
                                   g_free);
            g_object_set_data(G_OBJECT(btn_calc), "size-label", lbl_size);
            g_signal_connect(btn_calc, "clicked",
                             G_CALLBACK(on_calc_dir_size_clicked), NULL);
            gtk_box_pack_start(GTK_BOX(hbox_size), lbl_size, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(hbox_size), btn_calc, FALSE, FALSE, 0);
            size_widget    = hbox_size;
            dir_calc_btn   = btn_calc;
        }
        else
        {
            GtkWidget *lbl_size = gtk_label_new(size_str.c_str());
            gtk_misc_set_alignment(GTK_MISC(lbl_size), 0.0, 0.5);
            gtk_label_set_selectable(GTK_LABEL(lbl_size), TRUE);
            size_widget = lbl_size;
        }
        gtk_table_attach_defaults(GTK_TABLE(table), lbl_key,    0, 1, 5, 6);
        gtk_table_attach_defaults(GTK_TABLE(table), size_widget, 1, 2, 5, 6);
    }

    /* rows 6-7: mtime, atime */
    struct { const char* label; const char* value; } tail_rows[] = {
        {"修改时间：", mtime_buf},
        {"访问时间：", atime_buf},
    };
    for (int i = 0; i < 2; i++)
    {
        GtkWidget *lbl_key = gtk_label_new(tail_rows[i].label);
        gtk_misc_set_alignment(GTK_MISC(lbl_key), 1.0, 0.5);
        GtkWidget *lbl_val = gtk_label_new(tail_rows[i].value);
        gtk_misc_set_alignment(GTK_MISC(lbl_val), 0.0, 0.5);
        gtk_label_set_selectable(GTK_LABEL(lbl_val), TRUE);
        int r = 6 + i;
        gtk_table_attach_defaults(GTK_TABLE(table), lbl_key, 0, 1, r, r+1);
        gtk_table_attach_defaults(GTK_TABLE(table), lbl_val, 1, 2, r, r+1);
    }

    gtk_box_pack_start(GTK_BOX(hbox_top), table, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(content), hbox_top, TRUE, TRUE, 0);
    gtk_widget_show_all(content);

    /* If there is a directory calc button, connect cancel-on-close.
       The signal is connected once here regardless of how many times the
       button is clicked; on_props_dlg_destroy reads the current ctx from
       the button's "current-ctx" data, which is updated on each click. */
    if (dir_calc_btn)
        g_signal_connect(dlg, "destroy",
                         G_CALLBACK(on_props_dlg_destroy), dir_calc_btn);

    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* ---- image viewer helpers ----------------------------------------- */

static void UpdateImageDisplay()
{
    if (!img_state.original || !img_state.image_widget) return;
    int w = (int)(gdk_pixbuf_get_width(img_state.original)  * img_state.zoom);
    int h = (int)(gdk_pixbuf_get_height(img_state.original) * img_state.zoom);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(
        img_state.original, w, h, GDK_INTERP_BILINEAR);
    gtk_image_set_from_pixbuf(GTK_IMAGE(img_state.image_widget), scaled);
    g_object_unref(scaled);
}

void ImageZoomIn(GtkWidget *widget, gpointer data)
{
    img_state.zoom *= 1.5;
    UpdateImageDisplay();
}

void ImageZoomOut(GtkWidget *widget, gpointer data)
{
    img_state.zoom /= 1.5;
    if (img_state.zoom < 0.05) img_state.zoom = 0.05;
    UpdateImageDisplay();
}

void ImageZoomFit(GtkWidget *widget, gpointer data)
{
    img_state.zoom = img_state.fit_zoom;
    UpdateImageDisplay();
}

void OnImageViewerDestroy(GtkWidget *widget, gpointer data)
{
    if (img_state.original)
    {
        g_object_unref(img_state.original);
        img_state.original = NULL;
    }
    img_state.image_widget = NULL;
}

void EditTextFile(GtkWidget *widget, gpointer data)
{
    if (!FileExist(EDITOR))
    {
        win->MessageBox(string("找不到编辑器：LeafPad"));
        return;
    }
    Execute(string(EDITOR) + " " + text_viewer_filename);
}

/* --------------------------------------------------------------------- */

void OpenTextViewer(string fullpath, string filename)
{
    /* Read file content up-front. ShakeWindow functions can throw int on
       error (e.g. get_file_contents on unreadable or special files).
       Catching here lets us show a friendly message instead of aborting. */
    string txt;
    try {
        txt = get_file_contents(fullpath.c_str());
    } catch (...) {
        win->MessageBox("\n  无法读取文件内容。\n  文件可能无法访问。\n");
        return;
    }

    ShakeWindow *viewer = new ShakeWindow();
    viewer->Load(GetResFile("ViewText.glade"), false);
    viewer->SetCloseButton("btnClose", true);
    viewer->ApplyImage("btnClose", GetResFile("back.png"));
    viewer->ApplyImage("btnEdit",  GetResFile("edit.png"));
    viewer->SetModal(win);
    viewer->SetText("lblName", filename, "Tahoma 14");

    text_viewer_filename = fullpath;
    viewer->OnClick("btnEdit", EditTextFile);

    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    gtk_text_buffer_set_text(buffer, txt.c_str(), -1);
    gtk_text_view_set_buffer((GtkTextView*)viewer->GetWidget("txtData"), buffer);
    viewer->SetFont("txtData", "Tahoma 7");

    viewer->Show();
}

void OpenImageViewer(string fullpath, string filename)
{
    ShakeWindow *viewer = new ShakeWindow();
    viewer->Load(GetResFile("ViewImage.glade"), false);
    viewer->SetCloseButton("btnClose", true);
    viewer->ApplyImage("btnClose", GetResFile("back.png"));
    viewer->SetModal(win);
    viewer->SetText("lblName", filename, "Tahoma 14");

    GError *load_err = NULL;
    GdkPixbuf *orig = gdk_pixbuf_new_from_file(fullpath.c_str(), &load_err);
    if (orig)
    {
        int orig_w = gdk_pixbuf_get_width(orig);
        int orig_h = gdk_pixbuf_get_height(orig);
        int screen_w = gdk_screen_get_width(gdk_screen_get_default());
        int screen_h = gdk_screen_get_height(gdk_screen_get_default());
        double scale_w = (double)(screen_w - 30)  / orig_w;
        double scale_h = (double)(screen_h - 130) / orig_h;
        double fit_z = (scale_w < scale_h) ? scale_w : scale_h;
        if (fit_z > 1.0) fit_z = 1.0;

        GtkWidget *img = gtk_image_new();
        gtk_widget_show(img);
        gtk_container_add(GTK_CONTAINER(viewer->GetWidget("viewportMain")), img);

        img_state.original     = orig;
        img_state.image_widget = img;
        img_state.zoom         = fit_z;
        img_state.fit_zoom     = fit_z;
        UpdateImageDisplay();

        viewer->OnClick("btnZoomIn",  ImageZoomIn);
        viewer->OnClick("btnZoomOut", ImageZoomOut);
        viewer->OnClick("btnZoomFit", ImageZoomFit);

        g_signal_connect(viewer->GetWidget("wndViewImage"), "destroy",
                         G_CALLBACK(OnImageViewerDestroy), NULL);
    }
    else
    {
        GtkWidget *image = gtk_image_new_from_file(fullpath.c_str());
        gtk_container_add(GTK_CONTAINER(viewer->GetWidget("viewportMain")), image);
    }
    if (load_err) g_error_free(load_err);

    viewer->Show();
}

/* Script action dialog response IDs */
#define SCRIPT_RESP_PREVIEW 1
#define SCRIPT_RESP_EDIT    2
#define SCRIPT_RESP_EXEC    3

void ShowScriptDialog(string fullpath, string filename)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "脚本操作",
        GTK_WINDOW(win->GetWidget("wndMain")),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "预览", SCRIPT_RESP_PREVIEW,
        "编辑", SCRIPT_RESP_EDIT,
        "执行", SCRIPT_RESP_EXEC,
        "取消", GTK_RESPONSE_CANCEL,
        NULL);
    gtk_window_set_title(GTK_WINDOW(dlg), "L:D_N:dialog_ID:net.tqhyg.explorer.Script");

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);

    GtkWidget *hbox_info = gtk_hbox_new(FALSE, 12);

    /* Script icon */
    GtkWidget *icon_img = gtk_image_new_from_file(GetResFile("script.png").c_str());
    gtk_box_pack_start(GTK_BOX(hbox_info), icon_img, FALSE, FALSE, 0);

    /* Filename + subtitle */
    GtkWidget *vbox_text = gtk_vbox_new(FALSE, 4);
    GtkWidget *lbl_name  = gtk_label_new(filename.c_str());
    gtk_misc_set_alignment(GTK_MISC(lbl_name), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(lbl_name), TRUE);
    gtk_widget_set_size_request(lbl_name, 280, -1);
    GtkWidget *lbl_sub   = gtk_label_new("请选择要执行的操作                  ");
    gtk_misc_set_alignment(GTK_MISC(lbl_sub), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(vbox_text), lbl_name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_text), lbl_sub,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_info), vbox_text, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(content), hbox_info, TRUE, TRUE, 0);
    gtk_widget_show_all(content);

    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    if (resp == SCRIPT_RESP_PREVIEW)
        OpenTextViewer(fullpath, filename);
    else if (resp == SCRIPT_RESP_EDIT)
    {
        if (!FileExist(EDITOR))
            win->MessageBox(string("找不到编辑器：LeafPad"));
        else
            Execute(string(EDITOR) + " " + fullpath);
    }
    else if (resp == SCRIPT_RESP_EXEC)
        Execute(fullpath);
}

void ViewFile(GtkWidget *widget, gpointer data)
{
    if (!fs->IsItemSelected())
        return;
    FileItem sel = fs->GetSelectedItem();

    if (sel.dir)
    {
        chdir(sel.name.c_str());
        fs->UpdateList();
        return;
    }

    string fullpath = GetFullLocalFilePath(sel.name);

    /* Re-detect type using stat for correct mode bits */
    struct stat st;
    FileItem tmp_fi;
    tmp_fi.name = sel.name;
    tmp_fi.dir  = false;
    tmp_fi.mode = 0;
    if (lstat(sel.name.c_str(), &st) == 0)
        tmp_fi.mode = st.st_mode;
    FileType ft = detect_file_type(tmp_fi);

    switch (ft)
    {
        case FT_IMAGE:
            OpenImageViewer(fullpath, sel.name);
            break;
        case FT_SCRIPT:
            ShowScriptDialog(fullpath, sel.name);
            break;
        case FT_BINARY:
        case FT_EBOOK:
        case FT_OTHER:
            /* no action for non-text / non-viewable files */
            break;
        default:
            OpenTextViewer(fullpath, sel.name);
            break;
    }
}

void on_changed(GtkWidget *widget, gpointer label)
{
    UpdateButtons();
}

/* Intercept taps on the file list when in multi-select mode.
   Connected to button-press-event (runs before GTK's default selection handler). */
gboolean OnMultiSelectPress(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    if (!multiselect_mode) return FALSE;
    if (event->button != 1) return FALSE;

    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
                                        (gint)event->x, (gint)event->y,
                                        &path, NULL, NULL, NULL))
        return FALSE;

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
    if (gtk_tree_selection_path_is_selected(sel, path))
        gtk_tree_selection_unselect_path(sel, path);
    else
        gtk_tree_selection_select_path(sel, path);

    gtk_tree_path_free(path);
    UpdateButtons();
    return TRUE; /* prevent GTK's default single-select behavior */
}

gboolean ListCkicked(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    if (multiselect_mode) return FALSE; /* handled by OnMultiSelectPress */
    if (!fs->IsItemSelected())
        return FALSE;

    FileItem sel = fs->GetSelectedItem();

    if (!timer.IsStarted() || timer.GetDuration() > 0.7 || lastSel != sel.name)
    {
        timer.Start();
        lastSel = sel.name;
        return FALSE;
    }
    timer.Start();

    ViewFile(NULL, NULL);

    return TRUE;
}

void ScrollPageUp(GtkWidget *widget, gpointer data)
{
    GtkWidget *sw = win->GetWidget("scrolledwindow1");
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    gdouble new_value = vadj->value - vadj->page_size;
    if (new_value < vadj->lower) new_value = vadj->lower;
    gtk_adjustment_set_value(vadj, new_value);
}

void ScrollPageDown(GtkWidget *widget, gpointer data)
{
    GtkWidget *sw = win->GetWidget("scrolledwindow1");
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    gdouble new_value = vadj->value + vadj->page_size;
    gdouble max_value = vadj->upper - vadj->page_size;
    if (new_value > max_value) new_value = max_value;
    gtk_adjustment_set_value(vadj, new_value);
}

void BreadcrumbClick(GtkWidget *btn, gpointer data)
{
    const char *target = (const char*)g_object_get_data(G_OBJECT(btn), "nav_path");
    if (target)
    {
        chdir(target);
        fs->UpdateList();
    }
}

void AdaptSize()
{
    gint screenWidth = gdk_screen_get_width(gdk_screen_get_default());
    gint size = screenWidth / 12;
    if (size < 1000) // for small screens downsize buttons
    {
        vector<GtkWidget*> buttons = win->FindWidgetsByType("GtkButton");
        for(vector<GtkWidget*>::iterator it = buttons.begin(); it != buttons.end(); it++)
            win->ResizeWidget(*it, size, size);
    }
}

int main (int argc, char **argv)
{
    ShakeWindow::Initialize();
    gtk_rc_parse_string(
        "style \"kindle-scrollbar\" {\n"
        "  GtkScrollbar::slider-width = 40\n"
        "  GtkScrollbar::min-slider-length = 50\n"
        "}\n"
        "class \"GtkVScrollbar\" style \"kindle-scrollbar\"\n"
    );
    ShakeWindow::SetDefaultTitle("L:A_N:application_PC:T_ID:net.tqhyg.explorer");
    win = new ShakeWindow();
    win->Load(GetResFile("MainWindow.glade"), true);
    win->SetCloseButton("btnClose");

    win->OnClick("btnDirUp",       DirUp);
    win->OnClick("btnMultiSelect", ToggleMultiSelect);
    win->OnClick("btnAdd",         CreateNewItem);
    win->OnClick("btnCopy",        CopyFile);
    win->OnClick("btnRename",      RenameFile);
    win->OnClick("btnDelete",      DeleteFile);
    win->OnClick("btnProperties",  ShowProperties);
    win->OnClick("btnTerminal",    RunTerminal);
    win->OnClick("btnPaste",       PasteFile);
    win->OnClick("btnCancelCopy",  CancelCopy);

    win->ApplyImage("btnDirUp",       GetResFile("up.png"));
    win->ApplyImage("btnMultiSelect", GetResFile("multiselect.png"));
    win->ApplyImage("btnAdd",         GetResFile("add.png"));
    ms_icon_normal   = gdk_pixbuf_new_from_file(GetResFile("multiselect.png").c_str(), NULL);
    ms_icon_inverted = invert_pixbuf(ms_icon_normal);
    win->ApplyImage("btnCopy",        GetResFile("copy.png"));
    win->ApplyImage("btnRename",      GetResFile("rename.png"));
    win->ApplyImage("btnDelete",      GetResFile("delete.png"));
    win->ApplyImage("btnProperties",  GetResFile("properties.png"));
    win->ApplyImage("btnTerminal",    GetResFile("terminal.png"));
    win->ApplyImage("btnPaste",       GetResFile("paste.png"));
    win->ApplyImage("btnCancelCopy",  GetResFile("close.png"));
    win->ApplyImage("btnClose",       GetResFile("close.png"));

    AdaptSize();

    /* Initialize file list model AFTER AdaptSize so dynamically-created
       breadcrumb buttons are not caught by AdaptSize's button resize sweep */
    lstFiles = win->GetWidget("lstFiles");
    hboxBreadcrumb = win->GetWidget("hboxBreadcrumb");
    fs = new FilesModel();
    fs->Assign(lstFiles, hboxBreadcrumb, G_CALLBACK(BreadcrumbClick));
    hboxCopyBar = win->GetWidget("hboxCopyBar");

    /* Sort bar widgets */
    lblFileCount = win->GetWidget("lblFileCount");
    pbDiskUsage  = win->GetWidget("pbDiskUsage");
    lblDiskUsage = win->GetWidget("lblDiskUsage");
    /* Lock the disk-usage VBox to a fixed width (~15 chars at Sans 8).
       This prevents the label text length from widening the progress bar. */
    gtk_widget_set_size_request(win->GetWidget("vboxDiskUsage"), 300, -1);
    win->OnClick("btnSortField", CycleSortField);
    win->OnClick("btnSortDir",   ToggleSortDir);

    /* Sort bar appearance: small font, compact buttons, custom progress bar */
    {
        PangoFontDescription *sf = pango_font_description_from_string("Sans 8");

        /* Plain labels in the sort bar */
        gtk_widget_modify_font(win->GetWidget("lblSortPrefix"), sf);
        gtk_widget_modify_font(win->GetWidget("lblSortMiddle"), sf);
        gtk_widget_modify_font(lblFileCount,  sf);
        gtk_widget_modify_font(lblDiskUsage,  sf);

        /* Internal GtkLabel of each sort button (direct child for label-only buttons) */
        GtkWidget *btnSortField = win->GetWidget("btnSortField");
        GtkWidget *btnSortDir   = win->GetWidget("btnSortDir");
        GtkWidget *lbl;
        lbl = gtk_bin_get_child(GTK_BIN(btnSortField));
        if (lbl && GTK_IS_LABEL(lbl)) gtk_widget_modify_font(lbl, sf);
        lbl = gtk_bin_get_child(GTK_BIN(btnSortDir));
        if (lbl && GTK_IS_LABEL(lbl)) gtk_widget_modify_font(lbl, sf);

        pango_font_description_free(sf);

        /* Compact button height */
        gtk_widget_set_size_request(btnSortField, -1, 28);
        gtk_widget_set_size_request(btnSortDir,   -1, 28);
    }

    /* Progress bar: custom draw via expose-event (theme-independent) */
    g_signal_connect(pbDiskUsage, "expose-event", G_CALLBACK(pb_draw_cb), NULL);

    /* UpdateSortBar is called automatically after every UpdateList() */
    fs->on_list_updated = UpdateSortBar;

    // Wire custom vscrollbar to the scrolled window's vadjustment
    GtkWidget *scrolledWindow = win->GetWidget("scrolledwindow1");
    GtkWidget *vscrollbar = win->GetWidget("vscrollbar1");
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolledWindow));
    gtk_range_set_adjustment(GTK_RANGE(vscrollbar), vadj);
    gtk_widget_set_size_request(vscrollbar, 50, -1);

    // Use GTK native arrows for page buttons — renders reliably without font dependency
    GdkColor flat_bg = {0, 0xffff, 0xffff, 0xffff}; /* white */

    /* Apply flat white style to all toolbar buttons in hbox1 only.
       Breadcrumb (hboxBreadcrumb) and copy bar (hboxCopyBar) buttons
       are in different containers and are intentionally unaffected. */
    {
        GtkWidget *hbox1 = win->GetWidget("hbox1");
        GList *children = gtk_container_get_children(GTK_CONTAINER(hbox1));
        for (GList *l = children; l; l = l->next)
        {
            GtkWidget *child = GTK_WIDGET(l->data);
            if (GTK_IS_BUTTON(child))
            {
                gtk_button_set_relief(GTK_BUTTON(child), GTK_RELIEF_NONE);
                gtk_widget_modify_bg(child, GTK_STATE_NORMAL,   &flat_bg);
                gtk_widget_modify_bg(child, GTK_STATE_PRELIGHT, &flat_bg);
                gtk_widget_modify_bg(child, GTK_STATE_ACTIVE,   &flat_bg);
            }
        }
        g_list_free(children);
    }

    GtkWidget *btnPageUp = win->GetWidget("btnPageUp");
    GtkWidget *arrowUp = gtk_arrow_new(GTK_ARROW_UP, GTK_SHADOW_NONE);
    gtk_widget_set_size_request(arrowUp, 32, 32);
    gtk_widget_show(arrowUp);
    gtk_button_set_image(GTK_BUTTON(btnPageUp), arrowUp);
    gtk_button_set_relief(GTK_BUTTON(btnPageUp), GTK_RELIEF_NONE);
    gtk_widget_modify_bg(btnPageUp, GTK_STATE_NORMAL,   &flat_bg);
    gtk_widget_modify_bg(btnPageUp, GTK_STATE_PRELIGHT, &flat_bg);
    gtk_widget_modify_bg(btnPageUp, GTK_STATE_ACTIVE,   &flat_bg);

    GtkWidget *btnPageDown = win->GetWidget("btnPageDown");
    GtkWidget *arrowDown = gtk_arrow_new(GTK_ARROW_DOWN, GTK_SHADOW_NONE);
    gtk_widget_set_size_request(arrowDown, 32, 32);
    gtk_widget_show(arrowDown);
    gtk_button_set_image(GTK_BUTTON(btnPageDown), arrowDown);
    gtk_button_set_relief(GTK_BUTTON(btnPageDown), GTK_RELIEF_NONE);
    gtk_widget_modify_bg(btnPageDown, GTK_STATE_NORMAL,   &flat_bg);
    gtk_widget_modify_bg(btnPageDown, GTK_STATE_PRELIGHT, &flat_bg);
    gtk_widget_modify_bg(btnPageDown, GTK_STATE_ACTIVE,   &flat_bg);

    win->OnClick("btnPageUp", ScrollPageUp);
    win->OnClick("btnPageDown", ScrollPageDown);

    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(lstFiles)), "changed", G_CALLBACK(on_changed), NULL);
    g_signal_connect(G_OBJECT(lstFiles), "button-press-event",   G_CALLBACK(OnMultiSelectPress), NULL);
    g_signal_connect(G_OBJECT(lstFiles), "button_release_event", (GtkSignalFunc)ListCkicked, NULL);

    UpdateButtons();
    win->Show();
    gtk_widget_hide(hboxCopyBar);
    UpdateSortBar(); /* populate sort bar on first display */

    gtk_main();

    delete fs;
    return 0;
}
