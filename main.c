
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

int verbose = 1;

#define LVL_ERROR       -1
#define LVL_NORMAL      0
#define LVL_VERBOSE     1
#define LVL_DEBUG       2

#define p(level, ...)  do {             \
    if (level == LVL_ERROR)             \
    {                                   \
        fprintf (stderr, __VA_ARGS__);  \
    }                                   \
    else if (verbose >= level)          \
    {                                   \
        fprintf (stdout, __VA_ARGS__);  \
    }                                   \
} while (0)

typedef struct
{
    const char **dirs;
    int          alloc;
    int          len;
} dirs_t;

typedef struct _files_t
{
    char            *name;
    struct _files_t *next;
} files_t;

char *
trim (char *str)
{
    char *start, *end;
    
    for (start = str; *start == ' ' || *start == '\t'; ++start)
        ;
    
    end = NULL;
    for (str = start; *str; ++str)
    {
        if (*str != ' ' && *str != '\t' && *str != '\n')
        {
            end = NULL;
        }
        else if (!end)
        {
            end = str;
        }
    }
    if (end)
    {
        *end = '\0';
    }
    return start;
}

typedef enum {
    PARSE_OK        = 0,
    PARSE_ABORTED,
    PARSE_FAILED,
} parse_t;

void
parse_file (char *file)
{
    struct stat statbuf;
    FILE       *fp;
    
    /* get the file size */
    if (stat (file, &statbuf) != 0)
    {
        p (LVL_ERROR, "%s: unable to stat file\n", file);
        return;
    }
    
    if (!(fp = fopen (file, "r")))
    {
        p (LVL_ERROR, "%s: unable to open file\n", file);
        return;
    }
    
    char  buf[4096];
    char *data;
    int   l;
    
    char   *line;
    char   *s;
    int     line_nb     = 0;
    int     in_section  = 0;
    char   *key;
    char   *value;
    parse_t state       = PARSE_OK;
    
    char *icon          = NULL;
    int   hidden        = 0;
    char *only_in       = NULL;
    char *not_in        = NULL;
    char *try_exec      = NULL;
    char *exec          = NULL;
    char *path          = NULL;
    int   terminal      = 0;
    
    /* can we read the whole file in buf, or do we need to allocate memory? */
    if (statbuf.st_size < 4)
    {
        data = buf;
    }
    else
    {
        /* +2: 1 for extra LF; 1 for NUL */
        data = malloc (sizeof (*data) * (statbuf.st_size + 2));
        *data = '\0';
    }
    p (LVL_DEBUG, "read file (%d bytes)\n", statbuf.st_size);
    fread (data, statbuf.st_size, 1, fp);
    fclose (fp);
    strcat (data, "\n");
    
    /* now do the parsing */
    line = data;
    p (LVL_DEBUG, "start parsing\n");
    while ((s = strchr (line, '\n')))
    {
        *s = '\0';
        ++line_nb;
        p (LVL_DEBUG, "line %d: %s\n", line_nb, line);
        line = trim (line);
        
        /* ignore comments & empty lines */
        if (*line == '#' || *line == '\0')
        {
            goto next;
        }
        
        /* we only support section "Desktop Entry" */
        if (*line == '[' && (l = strlen (line)) && line[l - 1] == ']')
        {
            in_section = (strcmp ("Desktop Entry]", line + 1) == 0);
        }
        else if (in_section)
        {
            if (!(value = strchr (line, '=')))
            {
                p (LVL_ERROR, "%s: syntax error (missing =) line %d\n",
                   file, line_nb);
                goto next;
            }
            
            *value = '\0';
            key = trim (line);
            value = trim (value + 1);
            
            if (strcmp (key, "Type") == 0)
            {
                if (strcmp (value, "Application") != 0)
                {
                    p (LVL_ERROR, "%s: invalid type line %d: %s\n",
                       file, line_nb, value);
                    state = PARSE_FAILED;
                }
            }
            else if (strcmp (key, "Hidden") == 0)
            {
                if (strcmp (value, "true") == 0)
                {
                    hidden = 1;
                    p (LVL_VERBOSE, "auto-start disabled (Hidden)\n");
                    state = PARSE_ABORTED;
                }
                else if (strcmp (value, "false") != 0)
                {
                    p (LVL_ERROR, "%s: invalid value for %s line %d: %s\n",
                       file, key, line_nb, value);
                    state = PARSE_FAILED;
                }
            }
            else if (strcmp (key, "Exec") == 0)
            {
                p (LVL_VERBOSE, "%s set to %s\n", key, value);
                exec = value;
            }
            else if (strcmp (key, "TryExec") == 0)
            {
                p (LVL_VERBOSE, "%s set to %s\n", key, value);
                try_exec = value;
            }
            else if (strcmp (key, "OnlyShowIn") == 0)
            {
                p (LVL_VERBOSE, "%s set to %s\n", key, value);
                only_in = value;
            }
            else if (strcmp (key, "NotShowIn") == 0)
            {
                p (LVL_VERBOSE, "%s set to %s\n", key, value);
                not_in = value;
            }
            else if (strcmp (key, "Icon") == 0)
            {
                p (LVL_VERBOSE, "%s set to %s\n", key, value);
                icon = value;
            }
            else if (strcmp (key, "Path") == 0)
            {
                p (LVL_VERBOSE, "%s set to %s\n", key, value);
                path = value;
            }
            else if (strcmp (key, "Terminal") == 0)
            {
                if (strcmp (value, "true") == 0)
                {
                    terminal = 1;
                    p (LVL_VERBOSE, "set to be run in terminal\n");
                }
                else if (strcmp (value, "false") != 0)
                {
                    p (LVL_ERROR, "%s: invalid value for %s line %d: %s\n",
                       file, key, line_nb, value);
                    state = PARSE_FAILED;
                }
            }
        }
        else
        {
            p (LVL_DEBUG, "not in section Desktop Entry, done\n");
            state = PARSE_ABORTED;
        }
next:
        if (state != PARSE_OK)
        {
            p (LVL_DEBUG, "stop parsing\n");
            break;
        }
        line = s + 1;
    }
    p (LVL_VERBOSE, "parsing completed\n");
    
    if (data != buf)
    {
        free (data);
    }
    
    if (state == PARSE_OK || state == PARSE_ABORTED)
    {
        if (hidden)
        {
            p (LVL_VERBOSE, "no auto-start to perform\n", file);
            return;
        }
        
        printf("icon=%s\nonly=%s\nnot=%s\ntry=%s\nexec=%s\npath=%s\nterm=%d\n",
               icon, only_in, not_in, try_exec, exec, path, terminal);
    }
}

void
process_dir (dirs_t *dirs, files_t **files, const char *dir)
{
    int i;
    
    /* make sure this dir hasn't been processed already. This is in case e.g.
     * XDG_CONFIG_DIRS was set to /etc/xdg:/etc/xdg:/etc/xdg */
    for (i = 0; i < dirs->len; ++i)
    {
        if (strcmp (dirs->dirs[i], dir) == 0)
        {
            p (LVL_VERBOSE, "%s/autostart: already processed, skip\n");
            return;
        }
    }
    if (dirs->len == dirs->alloc)
    {
        dirs->alloc += 10;
        dirs->dirs = realloc (dirs->dirs, sizeof (*dirs->dirs) * dirs->alloc);
    }
    dirs->dirs[dirs->len++] = strdup (dir);
    
    DIR           *dp;
    struct dirent *dirent;
    char           buf[4096];
    int            l;
    char          *path;
    int            len_path;
    
    l = snprintf (buf, 4096, "%s/autostart", dir);
    if (l < 4096)
    {
        path = buf;
    }
    else
    {
        path = malloc (sizeof (*path) * (l + 1));
        snprintf (path, l, "%s/autostart", dir);
    }
    
    len_path = strlen (path);
    p (LVL_VERBOSE, "open folder %s\n", path);
    dp = opendir (path);
    while ((dirent = readdir (dp)))
    {
        if (!(dirent->d_type & DT_REG))
        {
            /* ignore directory, etc -- symlinks to file are NOT ignored */
            p (LVL_DEBUG, "\n%s: not a file, ignoring\n", dirent->d_name);
            continue;
        }

        l = strlen (dirent->d_name);
        /* 8 == strlen (".desktop") */
        if (l < 8 || strcmp (".desktop", &dirent->d_name[l - 8]) != 0)
        {
            /* ignore anything not .desktop */
            p (LVL_DEBUG, "\n%s: not named *.desktop, ignoring\n", dirent->d_name);
            continue;
        }
        
        files_t  *f;
        files_t  *last;
        files_t  *file;
        int       process;
        char     *s;
        
        /* make sure we don't already have this item (from a previous dir) */
        process = 1;
        for (f = *files; f; f = f->next)
        {
            last = f;
            if (strcmp (dirent->d_name, f->name) == 0)
            {
                process = 0;
                p (LVL_VERBOSE, "\n%s: name already processed, ignoring\n",
                   dirent->d_name);
                break;
            }
        }
        
        if (process)
        {
            p (LVL_VERBOSE, "\n%s: processing\n", dirent->d_name);
            
            file = malloc (sizeof (*file));
            file->name = strdup (dirent->d_name);
            file->next = NULL;
            
            char  buf2[4096];
            char *s2;
            l = snprintf (buf2, 4096, "%s/%s", path, dirent->d_name);
            if (l < 4096)
            {
                s2 = buf2;
            }
            else
            {
                s2 = malloc (sizeof (*s2) * (l + 1));
                snprintf (s2, l, "%s/%s", path, dirent->d_name);
            }
            
            parse_file (s2);
            
            if (s2 != buf2)
            {
                free (s2);
            }
            
            /* if we have a list, update the next pointer of the last item (l),
             * else this becomes the first item of the list */
            if (*files)
            {
                last->next = file;
            }
            else
            {
                *files = file;
            }
        }
    }
    p (LVL_VERBOSE, "\nclosing folder\n");
    closedir (dp);
    
    if (path != buf)
    {
        free (path);
    }
}

int
main (int argc, char **argv)
{
    dirs_t   dirs    = { NULL, 0, 0 };
    files_t *files   = NULL;
    char    *dir;
    char    *s       = NULL;
    char    *ss;
    
    /* start with user dir */
    if (!(dir = getenv ("XDG_CONFIG_HOME")))
    {
        p (LVL_VERBOSE, "XDG_CONFIG_HOME not set, get HOME for default value\n");
        /* not defined, use default */
        if ((s = getenv ("HOME")))
        {
            /* 8 = strlen("/.config") + 1 for NULL */
            dir = malloc (sizeof (*dir) * (strlen (s) + 9));
            sprintf (dir, "%s/.config", s);
            p (LVL_VERBOSE, "using default: %s\n", dir);
        }
        else
        {
            fprintf (stderr,
                     "XDG_CONFIG_HOME not defined, unable to get HOME for default\n");
            return 1;
        }
    }
    process_dir (&dirs, &files, (const char *) dir);
    if (s)
    {
        free (dir);
    }
    
    /* then system wide dirs */
    if ((s = getenv ("XDG_CONFIG_DIRS-")))
    {
        p (LVL_VERBOSE, "XDG_CONFIG_DIRS set to %s\n", s);
        dir = s = strdup (s);
        while ((ss = strchr (dir, ':')))
        {
            *ss = '\0';
            process_dir (&dirs, &files, dir);
            dir = ss + 1;
        }
        process_dir (&dirs, &files, dir);
        free (s);
    }
    /* not defined, use default */
    else
    {
        p (LVL_VERBOSE, "XDG_CONFIG_DIRS not set, using default: /etc/xdg\n");
//        process_dir (&files, "/etc/xdg");
    }
    
    /* memory cleaning */
    p (LVL_DEBUG, "memory cleaning\n");
    
    int i;
    for (i = 0; i < dirs.len; ++i)
    {
        free ((void *) dirs.dirs[i]);
    }
    free (dirs.dirs);
    
    files_t *f, *ff;
    for (f = files; f; f = ff)
    {
        ff = f->next;
        free (f->name);
        free (f);
    }
    
    return 0;
}
