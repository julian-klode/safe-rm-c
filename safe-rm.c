/* safe-rm.c - wrapper around the rm command to prevent accidental deletions
 *
 * Copyright (C) 2008-2014 Francois Marier
 * Copyright (C) 2015 Julian Andres Klode <jak@jak-linux.org> (C translation)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <search.h>

#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <glob.h>
#include <stdio.h>

/** DEFAULT_PROTECTED_DIRS - Directories protected by default */
static const char *DEFAULT_PROTECTED_DIRS[] = {
    "/bin",
    "/boot",
    "/dev",
    "/etc",
    "/home",
    "/initrd",
    "/lib",
    "/lib32",
    "/lib64",
    "/proc",
    "/root",
    "/sbin",
    "/sys",
    "/usr",
    "/usr/bin",
    "/usr/include",
    "/usr/lib",
    "/usr/local",
    "/usr/local/bin",
    "/usr/local/include",
    "/usr/local/sbin",
    "/usr/local/share",
    "/usr/sbin",
    "/usr/share",
    "/usr/src",
    "/var",
    NULL,
};

/**
 * protected_dirs - A binary search tree of protected directories
 *
 * This is used with tsearch() and friends
 */
void *protected_dirs;

/**
 * strcmpvoid - Check whether string @a matches string @b
 * @a: the first string
 * @b: the second string
 *
 * Returns: strcmp(a, b).
 */
int strcmpvoid(const void *a, const void *b)
{
    return strcmp(a, b);
}

/**
 * strany - Check if any string is not NULL.
 * @a: the first string
 * @b: the second string
 *
 * This can be used with tfind to check for emptyness:
 *   tfind(NULL, &rootp, strany)
 * will return the first element in the tree.
 *
 * Returns: 0 if a or b is not NULL, 1 otherwise.
 */
int strany(const void *a, const void *b)
{
    return (a != NULL || b != NULL) ? 0 : 1;
}

/**
 * rtrim - Trim @value
 * @value: The string that shall be trimmed.
 * @totrim: Characters that shall be trimmed
 *
 * Replaces all characters from @totrim on the right hand side of @value.
 */
void rtrim(char *value, char *totrim)
{
    size_t len = strlen(value);

    while (--len > 0 && strchr(totrim, value[len]) != NULL) {
        value[len] = 0;
    }
}

/**
 * read_config_file - Read a configuration file
 * @path: The path to the configuration file
 *
 */
void read_config_file(const char *path)
{
    FILE *infile;
    char *line = NULL;
    size_t line_alloc = 0;
    glob_t globbed;

    infile = fopen(path, "r");

    if (infile == NULL) {
        if (errno != ENOENT) {
            fprintf(stderr, "Could not open configuration file %s: %s\n", path,
                    strerror(errno));
        }
        return;
    }

    while (getline(&line, &line_alloc, infile) != -1) {
        rtrim(line, "\n\r\t ");
        switch (glob(line, 0, NULL, &globbed)) {
        case GLOB_NOMATCH:
        case 0:
            break;
        default:
            fprintf(stderr, "Cannot glob() for line %s\n", line);
            exit(1);
        }

        /* Insert the matches into our tree of paths */
        for (size_t i = 0; i < globbed.gl_pathc; i++)
            tsearch(globbed.gl_pathv[i], &protected_dirs, strcmpvoid);
    }
}

/**
 * join - Join two paths
 * @a: First path
 * @b: Second path
 *
 * Returns: @a/@b
 */
char *join(const char *a, const char *b)
{
    size_t lena;
    size_t lenb;
    char *out;

    lena = strlen(a);
    lenb = strlen(b);

    out = malloc(lena + 1 /* separator */  + lenb + 1 /* 0 byte */ );

    strncpy(out, a, lena);
    strncpy(out + lena, "/", 1);
    strncpy(out + lena + 1, b, lenb);

    out[lena + 1 + lenb] = '\0';

    return out;
}

int main(int argc, char *argv[])
{
    const char *HOME = getenv("HOME") ? : "";
    const char *XDG_CONFIG_HOME =
        getenv("XDG_CONFIG_HOME") ? : join(HOME, ".config");
    const char *LEGACY_CONFIG_FILE = join(HOME, ".safe-rm");
    const char *USER_CONFIG_FILE = join(XDG_CONFIG_HOME, "safe-rm");
    const char *GLOBAL_CONFIG_FILE = "/etc/safe-rm.conf";
    /* Allocate one more parameter than necessary, so we can quickly
     * prepend something for testing. */
    char **allowed_args = calloc(argc + 2, sizeof(*allowed_args));
    int allowed = 0;

    read_config_file(GLOBAL_CONFIG_FILE);
    read_config_file(LEGACY_CONFIG_FILE);
    read_config_file(USER_CONFIG_FILE);

    /* Insert defaults if no configuration option was read. */
    if (tfind(NULL, &protected_dirs, strany) == NULL) {
        for (int i = 0; DEFAULT_PROTECTED_DIRS[i] != NULL; i++)
            tsearch(DEFAULT_PROTECTED_DIRS[i], &protected_dirs, strcmpvoid);
    }

    /* Build the array of allowed arguments (TODO: Disable the echo test.) */
    allowed_args[allowed++] = "/bin/echo";
    allowed_args[allowed++] = "/bin/rm";

    for (int i = 1; i < argc; i++) {
        struct stat buf;
        char *pathname = argv[i];
        /* Normalize the pathname */
        char *normalized_pathname = pathname;

        /* stat the file */
        buf.st_mode = 0;
        lstat(pathname, &buf);
        
        /* Try looking up the real path */
        if (!S_ISLNK(buf.st_mode)) {
            char *cpath = realpath(pathname, NULL);
            if (cpath != NULL)
                normalized_pathname = cpath;
        }

        /* Trim trailing slashes */
        rtrim(normalized_pathname, "/");

        /* Check against the blacklist */
        if (tfind(normalized_pathname, &protected_dirs, strcmpvoid) != NULL) {
            fprintf(stderr, "safe-rm: skipping %s\n", pathname);
        } else {
            allowed_args[allowed++] = normalized_pathname;
        }
    }
    allowed_args[allowed] = NULL;

    /* Make sure we're not calling ourselves recursively */
    if (strcmp(realpath(allowed_args[0], NULL), realpath(argv[0], NULL)) == 0) {
        fprintf(stderr, "safe-rm cannot find the real \"rm\" binary\n");
        exit(1);
    }

    /* Run the real rm command, returning with the same error code */
    if (execv(allowed_args[0], allowed_args) != 0) {
        fprintf(stderr, "safe-rm: Cannot execute the real \"rm\" binary: %s\n",
                strerror(errno));
        exit(2);
    }
}
