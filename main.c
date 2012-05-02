
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

typedef struct
{
    const char **dirs;
    int          alloc;
    int          len;
} dirs_t;

typedef struct _list_t
{
    char            *pathname;
    char            *filename;
    struct _list_t  *next;
} list_t;

/**
 * Adds the given path/file to the list of files to process, unless there's
 * already an item for that file (from a previous dir)
 * 
 * @param list          pointer to address of the (first item of the) list
 * @param path
 * @param filename
 */
void
add_to_list (list_t **list, char *path, char *filename)
{
    list_t  *l;
    list_t  *last;
    list_t  *item;
    int      len;
    
    /* make sure we don't already have this item (from a previous dir) */
    for (l = *list; l; l = l->next)
    {
        last = l;
        if (strcmp (filename, l->filename) == 0)
        {
            return;
        }
    }
    
    item = malloc (sizeof (*item));
    len = strlen (path) + 1 + strlen (filename);
    item->pathname = malloc (sizeof (*item->pathname) * (len + 1));
    sprintf (item->pathname, "%s/%s", path, filename);
    item->filename = item->pathname + strlen (path) + 1;
    item->next = NULL;
    
    /* if we have a list, update the next pointer of the last item (l),
     * else this becomes the first item of the list */
    if (*list)
    {
        last->next = item;
    }
    else
    {
        *list = item;
    }
}

void
trim (char *str)
{
    char *s, *ss;
    
    for (s = str; *s == ' ' || *s == '\t'; ++s)
        ;
    if (s != str)
    {
        memmove (str, s, strlen (s) + 1); /* +1 to include NULL */
    }
    
    ss = NULL;
    for (s = str; *s; ++s)
    {
        if (*s != ' ' && *s != '\t' && *s != '\n')
        {
            ss = NULL;
        }
        else if (!ss)
        {
            ss = s;
        }
    }
    if (ss)
    {
        *ss = '\0';
    }
}

void
parse_file (char *file)
{
    FILE *fp;
    
    if (!(fp = fopen (file, "r")))
    {
        return;
    }
    
    char buf[4096];
    char *s = NULL;
    int  alloc = 0;
    int  len = 0;
    int  l;
    
    int  in_section = 0;
    char *key;
    char *value;
    
    for (;;)
    {
        /* try to read line */
        if (!fgets (buf, 4096, fp))
        {
            /* nothing read, but we might have something stored in s, e.g. if
             * the last line of the file does not end with a LF */
            if (!s)
            {
                break;
            }
            buf[0] = '\0';
        }
        
        l = strlen (buf);
        if (s || buf[l - 1] != '\n')
        {
            /* probably we couldn't read the whole line... */
            if (alloc < len + l)
            {
                alloc += 1024 + l;
                s = realloc (s, sizeof (*s) * alloc);
                /* if this was the first alloc, make sure the string is "empty" */
                if (len == 0)
                {
                    *s = '\0';
                }
            }
            strcat (s, buf);
            len += l;
            
            if (l > 0 && buf[l - 1] != '\n')
            {
                continue;
            }
        }
        else
        {
            s = buf;
        }
        
        trim (s);
        /* ignore comments & empty lines */
        if (*s == '#' || *s == '\0')
        {
            goto clean;
        }
        
        /* we only support section "Desktop Entry" */
        if (*s == '[' && (l = strlen (s)) && s[l - 1] == ']')
        {
            in_section = (strcmp ("Desktop Entry]", s + 1) == 0);
        }
        else if (in_section)
        {
            if (!(value = strchr (s, '=')))
            {
                printf("missing =\n");
                goto clean;
            }
            
            *value = '\0';
            key = s;
            trim (key);
            ++value;
            trim (value);
            
            if (strcmp (key, "Type") == 0)
            {
                if (strcmp (value, "Application") != 0)
                {
                    printf("invalid type\n");
                }
            }
            else if (strcmp (key, "Hidden") == 0)
            {
                if (strcmp (value, "true") == 0)
                {
                    printf("is hidden\n");
                }
                else if (strcmp (value, "false") != 0)
                {
                    printf("invalid Hidden\n");
                }
            }
            else
            {
                printf("#%s=%s#\n", key, value);
            }
        }
        
clean:
        if (s != buf)
        {
            free (s);
            alloc = len = 0;
        }
        s = NULL;
    }
    fclose (fp);
}

void
process_dir (dirs_t *dirs, list_t **files, const char *dir)
{
    int i;
    
    /* make sure this dir hasn't been processed already. This is in case e.g.
     * XDG_CONFIG_DIRS was set to /etc/xdg:/etc/xdg:/etc/xdg */
    for (i = 0; i < dirs->len; ++i)
    {
        if (strcmp (dirs->dirs[i], dir) == 0)
        {
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
    char          *s;
    
    l = snprintf (buf, 4096, "%s/autostart", dir);
    if (l < 4096)
    {
        s = buf;
    }
    else
    {
        s = malloc (sizeof (*s) * (l + 1));
        snprintf (s, l, "%s/autostart", dir);
    }
    
    dp = opendir (s);
    while ((dirent = readdir (dp)))
    {
        if (!(dirent->d_type & DT_REG))
        {
            /* ignore directory, etc -- symlinks to file are NOT ignored */
            continue;
        }

        l = strlen (dirent->d_name);
        /* 8 == strlen (".desktop") */
        if (l < 8 || strcmp (".desktop", &dirent->d_name[l - 8]) != 0)
        {
            /* ignore anything not .desktop */
            continue;
        }

        /* add item to list, unless already present (from previous dir)  */
        add_to_list (files, s, dirent->d_name);
    }
    closedir (dp);
    
    if (s != buf)
    {
        free (s);
    }
}

int
main (int argc, char **argv)
{
    dirs_t  dirs    = { NULL, 0, 0 };
    list_t *files   = NULL;
    char   *dir;
    char   *s       = NULL;
    char   *ss;
    
    /* start with user dir */
    if (!(dir = getenv ("XDG_CONFIG_HOME")))
    {
        /* not defined, use default */
        if ((s = getenv ("HOME")))
        {
            /* 8 = strlen("/.config") + 1 for NULL */
            dir = malloc (sizeof (*dir) * (strlen (s) + 9));
            sprintf (dir, "%s/.config", s);
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
//        process_dir (&files, "/etc/xdg");
    }
    
    /* STEP 3: parse .desktop files, starting what needs to be */
    
    list_t *f;
    
    for (f = files; f; f = f->next)
    {
        printf("%s as in %s\n", f->filename, f->pathname);
        parse_file (f->pathname);
        
        free (f->pathname);
    }
    free (files);
    
    int i;
    for (i = 0; i < dirs.len; ++i)
    {
        free ((void *) dirs.dirs[i]);
    }
    
    return 0;
}

