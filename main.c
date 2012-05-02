
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

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

/**
 * Finds string str in NULL-terminated array list
 * 
 * @param list
 * @param str
 * @return 
 */
static inline char *
find_str (char **list, char *str)
{
    for ( ; *list; ++list)
    {
        if (strcmp (str, *list) == 0)
        {
            return *list;
        }
    }
    return NULL;
}

int
main (int argc, char **argv)
{
    
    /* STEP 1: get list of dirs where to look for .desktop files */
    
    char **dirs;
    int    alloc    = 10;
    int    len      = 0;
    char  *user_dir = NULL;
    char  *cfg_dirs = NULL;
    char  *s;
    char  *ss;
    
    /* NULL-terminated pointers to the dirs to look for .desktop files, in order */
    dirs = malloc (sizeof (*dirs) * (alloc + 1));
    memset (dirs, '\0', alloc + 1);
    
    /* start with user dir */
    if ((s = getenv ("XDG_CONFIG_HOME")))
    {
        dirs[len++] = s;
    }
    /* not defined, use default */
    else if ((s = getenv ("HOME")))
    {
        /* 8 = strlen("/.config") + 1 for NULL */
        ss = user_dir = malloc (sizeof (*ss) * (strlen (s) + 9));
        sprintf (ss, "%s/.config", s);
        dirs[len++] = ss;
    }
    else
    {
        fprintf (stderr, "XDG_CONFIG_HOME not defined, unable to get HOME for default\n");
        return 1;
    }
    
    /* then system wide dirs */
    if ((s = getenv ("XDG_CONFIG_DIRS-")))
    {
        s = cfg_dirs = strdup (s);
        while ((ss = strchr (s, ':')))
        {
            *ss = '\0';
            /* make sure this path isn't already on the list */
            if (!find_str (dirs, s))
            {
                /* do we need to re-alloc? 
                 * Note: this also accounts for the one after the while loop */
                if (len + 1 == alloc)
                {
                    alloc += 10;
                    dirs = realloc (dirs, sizeof (*dirs) * (alloc + 1));
                    memset (&dirs[len + 1], '\0', 10);
                }
                dirs[len++] = s;
            }
            s = ss + 1;
        }
        /* make sure this path isn't already on the list */
        if (!find_str (dirs, s))
        {
            dirs[len++] = s;
        }
    }
    /* not defined, use default */
    else
    {
//        dirs[len++] = "/etc/xdg";
    }
    
    /* STEP 2: look for .desktop files */
    
    list_t        *files = NULL;
    DIR           *dir;
    struct dirent *dirent;
    char         **d;
    char           buf[4096];
    int            l;
    
    for (d = dirs; *d; ++d)
    {
        l = snprintf (buf, 4096, "%s/autostart", *d);
        if (l < 4096)
        {
            s = buf;
        }
        else
        {
            s = malloc (sizeof (*s) * (l + 1));
            snprintf (s, l, "%s/autostart", *d);
        }
        dir = opendir (s);
        while ((dirent = readdir (dir)))
        {
            if (!(dirent->d_type & DT_REG))
            {
                continue;
            }
            
            l = strlen (dirent->d_name);
            /* 8 == strlen (".desktop") */
            if (l < 8 || strcmp (".desktop", &dirent->d_name[l - 8]) != 0)
            {
                continue;
            }
            
            /* add item to list, unless already present (from previous dir)  */
            add_to_list (&files, s, dirent->d_name);
        }
        closedir (dir);
        if (s != buf)
        {
            free (s);
        }
    }
    free (user_dir);
    free (cfg_dirs);
    free (dirs);
    
    /* STEP 3: parse .desktop files, starting what needs to be */
    
    list_t *f;
    
    for (f = files; f; f = f->next)
    {
        printf("%s as in %s\n", f->filename, f->pathname);
        parse_file (f->pathname);
        
        free (f->pathname);
    }
    free (files);
    
    return 0;
}

