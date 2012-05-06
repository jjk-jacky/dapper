
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>

char *desktop = "KDE";
int   verbose = 2;

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

typedef enum {
    PARSE_OK        = 0,
    PARSE_ABORTED,
    PARSE_FAILED,
} parse_t;

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

void
unesc (char *str)
{
    int l;
    for (l = strlen (str); l; ++str, --l)
    {
        if (*str == '\\')
        {
            if (str[1] == 's')
            {
                *str = ' ';
            }
            else if (str[1] == 'n')
            {
                *str = '\n';
            }
            else if (str[1] == 't')
            {
                *str = '\t';
            }
            else if (str[1] == 'r')
            {
                *str = '\r';
            }
            else if (str[1] == '\\')
            {
                /* it's already a backslash, just need to memmove */
            }
            else
            {
                continue;
            }
            --l;
            memmove (str + 1, str + 2, l);
        }
    }
}

int
replace_fields (char **str, char *icon, char *name, char *file)
{
    char *s_icon;
    char *s_name;
    char *s_file;
    int   len = 0;
    
    s_icon = strstr (*str, "%i");
    s_name = strstr (*str, "%c");
    s_file = strstr (*str, "%k");
    
    if (s_icon)
    {
        if (icon)
        {
            /* 7 == strlen ("--icon ") */
            len += 7 + strlen (icon);
        }
        else
        {
            memmove (s_icon, s_icon + 3, strlen (s_icon) - 2);
        }
    }
    if (s_name)
    {
        if (name)
        {
            len += strlen (name);
        }
        else
        {
            memmove (s_name, s_name + 3, strlen (s_name) - 2);
        }
    }
    if (s_file)
    {
        if (file)
        {
            len += strlen (file);
        }
        else
        {
            memmove (s_file, s_file + 3, strlen (s_file) - 2);
        }
    }
    
    /* nothing, or already taken care of */
    if (len == 0)
    {
        return 0;
    }
    
    /* we need to allocate a new memory block to put the replacement(s) in */
    char *new;
    char *s;
    
    len += strlen (*str);
    new = malloc (sizeof (*str) * len);
    memcpy (new, *str, strlen (*str) + 1);
    
    if (s_icon)
    {
        /* s is s_icon in new */
        s = new + (s_icon - *str);
        /* move s_icon past the %i */
        s_icon += 2;
        /* put replacement in */
        len = strlen (icon);
        memcpy (s, "--icon ", 7);
        memcpy (s + 7, icon, len);
        memcpy (s + 7 + len, s_icon, strlen (s_icon) + 1); /* +1 for NUL */
    }
    
    if (s_name)
    {
        /* s is s_name in new */
        s = new + (s_name - *str);
        /* move s_name past the %c */
        s_name += 2;
        if (s_icon && s_icon < s_name)
        {
            /* replacement happened for s_icon already, we need to adjust */
            s += 7 + strlen (icon) - 2;
        }
        /* put replacement */
        len = strlen (name);
        memcpy (s, name, len);
        memcpy (s + len, s_name, strlen (s_name) + 1);
    }
    
    if (s_file)
    {
        /* s is s_file in new */
        s = new + (s_file - *str);
        /* move s_file past the %k */
        s_file += 2;
        if (s_icon && s_icon < s_file)
        {
            /* replacement happened for s_icon already, we need to adjust */
            s += 7 + strlen (icon) - 2;
        }
        if (s_name && s_name < s_file)
        {
            /* replacement happened for s_name already, we need to adjust */
            s += strlen (name) - 2;
        }
        /* put replacement */
        len = strlen (file);
        memcpy (s, file, len);
        memcpy (s + len, s_file, strlen (s_file) + 1);
    }
    
    *str = new;
    return 1;
}

void
split_exec (char *exec, int *argc, char ***argv, int *alloc)
{
    int    in_arg    = 0;
    int    is_quoted = 0;
    int    l;
    
    for (l = strlen (exec); l; ++exec, --l)
    {
        if (in_arg)
        {
            /* field codes */
            if (*exec == '%')
            {
                /* those are either deprecated or don't apply here */
                if (   exec[1] == 'f' || exec[1] == 'F' || exec[1] == 'u'
                    || exec[1] == 'U' || exec[1] == 'd' || exec[1] == 'D'
                    || exec[1] == 'n' || exec[1] == 'N' || exec[1] == 'v'
                    || exec[1] == 'm')
                {
                    l -= 2;
                    memmove (exec, exec + 3, l);
                    --exec;
                    continue;
                }
                /* unknown field codes are not allowed. i, c and k were already
                 * processed in replace_fields */
                free (*argv);
                *argv = NULL;
                return;
            }
            
            if (is_quoted)
            {
                if (*exec == '\\')
                {
                    /* some characters needs un-escaping */
                    if (   exec[1] == '"' || exec[1] == '`' || exec[1] == '$'
                        || exec[1] == '\\')
                    {
                        memmove (exec, exec + 1, l);
                        --l;
                    }
                    continue;
                }
                else if (*exec != '"')
                {
                    continue;
                }
            }
            else if (*exec != ' ')
            {
                continue;
            }
            
            /* arg over */
            *exec= '\0';
            in_arg = 0;
            is_quoted = 0;
            p (LVL_VERBOSE, "argv[%d]=%s\n", *argc, *argv[*argc]);
        }
        else
        {
            /* we're looking for an arg. skip spaces, and if quoted start
             * after the dbl-quote */
            if (*exec != ' ')
            {
                in_arg = 1;
                is_quoted = (*exec == '"');
                if (++*argc >= *alloc - 1)
                {
                    *alloc += 10;
                    *argv = realloc (*argv, sizeof (**argv) * *alloc);
                    memset (*argv + *argc + 1, '\0', 10 * sizeof (**argv));
                }
                *argv[*argc] = exec + is_quoted;
            }
        }
    }
    if (in_arg)
    {
        p (LVL_VERBOSE, "argv[%d]=%s\n", *argc, *argv[*argc]);
    }
}

int
is_in_list (char *name, char *items, char *item)
{
    int   len = strlen (items);
    char *s;
    
    p (LVL_DEBUG, "[%s] searching for %s in %s\n", name, item, items);
    
    if (items[len - 1] != ';')
    {
        p (LVL_ERROR, "invalid syntax for %s\n", name);
        return 0;
    }
    
    while ((s = strchr (items, ';')))
    {
        if (strncmp (item, items, s - items) == 0)
        {
            return 1;
        }
        items = s + 1;
    }
    return 0;
}

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
    }
    *data = '\0'; /* in case the file is empty, so strcat works */
    data[statbuf.st_size] = '\0'; /* because fread won't put it */
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
                unesc (value);
                p (LVL_VERBOSE, "%s set to %s\n", key, value);
                exec = value;
            }
            else if (strcmp (key, "TryExec") == 0)
            {
                unesc (value);
                p (LVL_VERBOSE, "%s set to %s\n", key, value);
                try_exec = value;
            }
            else if (strcmp (key, "OnlyShownIn") == 0)
            {
                if (not_in)
                {
                    p (LVL_ERROR, "%s: error, OnlyShownIn and NotShownIn both defined\n",
                       file);
                    state = PARSE_FAILED;
                }
                unesc (value);
                p (LVL_VERBOSE, "%s set to %s\n", key, value);
                only_in = value;
            }
            else if (strcmp (key, "NotShownIn") == 0)
            {
                if (only_in)
                {
                    p (LVL_ERROR, "%s: error, OnlyShownIn and NotShownIn both defined\n",
                       file);
                    state = PARSE_FAILED;
                }
                unesc (value);
                p (LVL_VERBOSE, "%s set to %s\n", key, value);
                not_in = value;
            }
            else if (strcmp (key, "Icon") == 0)
            {
                unesc (value);
                p (LVL_VERBOSE, "%s set to %s\n", key, value);
                icon = value;
            }
            else if (strcmp (key, "Path") == 0)
            {
                unesc (value);
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
    
    if (state == PARSE_OK || state == PARSE_ABORTED)
    {
        if (hidden)
        {
            p (LVL_VERBOSE, "no auto-start to perform\n", file);
            if (data != buf)
            {
                free (data);
            }
            return;
        }
        
        if (desktop)
        {
            if (only_in && !is_in_list ("OnlyShownIn", only_in, desktop))
            {
                p (LVL_VERBOSE, "%s not in OnlyShownIn, no auto-start\n", desktop);
                if (data != buf)
                {
                    free (data);
                }
                return;
            }
            else if (not_in && is_in_list ("NotShownIn", not_in, desktop))
            {
                p (LVL_VERBOSE, "%s in NotShownIn, no auto-start\n", desktop);
                if (data != buf)
                {
                    free (data);
                }
                return;
            }
        }
        else if (only_in)
        {
            p (LVL_ERROR, "%s: OnlyShownIn set, desktop unknown, no auto-start\n",
               file);
            if (data != buf)
            {
                free (data);
            }
            return;
        }
        else if (not_in)
        {
            p (LVL_ERROR, "%s: NotShownIn set, desktop unknown, no auto-start\n",
               file);
            if (data != buf)
            {
                free (data);
            }
            return;
        }
        
        if (try_exec)
        {
            int try_state = 0;
            
            /* is it an absolute path or not? */
            if (*try_exec != '/')
            {
                /* must search the PATH then */
                if (!(s = getenv ("PATH")))
                {
                    p (LVL_VERBOSE, "%s: no PATH to find TryExec (%s), no auto-start\n",
                       file, try_exec);
                    if (data != buf)
                    {
                        free (data);
                    }
                    return;
                }
                
                char  buf2[2048];
                char *path = strdup (s);
                char *dir  = path;
                
                while ((s = strchr (dir, ':')))
                {
                    *s = '\0';
                    snprintf (buf2, 2048, "%s/%s", dir, try_exec);
                    p (LVL_DEBUG, "TryExec: checking %s\n", buf2);
                    if (access (buf2, F_OK | X_OK) == 0)
                    {
                        try_state = 1;
                        break;
                    }
                    dir = s + 1;
                }
                if (!try_state && dir)
                {
                    snprintf (buf2, 2048, "%s/%s", dir, try_exec);
                    p (LVL_DEBUG, "TryExec: checking %s\n", buf2);
                    if (access (buf2, F_OK | X_OK) == 0)
                    {
                        try_state = 1;
                    }
                }
                free (path);
            }
            else
            {
                p (LVL_DEBUG, "TryExec: checking %s\n", try_exec);
                if (access (try_exec, F_OK | X_OK) == 0)
                {
                    try_state = 1;
                }
            }
            
            if (try_state)
            {
                p (LVL_DEBUG, "TryExec: found & executable\n");
            }
            else
            {
                p (LVL_VERBOSE, "%s: unable to find executable TryExec (%s), "
                                "no autostart\n",
                   file, try_exec);
                if (data != buf)
                {
                    free (data);
                }
                return;
            }
        }
        
        char  *s;
        int    need_free;
        char **argv     = NULL;
        int    argc     = -1;
        int    alloc    = 0;
        pid_t  pid;
        
        p (LVL_VERBOSE, "%s: triggering auto-start\n", file);
        
        s = exec;
        need_free = replace_fields (&s, icon, NULL, file);
        
        split_exec (s, &argc, &argv, &alloc);
        if (!argv)
        {
            p (LVL_ERROR, "%s: error processing command line\n", file);
            if (data != buf)
            {
                free (data);
            }
            free (argv);
            if (need_free)
            {
                free (s);
            }
            return;
        }
        pid = fork ();
        if (pid == 0)
        {
            /* child */
            if (strcmp(icon, "foobar")==0)
            {
                execvp (argv[0], argv);
            }
            /* TODO: remove: won't be shown if parent is done first... */
            p (LVL_ERROR, "%s: unable to start process\n", file);
            exit (1);
        }
        else if (pid == -1)
        {
            p (LVL_ERROR, "%s: unable to fork\n", file);
        }
        free (argv);
        if (need_free)
        {
            free (s);
        }
    }
    
    if (data != buf)
    {
        free (data);
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
