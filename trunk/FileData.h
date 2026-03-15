#include <vector>
#include <fstream>
#include <string>
#include <cerrno>
#include <algorithm>
#include <sys/stat.h>
#include "tinydir.h"

#ifndef FILE_ITEM_H
#define FILE_ITEM_H

using namespace std;

enum FileType { FT_DIR, FT_IMAGE, FT_SCRIPT, FT_BINARY, FT_EBOOK, FT_TEXT, FT_OTHER };
enum SortField { SORT_NAME, SORT_SIZE, SORT_TYPE, SORT_MTIME };
enum SortDir   { SORT_ASC,  SORT_DESC };

typedef struct
{
    string name;
    bool dir;
    bool is_symlink;
    bool symlink_broken;
    mode_t mode;            /* from lstat: used for permissions display */
    off_t  size;            /* file size; for symlinks: target size; broken: 0 */
    time_t mtime;           /* modification time (target mtime for valid symlinks) */
    int dir_count;          /* for directories: item count (-1 if unavailable) */
    string symlink_target;  /* readlink() result; empty for non-symlinks */
    FileType file_type;     /* detected file type */
} FileItem;

#include <stdio.h>  /* defines FILENAME_MAX */
#ifdef WINDOWS
    #include <windows.h>
    #include <Shellapi.h>
    #define GetCurrentDir _getcwd
    #undef MessageBox
    #undef DeleteFile
    #undef CopyFile
    #define PATH_SEPARATOR '\\'
    string GetAppPath()
    {
        char buffer[MAX_PATH];
        GetModuleFileName( NULL, buffer, MAX_PATH );
        *strrchr(buffer, PATH_SEPARATOR) = 0;
        return string(buffer);
    }
    void Execute(string name, string workPath = "")
    {
        ShellExecute(NULL, (LPCTSTR)"open", name.c_str(), 0, 0, SW_SHOWNORMAL);
    }
    void CreateFolder(string name, int attr)
    {
        CreateDirectory(name.c_str(), NULL);
    }
    bool DirectoryExists(string path)
    {
      DWORD dwAttrib = GetFileAttributes(path.c_str());

      return (dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
    }
    void OpenKeyboard() {};
    void CloseKeyboard() {}
#else
    #include <unistd.h>
    #define GetCurrentDir getcwd
    #define PATH_SEPARATOR '/'
    std::string GetAppPath()
    {
        char buffer[2048];
        ssize_t len = ::readlink("/proc/self/exe", buffer, sizeof(buffer)-1);
        if (len != -1)
        {
            buffer[len] = '\0';
            *strrchr(buffer, PATH_SEPARATOR) = 0;
            return std::string(buffer);
        }
    }

    void Execute(string name, string workPath = "")
    {
        string work;

        char curdir[2048];
        GetCurrentDir(curdir, sizeof(curdir));

        if (name[0] != '/')
            name = "./" + name;
        int space = name.find(' ');
        if (space != string::npos)
            work = name.substr(0, space);
        else
            work = name;

        int dirpos = work.find_last_of(PATH_SEPARATOR);
        if (dirpos != string::npos)
            work = name.substr(0, dirpos);

        if (workPath.length() > 0)
            work = workPath;

        if (work.length() > 0)
        {
            chdir(work.c_str());
            system(("(cd " + work + "; exec " + name + ")").c_str());
            chdir(curdir);
        }
        else
        {
            system((name + " &").c_str());
        }
    }

    void CreateFolder(string name, int attr)
    {
        mkdir(name.c_str(), attr);
    }

    bool DirectoryExists(string path)
    {
        DIR *pDir;
        bool bExists = false;

        pDir = opendir (path.c_str());

        if (pDir != NULL)
        {
            bExists = true;
            closedir(pDir);
        }

        return bExists;
    }
    void OpenKeyboard()
    {
      switch(fork())
      {
        case 0:
          {
            execlp("/bin/sh", "sh", "-c", "/usr/bin/lipc-set-prop -s com.lab126.keyboard open net.fabiszewski.leafpad:Abc:1", NULL);
          }
        case -1:
          perror("Failed to open keyboard\n");
          exit(1);
      }
    }

    void CloseKeyboard()
    {
      switch(fork())
      {
        case 0:
          {
            execlp("/bin/sh", "sh", "-c", "/usr/bin/lipc-set-prop -s com.lab126.keyboard close net.fabiszewski.leafpad", NULL);
          }
        case -1:
          perror("Failed to open keyboard\n");
          exit(1);
      }
    }
 #endif

std::string truncate_filename(const std::string& filename, size_t max_len = 20) {
    if (filename.length() <= max_len) {
        return filename;
    }
    return filename.substr(0, max_len - 3) + "...";
}

bool FileExist(string fileName)
{
    FILE *file = fopen(fileName.c_str(), "r");
    if (file)
    {
        fclose(file);
        return 1;
    }
    return 0;
}

string GetFullLocalFilePath(string name)
{
    char curdir[2048];
    GetCurrentDir(curdir, sizeof(curdir));
    return string(curdir) + PATH_SEPARATOR + name;
}

string GetFileName(string file)
{
    guint dirpos = file.find_last_of(PATH_SEPARATOR);
    if (dirpos != string::npos)
        return file.substr(dirpos + 1);
    else
        return file;
}

string GetFilePath(string file)
{
    guint dirpos = file.find_last_of(PATH_SEPARATOR);
    if (dirpos != string::npos)
        return file.substr(0, dirpos);
    else
        return file;
}

std::string GetAppFile(string file)
{
    return GetAppPath() + PATH_SEPARATOR + string(file);
}

std::string GetResFile(string file)
{
    return GetAppFile("res" + (PATH_SEPARATOR + string(file)));
}

bool sorter(FileItem i, FileItem j) { if (i.dir != j.dir) return i.dir > j.dir; else return i.name < j.name; }

static string get_extension(const string& name)
{
    size_t pos = name.find_last_of('.');
    if (pos == string::npos || pos == 0) return "";
    return name.substr(pos + 1);
}

static bool compare_file_items(const FileItem& a, const FileItem& b,
                                SortField field, SortDir dir)
{
    /* Directories always come before files */
    if (a.dir != b.dir) return a.dir > b.dir;

    bool asc = (dir == SORT_ASC);
    switch (field)
    {
        case SORT_SIZE:
            if (a.dir)
            {
                /* For size sort, dirs are ordered by name ascending (fixed) */
                return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
            }
            return asc ? (a.size < b.size) : (a.size > b.size);

        case SORT_TYPE:
        {
            string ea = get_extension(a.name);
            string eb = get_extension(b.name);
            int ecmp  = strcasecmp(ea.c_str(), eb.c_str());
            if (ecmp != 0)
                return asc ? (ecmp < 0) : (ecmp > 0);
            /* Same extension: sort by name with direction */
            int ncmp = strcasecmp(a.name.c_str(), b.name.c_str());
            return asc ? (ncmp < 0) : (ncmp > 0);
        }

        case SORT_MTIME:
            return asc ? (a.mtime < b.mtime) : (a.mtime > b.mtime);

        case SORT_NAME:
        default:
        {
            int ncmp = strcasecmp(a.name.c_str(), b.name.c_str());
            return asc ? (ncmp < 0) : (ncmp > 0);
        }
    }
}

static string format_perms(mode_t mode)
{
    char p[11];
    if      (S_ISLNK(mode))  p[0] = 'l';
    else if (S_ISDIR(mode))  p[0] = 'd';
    else if (S_ISREG(mode))  p[0] = '-';
    else                     p[0] = '?';
    p[1] = (mode & S_IRUSR) ? 'r' : '-';
    p[2] = (mode & S_IWUSR) ? 'w' : '-';
    p[3] = (mode & S_IXUSR) ? 'x' : '-';
    p[4] = (mode & S_IRGRP) ? 'r' : '-';
    p[5] = (mode & S_IWGRP) ? 'w' : '-';
    p[6] = (mode & S_IXGRP) ? 'x' : '-';
    p[7] = (mode & S_IROTH) ? 'r' : '-';
    p[8] = (mode & S_IWOTH) ? 'w' : '-';
    p[9] = (mode & S_IXOTH) ? 'x' : '-';
    p[10] = '\0';
    return string(p);
}

static string format_size_auto(off_t size)
{
    char buf[32];
    if      (size < 1024LL)              snprintf(buf, sizeof(buf), "%lldB",  (long long)size);
    else if (size < 1024LL * 1024)       snprintf(buf, sizeof(buf), "%.2fK",  size / 1024.0);
    else if (size < 1024LL * 1024*1024)  snprintf(buf, sizeof(buf), "%.2fM",  size / (1024.0*1024));
    else                                 snprintf(buf, sizeof(buf), "%.2fG",  size / (1024.0*1024*1024));
    return string(buf);
}

static int count_dir_items(const char* dirpath)
{
    DIR *d = opendir(dirpath);
    if (!d) return -1;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
        {
            count++;
            if (count >= 9999) { count = 9999; break; } /* safety cap */
        }
    }
    closedir(d);
    return count;
}

bool CheckExtension(string list, string fileName)
{
    list += ";";
    string ext = fileName.substr(fileName.find_last_of(".") + 1);
    return list.find(ext + ";") != std::string::npos;
}

static FileType detect_file_type(const FileItem& fi)
{
    if (fi.dir) return FT_DIR;
    /* Only open files for magic inspection if they are regular files.
       Broken symlinks, devices, pipes, and sockets have non-regular mode
       bits from lstat; opening them could block or fail unpredictably. */
    if (!S_ISREG(fi.mode)) return FT_OTHER;
    const string& name = fi.name;
    if (CheckExtension("bmp;png;gif;ico;jpg;jpeg;wmf;tga", name)) return FT_IMAGE;
    if (CheckExtension("epub;mobi;azw;azw3;prc;pdf;kfx;", name))       return FT_EBOOK;
    /* ELF binary detection */
    {
        FILE* f = fopen(name.c_str(), "rb");
        if (f) {
            unsigned char hdr[4] = {0, 0, 0, 0};
            fread(hdr, 1, 4, f);
            fclose(f);
            if (hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F')
                return FT_BINARY;
        }
    }
    /* Script: exec bit set AND (shebang OR script extension) */
    bool has_exec = (fi.mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
    bool script_ext = CheckExtension("sh;bash;py;pl;rb;lua", name);
    bool has_shebang = false;
    {
        FILE* f = fopen(name.c_str(), "r");
        if (f) {
            char buf[3] = {0, 0, 0};
            fread(buf, 1, 2, f);
            fclose(f);
            has_shebang = (buf[0] == '#' && buf[1] == '!');
        }
    }
    if (has_exec && (has_shebang || script_ext)) return FT_SCRIPT;
    return FT_TEXT;
}

static vector<FileItem> GetFiles(SortField sf = SORT_NAME, SortDir sd = SORT_ASC)
{
    vector<FileItem> result;

    tinydir_dir dir;
	if (tinydir_open(&dir, ".") == -1)
	{
		perror("Error opening file");
		tinydir_close(&dir);
		return result;
	}

	while (dir.has_next)
	{
		tinydir_file file;
		if (tinydir_readfile(&dir, &file) == -1)
		{
			perror("Error getting file");
			tinydir_next(&dir);
			continue;
		}

        FileItem fit;
        fit.name        = file.name;
        fit.dir         = file.is_dir;
        fit.is_symlink  = file.is_symlink;
        fit.mode        = file._s.st_mode;
        fit.symlink_broken = false;
        fit.size        = 0;
        fit.mtime       = file._s.st_mtime;  /* lstat mtime; may be overridden below */
        fit.dir_count   = -1;

        if (fit.is_symlink)
        {
            /* Read the link target path */
            char link_buf[2048];
            ssize_t link_len = readlink(file.path, link_buf, sizeof(link_buf) - 1);
            if (link_len > 0)
            {
                link_buf[link_len] = '\0';
                fit.symlink_target = string(link_buf);
            }

            struct stat tgt;
            if (stat(file.path, &tgt) == 0)
            {
                fit.size  = tgt.st_size;
                fit.mtime = tgt.st_mtime;
                if (fit.dir)
                    fit.dir_count = count_dir_items(file.path);
            }
            else
            {
                fit.symlink_broken = true;
            }
        }
        else if (fit.dir)
        {
            fit.dir_count = count_dir_items(file.path);
        }
        else
        {
            fit.size = file._s.st_size;
        }

		if (fit.name != "." && fit.name != "..")
        {
            fit.file_type = detect_file_type(fit);
            result.push_back(fit);
        }

		tinydir_next(&dir);
	}

    tinydir_close(&dir);
    std::sort(result.begin(), result.end(),
              [sf, sd](const FileItem& a, const FileItem& b) {
                  return compare_file_items(a, b, sf, sd);
              });
    return result;
}

static bool RemovePath(string path)
{
    tinydir_dir dir;
	if (tinydir_open(&dir, path.c_str()) == -1)
	{
	    if (remove(path.c_str()) == 0)
            return true; // file

		perror("Error opening dir");
		tinydir_close(&dir);
		return false;
	}

    ShakeWindow::DoEvents();

	while (dir.has_next)
	{
		tinydir_file file;
		if (tinydir_readfile(&dir, &file) == -1)
		{
			perror("Error getting file");
			tinydir_next(&dir);
			continue;
		}

        FileItem fit;
        fit.name = file.name;
        fit.dir = file.is_dir;

		if (fit.name != "." && fit.name != "..")
        {
            string it = path + PATH_SEPARATOR + fit.name;
            if (fit.dir)
                RemovePath(it);
            else
                remove(it.c_str());
        }

		tinydir_next(&dir);
	}

    tinydir_close(&dir);
    return rmdir(path.c_str()) == 0;
}

bool CopyLocalFile(string from, string to)
{
    size_t BUFFER_SIZE = 4096;
    char buf[BUFFER_SIZE];
    size_t size;

    FILE* source = fopen(from.c_str(), "rb");
    if (source == NULL) return false;
    FILE* dest = fopen(to.c_str(), "wb");
    if (dest == NULL) return false;

    while (true)
    {
        size = fread(buf, 1, BUFFER_SIZE, source);
        if (size <= 0) break;
        fwrite(buf, 1, size, dest);
    }

    fclose(source);
    fclose(dest);
    return true;
}

static bool CopyPath(string from, string todir)
{
    std::string display_name_from = truncate_filename(from, 25);
    std::string display_name_todir = truncate_filename(todir, 25);
    if (!DirectoryExists(from))
    {
        if (FileExist(from))
            return CopyLocalFile(from, todir);
        else
        {
            (new ShakeWindow())->MessageBox("\n  错误\n\n  以下文件未找到            \n  " + display_name_from + "  \n");
            return false;
        }
    }

    if (!DirectoryExists(todir))
        CreateFolder(todir.c_str(), 766);

    tinydir_dir dir;
	if (tinydir_open(&dir, from.c_str()) == -1)
	{
		(new ShakeWindow())->MessageBox("\n  错误\n\n  无法打开以下目录               \n  " + display_name_todir + "  \n");
		tinydir_close(&dir);
		return false;
	}

    ShakeWindow::DoEvents();

	while (dir.has_next)
	{
		tinydir_file file;
        std::string display_name = truncate_filename(file.name, 20);
		if (tinydir_readfile(&dir, &file) == -1)
		{
		    (new ShakeWindow())->MessageBox(string("\n  错误\n\n  无法打开以下文件             \n  ") + display_name + string("  \n"));
			tinydir_next(&dir);
			continue;
		}

        FileItem fit;
        fit.name = file.name;
        fit.dir = file.is_dir;

		if (fit.name != "." && fit.name != "..")
        {
            string f1 = from + PATH_SEPARATOR + fit.name;
            string f2 = todir + PATH_SEPARATOR + fit.name;
            if (fit.dir)
                CopyPath(f1, f2);
            else if (!FileExist(f2))
                CopyLocalFile(f1, f2);
        }

		tinydir_next(&dir);
	}

    tinydir_close(&dir);
    return true;
}

std::string get_file_contents(const char *filename)
{
  std::ifstream in(filename, std::ios::in | std::ios::binary);
  if (in)
  {
    std::string contents;
    in.seekg(0, std::ios::end);
    contents.resize(in.tellg());
    in.seekg(0, std::ios::beg);
    in.read(&contents[0], contents.size());
    in.close();
    return(contents);
  }
  throw(errno);
}

#endif // END
