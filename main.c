
/* for getopt_long */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>

#include "config.h"

static char *desktop  = NULL;
static char *term_cmd = NULL;
static int   verbose  = 0;
static int   dry_run  = 0;

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
    PARSE_FILE_NOT_FOUND,       /* mostly when reading config */
} parse_t;

typedef enum {
    DIR_CONST = 0,      /* no suffix, no free needed */
    DIR_ADD_SUFFIX,     /* auto-adds suffix, free when done */
    DIR_NEEDS_FREE,     /* no suffix, but free when done */
} dir_type_t;

typedef struct
{
    char      *dir;
    dir_type_t type;
} dir_t;

typedef struct
{
    dir_t  *dirs;
    int     alloc;
    int     len;
} dirs_t;

typedef struct _files_t
{
    char            *name;
    struct _files_t *next;
} files_t;

typedef struct
{
    char *icon;
    int   hidden;
    char *only_in;
    char *not_in;
    char *try_exec;
    char *exec;
    char *path;
    int   terminal;
} desktop_t;

static char *
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

static void
unesc (char *str)
{
    size_t l;
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

static int
replace_fields (char **str, char *icon, char *name, char *file)
{
    const char *replacement[] = {
        "%i",   icon,
        "%c",   name,
        "%k",   file,
        NULL
    };
    char    *new        = *str;
    size_t   alloc      = 0;
    size_t   len        = strlen (*str);
    int      need_free  = 0;

    const char **r;
    for (r = replacement; r && *r; r += 2)
    {
        for (;;)
        {
            char *s = strstr (new, r[0]);
            if (!s)
            {
                break;
            }
            if (r[1])
            {
                /* replace */
                size_t len_fnd = strlen (r[0]);
                size_t len_rep = strlen (r[1]);
                size_t l;

                /* icon is a special case, w/ a prefix */
                if (r[1] == icon)
                {
                    len_rep += 7; /* 7 == strlen ("--icon ") */
                }
                l = len_rep - len_fnd;

                if (!need_free || alloc - len < l)
                {
                    alloc += l + 255;
                    new = realloc ((need_free) ? new : NULL,
                            sizeof (*new) * (alloc + 1));
                    if (!need_free)
                    {
                        need_free = 1;
                        memcpy (new, *str, len + 1 /* for NULL */);
                    }
                    s = strstr (new, r[0]);
                }
                /* put the bit after the replacement in */
                memmove (s + len_rep,
                        s + len_fnd,
                        strlen (s + len_fnd) + 1);
                /* and now the replacement itself */
                if (r[1] == icon)
                {
                    memcpy (s, "--icon ", 7);
                    memcpy (s + 7, r[1], len_rep - 7);
                }
                else
                {
                    memcpy (s, r[1], len_rep);
                }
                /* adjust len */
                len += l;
            }
            else
            {
                /* remove */
                size_t l = strlen (r[0]);
                if (*(s + l) == ' ')
                {
                    ++l;
                }
                memmove (s, s + l, strlen (s + l) + 1 /* for NULL */);
                len -= l;
            }
        }
    }

    *str = new;
    return need_free;
}

/* if the argument was nothing but a field code, we shouldn't send anything,
 * as opposed to send an empty string as argument (which might cause problems,
 * or unexpected behaviors, with some apps. E.g. a file manager or browser
 * would open a new tab in the "current" folder or something...) */
#define close_arg() do {                                        \
    if (had_field_code && (*argv)[*argc][0] == '\0')            \
    {                                                           \
        (*argv)[*argc] = NULL;                                  \
        --*argc;                                                \
    }                                                           \
    else                                                        \
    {                                                           \
        p (LVL_DEBUG, "argv[%d]=%s\n", *argc, (*argv)[*argc]);  \
    }                                                           \
} while (0)

static void
split_exec (char *exec, int *argc, char ***argv, int *alloc)
{
    int    in_arg    = 0;
    int    is_quoted = 0;
    char   quote;
    int    had_field_code = 0;
    size_t l;

    for (l = strlen (exec) + 1; l > 0 && --l; ++exec)
    {
        if (in_arg)
        {
            /* field codes */
            if (*exec == '%')
            {
                /* those are either deprecated or don't apply here */
                if (       exec[1] == 'f' || exec[1] == 'F' || exec[1] == 'u'
                        || exec[1] == 'U' || exec[1] == 'd' || exec[1] == 'D'
                        || exec[1] == 'n' || exec[1] == 'N' || exec[1] == 'v'
                        || exec[1] == 'm')
                {
                    had_field_code = 1;
                    --l;
                    if (l)
                    {
                        memmove (exec, exec + 2, l + 1);
                    }
                    else
                    {
                        *exec = '\0';
                        in_arg = 0;
                        is_quoted = 0;
                        close_arg ();
                    }
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
                    if (exec[1] == '"' || exec[1] == '`' || exec[1] == '$'
                            || exec[1] == '\\')
                    {
                        memmove (exec, exec + 1, l);
                        --l;
                    }
                    continue;
                }
                else if (*exec != quote)
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
            close_arg ();
        }
        else
        {
            /* we're looking for an arg. skip spaces, and if quoted start
             * after the dbl-quote */
            if (*exec != ' ')
            {
                in_arg = 1;
                is_quoted = (*exec == '"' || *exec == '\'');
                had_field_code = 0;
                if (++*argc >= *alloc - 1)
                {
                    *alloc += 10;
                    *argv = realloc (*argv, sizeof (**argv) * (size_t) *alloc);
                    memset (*argv + *argc + 1, '\0', 10 * sizeof (**argv));
                }
                (*argv)[*argc] = exec + is_quoted;
                if (is_quoted)
                {
                    quote = *exec;
                }
                else
                {
                    --exec;
                    ++l;
                }
            }
        }
    }
    if (in_arg)
    {
        close_arg ();
    }
}

static int
is_in_list (const char *name, char *items, char *item)
{
    size_t len = strlen (items);
    char  *s;

    p (LVL_DEBUG, "[%s] searching for %s in %s\n", name, item, items);

    if (items[len - 1] != ';')
    {
        p (LVL_ERROR, "invalid syntax for %s\n", name);
        return 0;
    }

    while ((s = strchr (items, ';')))
    {
        if (strncmp (item, items, (size_t) (s - items)) == 0)
        {
            return 1;
        }
        items = s + 1;
    }
    return 0;
}

static parse_t
parse_file (int is_desktop, char *file, char **data, size_t len_data, void *out)
{
    struct stat statbuf;
    FILE       *fp;

    /* get the file size */
    if (stat (file, &statbuf) != 0)
    {
        if (errno == ENOENT)
        {
            p (LVL_VERBOSE, "%s: does not exists\n", file);
            return PARSE_FILE_NOT_FOUND;
        }
        p (LVL_ERROR, "%s: unable to stat file\n", file);
        return PARSE_FAILED;
    }

    if (!(fp = fopen (file, "r")))
    {
        p (LVL_ERROR, "%s: unable to open file\n", file);
        return PARSE_FAILED;
    }

    desktop_t *d = NULL;
    size_t  l;
    char   *line;
    char   *s;
    int     line_nb     = 0;
    int     in_section  = 0;
    char   *key;
    char   *value;
    parse_t state       = PARSE_OK;

    if (is_desktop)
    {
        d = (desktop_t *) out;
    }

    /* can we read the whole file in data, or do we need to allocate memory? */
    if ((size_t) statbuf.st_size >= len_data)
    {
        /* +2: 1 for extra LF; 1 for NUL */
        *data = malloc (sizeof (**data) * (size_t) (statbuf.st_size + 2));
    }
    **data = '\0'; /* in case the file is empty, so strcat works */
    (*data)[statbuf.st_size] = '\0'; /* because fread won't put it */
    p (LVL_DEBUG, "read file (%lu bytes)\n", statbuf.st_size);
    fread (*data, (size_t) statbuf.st_size, 1, fp);
    fclose (fp);
    strcat (*data, "\n");

    /* now do the parsing */
    line = *data;
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

        /* .desktop file: we only support section "Desktop Entry" */
        if (is_desktop && *line == '[' && (l = strlen (line)) && line[l - 1] == ']')
        {
            in_section = (strcmp ("Desktop Entry]", line + 1) == 0);
        }
        else if (in_section || !is_desktop)
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

            if (is_desktop)
            {
                /* .desktop file */

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
                        d->hidden = 1;
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
                    d->exec = value;
                }
                else if (strcmp (key, "TryExec") == 0)
                {
                    unesc (value);
                    p (LVL_VERBOSE, "%s set to %s\n", key, value);
                    d->try_exec = value;
                }
                else if (strcmp (key, "OnlyShowIn") == 0)
                {
                    if (d->not_in)
                    {
                        p (LVL_ERROR,
                                "%s: error, OnlyShowIn and NotShowIn both defined\n",
                                file);
                        state = PARSE_FAILED;
                    }
                    unesc (value);
                    p (LVL_VERBOSE, "%s set to %s\n", key, value);
                    d->only_in = value;
                }
                else if (strcmp (key, "NotShowIn") == 0)
                {
                    if (d->only_in)
                    {
                        p (LVL_ERROR,
                                "%s: error, OnlyShowIn and NotShowIn both defined\n",
                                file);
                        state = PARSE_FAILED;
                    }
                    unesc (value);
                    p (LVL_VERBOSE, "%s set to %s\n", key, value);
                    d->not_in = value;
                }
                else if (strcmp (key, "Icon") == 0)
                {
                    unesc (value);
                    p (LVL_VERBOSE, "%s set to %s\n", key, value);
                    d->icon = value;
                }
                else if (strcmp (key, "Path") == 0)
                {
                    unesc (value);
                    p (LVL_VERBOSE, "%s set to %s\n", key, value);
                    d->path = value;
                }
                else if (strcmp (key, "Terminal") == 0)
                {
                    if (strcmp (value, "true") == 0)
                    {
                        d->terminal = 1;
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
                /* dapper.conf */

                if (strcmp (key, "Desktop") == 0)
                {
                    desktop = value;
                    p (LVL_VERBOSE, "set desktop to %s\n", desktop);
                }
                else if (strcmp (key, "Terminal") == 0)
                {
                    term_cmd = value;
                    p (LVL_VERBOSE, "set terminal command line prefix to: %s\n",
                            term_cmd);
                }
                else
                {
                    p (LVL_ERROR, "%s: unknown option line %d: %s\n",
                            file, line_nb, key);
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
    return state;
}

static char *
get_expanded_path (char *path)
{
    char *home;
    char *s;

    if (*path != '~' || !(home = getenv ("HOME")))
    {
        return strdup (path);
    }

    s = malloc (sizeof (*s) * (strlen (home) + strlen (path)));
    sprintf (s, "%s%s", home, path + 1);
    return s;
}

static void
process_file (char *file)
{
    char      buf[4096];
    char     *data = buf;
    parse_t   state;
    desktop_t d;
    char     *s;

    p (LVL_DEBUG, "processing file: %s\n", file);
    memset (&d, 0, sizeof (d));
    state = parse_file (1, file, &data, 4096, &d);
    if (state == PARSE_OK || (state == PARSE_ABORTED && d.hidden))
    {
        if (d.hidden)
        {
            p (LVL_VERBOSE, "no auto-start to perform\n");
            if (data != buf)
            {
                free (data);
            }
            return;
        }

        if (desktop)
        {
            if (d.only_in && !is_in_list ("OnlyShowIn", d.only_in, desktop))
            {
                p (LVL_VERBOSE, "%s not in OnlyShowIn, no auto-start\n", desktop);
                if (data != buf)
                {
                    free (data);
                }
                return;
            }
            else if (d.not_in && is_in_list ("NotShowIn", d.not_in, desktop))
            {
                p (LVL_VERBOSE, "%s in NotShowIn, no auto-start\n", desktop);
                if (data != buf)
                {
                    free (data);
                }
                return;
            }
        }
        else if (d.only_in)
        {
            p (LVL_ERROR, "%s: OnlyShowIn set, desktop unknown, no auto-start\n",
                    file);
            if (data != buf)
            {
                free (data);
            }
            return;
        }
        else if (d.not_in)
        {
            p (LVL_ERROR, "%s: NotShowIn set, desktop unknown, no auto-start\n",
                    file);
            if (data != buf)
            {
                free (data);
            }
            return;
        }

        if (d.try_exec)
        {
            int try_state = 0;

            /* is it an absolute path or not? */
            if (*d.try_exec != '/')
            {
                /* must search the PATH then */
                char *path;
                char *dir;

                path = getenv ("PATH");
                if (!path)
                {
                    p (LVL_VERBOSE, "%s: no PATH to find TryExec (%s), no auto-start\n",
                            file, d.try_exec);
                    if (data != buf)
                    {
                        free (data);
                    }
                    return;
                }
                path = strdup (path);
                dir = path;

                for (;;)
                {
                    char   buf2[1024];
                    char  *b = buf2;
                    size_t l;

                    if ((s = strchr (dir, ':')))
                    {
                        *s = '\0';
                    }
                    l = strlen (dir) + strlen (d.try_exec) + 1; /* +1 == slash */
                    if (l >= 1024)
                    {
                        b = malloc (sizeof (*b) * (l + 1));
                    }
                    sprintf (b, "%s/%s", dir, d.try_exec);
                    p (LVL_DEBUG, "TryExec: checking %s\n", buf2);
                    if (access (b, F_OK | X_OK) == 0)
                    {
                        if (b != buf2)
                        {
                            free (b);
                        }
                        try_state = 1;
                        break;
                    }

                    if (b != buf2)
                    {
                        free (b);
                    }

                    if (!s)
                    {
                        break;
                    }
                    dir = s + 1;
                }
                free (path);
            }
            else
            {
                p (LVL_DEBUG, "TryExec: checking %s\n", d.try_exec);
                if (access (d.try_exec, F_OK | X_OK) == 0)
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
                        file, d.try_exec);
                if (data != buf)
                {
                    free (data);
                }
                return;
            }
        }

        int    need_free;
        char **argv     = NULL;
        int    argc     = -1;
        int    alloc    = 0;
        pid_t  pid;

        p (LVL_VERBOSE, "%s: triggering auto-start\n", file);

        s = d.exec;
        need_free = replace_fields (&s, d.icon, NULL, file);

        if (d.terminal)
        {
            if (term_cmd)
            {
                split_exec (term_cmd, &argc, &argv, &alloc);
            }
            if (!argv)
            {
                p (LVL_ERROR, "%s: error with terminal command line: %s\n",
                        file, term_cmd);
                if (data != buf)
                {
                    free (data);
                }
                if (need_free)
                {
                    free (s);
                }
                return;
            }
        }

        split_exec (s, &argc, &argv, &alloc);
        if (!argv)
        {
            p (LVL_ERROR, "%s: error processing command line\n", file);
            if (data != buf)
            {
                free (data);
            }
            if (need_free)
            {
                free (s);
            }
            return;
        }

        /* handle ~ for $HOME */
        char **a;
        int i;
        a = malloc (sizeof (*a) * (size_t) (argc + 2));
        for (i = 0; i <= argc; ++i)
        {
            a[i] = get_expanded_path (argv[i]);
        }

        if (dry_run)
        {
            char **_a;
            p (LVL_NORMAL, "auto-start: %s", a[0]);
            for (_a = a + 1; *_a; ++_a)
            {
                p (LVL_NORMAL, " %s", *_a);
            }
            p (LVL_NORMAL, "\n");
        }
        else
        {
            pid = fork ();
            if (pid == 0)
            {
                /* child */
                execvp (a[0], a);
                exit (1);
            }
            else if (pid == -1)
            {
                p (LVL_ERROR, "%s: unable to fork\n", file);
            }
        }

        for (i = 0; i <= argc; ++i)
        {
            free (a[i]);
        }
        free (a);
        free (argv);
        if (need_free)
        {
            free (s);
        }
    }
    else
    {
        p (LVL_VERBOSE, "parsing failed (%d), no auto-start\n", state);
    }

    if (data != buf)
    {
        free (data);
    }
}

static void
add_dir (dirs_t *dirs, char *dir, dir_type_t type)
{
    int     i;
    size_t  l;
    char   *s;

    l = strlen (dir);
    if (type == DIR_ADD_SUFFIX)
    {
        /* 11 = strlen ("/autostart") + 1 for NULL */
        s = malloc (sizeof (*s) * (l + 11));
        sprintf (s, "%s%sautostart", dir, dir[l - 1] == '/' ? "" : "/");
        if (dir[l - 1] != '/')
        {
            ++l;
        }
        l += 9; /* 9 = strlen ("autostart") */
        dir = s;
    }

    if (l && dir[l - 1] == '/')
    {
        dir[l - 1] = '\0';
    }

    /* make sure this dir hasn't been processed already */
    for (i = 0; i < dirs->len; ++i)
    {
        if (strcmp (dirs->dirs[i].dir, dir) == 0)
        {
            p (LVL_DEBUG, "%s: already listed, skipping\n", dir);
            if (type == DIR_ADD_SUFFIX || type == DIR_NEEDS_FREE)
            {
                free ((char *) dir);
            }
            return;
        }
    }
    if (dirs->len == dirs->alloc)
    {
        dirs->alloc += 10;
        dirs->dirs = realloc (dirs->dirs, sizeof (*dirs->dirs) * (size_t) dirs->alloc);
    }
    p (LVL_DEBUG, "adding folder: %s\n", dir);
    dirs->dirs[dirs->len].dir = (char *) dir;
    dirs->dirs[dirs->len++].type = type;
}

static int
load_conf (char **data)
{
    int          ret = 0;
    const char  *home = getenv ("HOME");
    char        *file;
    char         buf[2048];
    size_t       len;

    if (!home)
    {
        p (LVL_ERROR, "cannot load configuration: unable to get HOME path\n");
        return ret;
    }

    len = (size_t) snprintf (buf, 2048, "%s/.config/dapper.conf", home);
    if (len < 2048)
    {
        file = buf;
    }
    else
    {
        file = malloc (sizeof (*file) * (len + 1));
        sprintf (file, "%s/.config/dapper.conf", home);
    }

    p (LVL_VERBOSE, "loading config from %s\n", file);
    ret = parse_file (0, file, data, 0, NULL);
    ret = (ret == PARSE_OK || ret == PARSE_FILE_NOT_FOUND);
    p (LVL_VERBOSE, "\n");

    if (file != buf)
    {
        free (file);
    }

    return ret;
}

static void
show_help (void)
{
    fprintf (stdout, PACKAGE_NAME " - Desktop Applications Autostarter v" PACKAGE_VERSION "\n");
    fprintf (stdout, "\n");
    fprintf (stdout, " -h, --help               Show this help screen and exit\n");
    fprintf (stdout, " -V, --version            Show version information and exit\n");
    fprintf (stdout, " -s, --system-dirs        Process autostart from system folders\n");
    fprintf (stdout, " -u, --user-dir           Process autostart from user folder\n");
    fprintf (stdout, " -e, --extra-dir PATH     Process autostart from PATH\n");
    fprintf (stdout, " -d, --desktop DESKTOP    Start applications for DESKTOP\n");
    fprintf (stdout, " -t, --terminal CMDLINE   Use CMDLINE as prefix for terminal mode\n");
    fprintf (stdout, " -v, --verbose            Verbose mode (twice for debug mode)\n");
    fprintf (stdout, " -n, --dry-run            Do not actually start anything\n");
    exit (0);
}

static void
show_version (void)
{
    fprintf (stdout, PACKAGE_NAME " - Desktop Applications Autostarter v" PACKAGE_VERSION "\n");
    fprintf (stdout, "Copyright (C) 2012 Olivier Brunel\n");
    fprintf (stdout, "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>\n");
    fprintf (stdout, "This is free software: you are free to change and redistribute it.\n");
    fprintf (stdout, "There is NO WARRANTY, to the extent permitted by law.\n");
    exit (0);
}

int
main (int argc, char **argv)
{
    char    *data_conf  = NULL;
    dirs_t   dirs       = { NULL, 0, 0 };
    files_t *files      = NULL;
    char    *dir;
    char    *s          = NULL;
    char    *ss;

    if (load_conf (&data_conf) == 0)
    {
        free (data_conf);
        return 1;
    }

    int o;
    int index= 0;
    struct option options[] = {
        { "help",           no_argument,        0,  'h' },
        { "version",        no_argument,        0,  'V' },
        { "system-dirs",    no_argument,        0,  's' },
        { "user-dir",       no_argument,        0,  'u' },
        { "extra-dir",      required_argument,  0,  'e' },
        { "desktop",        required_argument,  0,  'd' },
        { "terminal",       required_argument,  0,  't' },
        { "verbose",        no_argument,        0,  'v' },
        { "dry-run",        no_argument,        0,  'n' },
        { 0,                0,                  0,    0 },
    };
    for (;;)
    {
        o = getopt_long (argc, argv, "hVsue:d:t:vn", options, &index);
        if (o == -1)
        {
            break;
        }

        switch (o)
        {
            case 'h':
                show_help ();
                /* not reached */
                break;
            case 'V':
                show_version ();
                /* not reached */
                break;
            case 'u':
                p (LVL_DEBUG, "add user dir\n");
                if ((dir = getenv ("XDG_CONFIG_HOME")))
                {
                    add_dir (&dirs, dir, DIR_ADD_SUFFIX);
                }
                else
                {
                    p (LVL_VERBOSE, "XDG_CONFIG_HOME not set, get HOME for default value\n");
                    /* not defined, use default */
                    if ((s = getenv ("HOME")))
                    {
                        /* 19 = strlen("/.config/autostart") + 1 for NULL */
                        dir = malloc (sizeof (*dir) * (strlen (s) + 19));
                        sprintf (dir, "%s/.config/autostart", s);
                        p (LVL_VERBOSE, "using default: %s\n", dir);
                        add_dir (&dirs, dir, DIR_NEEDS_FREE);
                        free (dir);
                    }
                    else
                    {
                        p (LVL_ERROR,
                                "XDG_CONFIG_HOME not defined, unable to get HOME for default\n");
                        return 1;
                    }
                }
                break;
            case 's':
                p (LVL_DEBUG, "add system dirs\n");
                if ((s = getenv ("XDG_CONFIG_DIRS")))
                {
                    p (LVL_VERBOSE, "XDG_CONFIG_DIRS set to %s\n", s);
                    dir = s = strdup (s);
                    while ((ss = strchr (dir, ':')))
                    {
                        *ss = '\0';
                        add_dir (&dirs, dir, DIR_ADD_SUFFIX);
                        dir = ss + 1;
                    }
                    add_dir (&dirs, dir, DIR_ADD_SUFFIX);
                    free (s);
                }
                /* not defined, use default */
                else
                {
                    p (LVL_VERBOSE, "XDG_CONFIG_DIRS not set, using default: /etc/xdg\n");
                    add_dir (&dirs, (char *) "/etc/xdg/autostart", DIR_CONST);
                }
                break;
            case 'e':
                p (LVL_DEBUG, "add extra dir: %s\n", optarg);
                add_dir (&dirs, optarg, DIR_CONST);
                break;
            case 'd':
                desktop = optarg;
                p (LVL_VERBOSE, "cmdline: set desktop to %s\n", desktop);
                break;
            case 't':
                term_cmd = optarg;
                p (LVL_VERBOSE, "cmdline: set terminal command line prefix to: %s\n",
                        term_cmd);
                break;
            case 'v':
                ++verbose;
                break;
            case 'n':
                dry_run = 1;
                break;
            case '?': /* unknown option */
            default:
                return 1;
        }
    }
    if (optind < argc)
    {
        p (LVL_ERROR, "unknown argument: %s\n", argv[optind]);
        return 1;
    }

    if (dirs.len == 0)
    {
        show_help ();
        /* not reached */
        return 1;
    }

    int i;
    p (LVL_DEBUG, "processing folders\n");
    for (i = 0; i < dirs.len; ++i)
    {
        DIR           *dp;
        struct dirent *dirent;
        size_t         l;

        dir = dirs.dirs[i].dir;
        p (LVL_VERBOSE, "open folder %s\n", dir);
        if (!(dp = opendir (dir)))
        {
            if (errno == ENOENT)
            {
                p (LVL_VERBOSE, "skip: %s does not exists\n", dir);
            }
            else
            {
                p (LVL_ERROR, "failed to open %s\n", dir);
            }
            continue;
        }

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
            files_t  *last = NULL;
            files_t  *file;
            int       process;

            /* make sure we don't already have this item (from a previous dir) */
            process = 1;
            for (f = files; f; f = f->next)
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

                char  buf[4096];
                l = (size_t) snprintf (buf, 4096, "%s/%s", dir, dirent->d_name);
                if (l < 4096)
                {
                    s = buf;
                }
                else
                {
                    s = malloc (sizeof (*s) * (l + 1));
                    snprintf (s, l, "%s/%s", dir, dirent->d_name);
                }

                process_file (s);

                if (s != buf)
                {
                    free (s);
                }

                /* if we have a list, update the next pointer of the last item (l),
                 * else this becomes the first item of the list */
                if (files && last)
                {
                    last->next = file;
                }
                else
                {
                    files = file;
                }
            }
        }
        p (LVL_VERBOSE, "\nclosing folder\n");
        closedir (dp);

        if (   dirs.dirs[i].type == DIR_ADD_SUFFIX
                || dirs.dirs[i].type == DIR_NEEDS_FREE)
        {
            free ((void *) dirs.dirs[i].dir);
        }
    }
    free (dirs.dirs);

    /* memory cleaning */
    p (LVL_DEBUG, "memory cleaning\n");

    files_t *f, *ff;
    for (f = files; f; f = ff)
    {
        ff = f->next;
        free (f->name);
        free (f);
    }

    free (data_conf);

    return 0;
}

