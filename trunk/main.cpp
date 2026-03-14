#include <stdlib.h>
#include <stdio.h>
#include <vector>
#include <unistd.h>
#include <string>
#include <gtk/gtk.h>
#include "../KindleLib/ShakeWindow.h"
#include "FSModel.h"
#include "timer.h"

#define KE_VERSION "2.0"
#define EDITOR "/mnt/us/extensions/leafpad/bin/leafpad"
#define TERMINAL "/mnt/us/extensions/kterm/bin/kterm"

using namespace std;

ShakeWindow *win;
GtkWidget *lstFiles, *hboxBreadcrumb;
FilesModel *fs;
Timer timer; string lastSel;
string copied;
const char* imageFormats = "bmp;png;gif;ico;jpg;jpeg;wmf;tga";


void UpdateButtons()
{
    bool sel = fs->IsItemSelected();

    win->Enable("btnView", sel);
    win->Enable("btnEdit", sel);
    win->Enable("btnExecute", sel);

    win->Enable("btnCopy", sel);
    win->Enable("btnRename", sel);
    win->Enable("btnDelete", sel);

    win->Enable("btnPaste", copied.length() > 0);
}

void DirUp(GtkWidget *widget, gpointer data)
{
    chdir("..");
    fs->UpdateList();
}

void ExecuteFile(GtkWidget *widget, gpointer data)
{
    if (!fs->IsItemSelected())
        return;
    FileItem sel = fs->GetSelectedItem();
    if (sel.dir)
        return;

    Execute(sel.name);
}

void EditFile(GtkWidget *widget, gpointer data)
{
    if (!fs->IsItemSelected())
        return;
    FileItem sel = fs->GetSelectedItem();
    if (sel.dir)
        return;

    if (!FileExist(EDITOR))
    {
        win->MessageBox(string("找不到编辑器：LeafPad"));
        return;
    }

    Execute(string(EDITOR) + " " + GetFullLocalFilePath(sel.name));
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
    if (!fs->IsItemSelected())
        return;
    FileItem sel = fs->GetSelectedItem();
    
    std::string display_name = truncate_filename(sel.name, 25);

    string msg = string("\n  删除\n\n  确定要删除以下文件吗？\n                                                             \n   \"") + display_name + "\"    \n";
    if (sel.dir)
        msg = "\n  删除\n\n  确定要删除以下目录及其内部所有文件吗？\n                                                                        \n   \"" + display_name + "\"    \n";

    gint res = win->MessageBox(msg, GTK_BUTTONS_YES_NO);
    if (res == GTK_RESPONSE_YES)
    {
        WaitWindow* wait = new WaitWindow(win);
        wait->Show();
        RemovePath(GetFullLocalFilePath(sel.name));
        fs->UpdateList();
        delete wait;
    }
}

void CopyFile(GtkWidget *widget, gpointer data)
{
    if (!fs->IsItemSelected())
        return;
    FileItem sel = fs->GetSelectedItem();
    copied = GetFullLocalFilePath(sel.name);
    //win->MessageBox("Clipboard item: " + copied);
    fs->UpdateList();
}

void PasteFile(GtkWidget *widget, gpointer data)
{
    if (copied.length() == 0)
        return;

    string to = GetFullLocalFilePath(GetFileName(copied));
    if (to == copied)
        to += "_copy";
    else if (to.find(copied) != string::npos)
    {
        std::string display_name_copied = truncate_filename(copied, 25);
        std::string display_name_to = truncate_filename(copied, 25);
        win->MessageBox("\n  错误\n\n  无法复制文件！                                 \n " + display_name_copied + "\n 到 \n" + display_name_to + "\n\n  目标与源文件冲突了！");
        return;
    }

    WaitWindow* wait = new WaitWindow(win);
    wait->Show();

    CopyPath(copied, to);
    copied = "";
    fs->UpdateList();

    delete wait;
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

    bool isImage = CheckExtension(imageFormats, sel.name);
    string layout = "";

    if (isImage)
        layout = "ViewImage.glade";
    else
        layout = "ViewText.glade";

    ShakeWindow *viewer = new ShakeWindow();
    viewer->Load(GetResFile(layout), false);
    viewer->SetCloseButton("btnClose", true);
    viewer->ApplyImage("btnClose", GetResFile("back.png"));
    viewer->SetModal(win);
    viewer->SetText("lblName", sel.name, "Tahoma 14");

    if (isImage)
    {
        GtkWidget *image = gtk_image_new_from_file(sel.name.c_str());
        gtk_container_add(GTK_CONTAINER(viewer->GetWidget("viewportMain")), image);
    }
    else
    {
        string txt = get_file_contents(sel.name.c_str());
        GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
        gtk_text_buffer_set_text(buffer, txt.c_str(), -1);
        gtk_text_view_set_buffer((GtkTextView*)viewer->GetWidget("txtData"), buffer);
        viewer->SetFont("txtData", "Tahoma 7");
    }

    viewer->Show();
}

void ShowAbout(GtkWidget *widget, gpointer data)
{
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(GetResFile("program.png").c_str(), NULL);

  GtkWidget *dialog = gtk_about_dialog_new();
  gtk_about_dialog_set_name(GTK_ABOUT_DIALOG(dialog), "KindleExplorer");
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), KE_VERSION);
  gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog), "Developer: anakod&TQHYG");
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), "快速的文件管理器");
  //gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "");
  gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(dialog), pixbuf);
  g_object_unref(pixbuf), pixbuf = NULL;
  gtk_window_set_title(GTK_WINDOW(dialog), "L:D_N:dialog_ID:net.tqhyg.explorer.About");
  gtk_window_set_position (GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
  gtk_dialog_run(GTK_DIALOG (dialog));
  gtk_widget_destroy(dialog);
}

void on_changed(GtkWidget *widget, gpointer label)
{
    UpdateButtons();
}

gboolean ListCkicked(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
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

    if (sel.dir)
    {
        chdir(sel.name.c_str());
        fs->UpdateList();
    }

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

    win->OnClick("btnDirUp", DirUp);
    win->OnClick("btnView", ViewFile);
    win->OnClick("btnExecute", ExecuteFile);
    win->OnClick("btnEdit", EditFile);
    win->OnClick("btnTerminal", RunTerminal);

    win->OnClick("btnRename", RenameFile);
    win->OnClick("btnCopy", CopyFile);
    win->OnClick("btnPaste", PasteFile);
    win->OnClick("btnDelete", DeleteFile);
    win->OnClick("btnAbout", ShowAbout);

    win->ApplyImage("btnDirUp", GetResFile("up.png"));
    win->ApplyImage("btnView", GetResFile("view.png"));
    win->ApplyImage("btnExecute", GetResFile("execute.png"));
    win->ApplyImage("btnEdit", GetResFile("edit.png"));
    win->ApplyImage("btnTerminal", GetResFile("terminal.png"));
    win->ApplyImage("btnRename", GetResFile("rename.png"));
    win->ApplyImage("btnCopy", GetResFile("copy.png"));
    win->ApplyImage("btnPaste", GetResFile("paste.png"));
    win->ApplyImage("btnDelete", GetResFile("delete.png"));
    win->ApplyImage("btnAbout", GetResFile("about.png"));
    win->ApplyImage("btnClose", GetResFile("close.png"));

    AdaptSize();

    /* Initialize file list model AFTER AdaptSize so dynamically-created
       breadcrumb buttons are not caught by AdaptSize's button resize sweep */
    lstFiles = win->GetWidget("lstFiles");
    hboxBreadcrumb = win->GetWidget("hboxBreadcrumb");
    fs = new FilesModel();
    fs->Assign(lstFiles, hboxBreadcrumb, G_CALLBACK(BreadcrumbClick));

    // Wire custom vscrollbar to the scrolled window's vadjustment
    GtkWidget *scrolledWindow = win->GetWidget("scrolledwindow1");
    GtkWidget *vscrollbar = win->GetWidget("vscrollbar1");
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolledWindow));
    gtk_range_set_adjustment(GTK_RANGE(vscrollbar), vadj);
    gtk_widget_set_size_request(vscrollbar, 50, -1);

    // Use GTK native arrows for page buttons — renders reliably without font dependency
    GtkWidget *arrowUp = gtk_arrow_new(GTK_ARROW_UP, GTK_SHADOW_NONE);
    gtk_widget_set_size_request(arrowUp, 32, 32);
    gtk_widget_show(arrowUp);
    gtk_button_set_image(GTK_BUTTON(win->GetWidget("btnPageUp")), arrowUp);

    GtkWidget *arrowDown = gtk_arrow_new(GTK_ARROW_DOWN, GTK_SHADOW_NONE);
    gtk_widget_set_size_request(arrowDown, 32, 32);
    gtk_widget_show(arrowDown);
    gtk_button_set_image(GTK_BUTTON(win->GetWidget("btnPageDown")), arrowDown);

    win->OnClick("btnPageUp", ScrollPageUp);
    win->OnClick("btnPageDown", ScrollPageDown);

    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(lstFiles)), "changed", G_CALLBACK(on_changed), NULL);
    g_signal_connect(G_OBJECT(lstFiles), "button_release_event", (GtkSignalFunc)ListCkicked, NULL);

    UpdateButtons();
    win->Show();

    gtk_main();

    delete fs;
    return 0;
}
