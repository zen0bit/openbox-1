/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   obt/ddfile.c for the Openbox window manager
   Copyright (c) 2009        Dana Jansens

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   See the COPYING file for a copy of the GNU General Public License.
*/

#include "obt/ddfile.h"
#include <glib.h>
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

typedef struct _ObtDDParse ObtDDParse;
typedef struct _ObtDDParseGroup ObtDDParseGroup;

typedef void (*ObtDDParseGroupFunc)(gchar *key, const gchar *val,
                                    ObtDDParse *parse, gboolean *error);

struct _ObtDDParseGroup {
    gchar *name;
    gboolean seen;
    ObtDDParseGroupFunc key_func;
    /* the key is a string (a key inside the group in the .desktop).
       the value is an ObtDDParseValue */
    GHashTable *key_hash;
};

struct _ObtDDParse {
    gchar *filename;
    gulong lineno;
    ObtDDParseGroup *group;
    /* the key is a group name, the value is a ObtDDParseGroup */
    GHashTable *group_hash;
};

typedef enum {
    DATA_STRING,
    DATA_LOCALESTRING,
    DATA_STRINGS,
    DATA_LOCALESTRINGS,
    DATA_BOOLEAN,
    DATA_NUMERIC,
    NUM_DATA_TYPES
} ObtDDDataType;

typedef struct _ObtDDParseValue {
    ObtDDDataType type;
    union _ObtDDParseValueValue {
        gchar *string;
        struct _ObtDDParseValueStrings {
            gchar *s;
            gulong n;
        } strings;
        gboolean boolean;
        gfloat numeric;
    } value;
} ObtDDParseValue;

struct _ObtDDFile {
    guint ref;

    ObtDDFileType type;
    gchar *name; /*!< Specific name for the object (eg Firefox) */
    gchar *generic; /*!< Generic name for the object (eg Web Browser) */
    gchar *comment; /*!< Comment/description to display for the object */
    gchar *icon; /*!< Name/path for an icon for the object */

    union _ObtDDFileData {
        struct _ObtDDFileApp {
            gchar *exec; /*!< Executable to run for the app */
            gchar *wdir; /*!< Working dir to run the app in */
            gboolean term; /*!< Run the app in a terminal or not */
            ObtDDFileAppOpen open;

            /* XXX gchar**? or something better, a mime struct.. maybe
               glib has something i can use. */
            gchar **mime; /*!< Mime types the app can open */

            ObtDDFileAppStartup startup;
            gchar *startup_wmclass;
        } app;
        struct _ObtDDFileLink {
            gchar *url;
        } link;
        struct _ObtDDFileDir {
        } dir;
    } d;
};

static void value_free(ObtDDParseValue *v)
{
    switch (v->type) {
    case DATA_STRING:
    case DATA_LOCALESTRING:
        g_free(v->value.string); break;
    case DATA_STRINGS:
    case DATA_LOCALESTRINGS:
        g_free(v->value.strings.s);
        v->value.strings.n = 0;
        break;
    case DATA_BOOLEAN:
        break;
    case DATA_NUMERIC:
        break;
    default:
        g_assert_not_reached();
    }
    g_slice_free(ObtDDParseValue, v);
}

static ObtDDParseGroup* group_new(gchar *name, ObtDDParseGroupFunc f)
{
    ObtDDParseGroup *g = g_slice_new(ObtDDParseGroup);
    g->name = name;
    g->key_func = f;
    g->seen = FALSE;
    g->key_hash = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, (GDestroyNotify)value_free);
    return g;
}

static void group_free(ObtDDParseGroup *g)
{
    g_free(g->name);
    g_hash_table_destroy(g->key_hash);
    g_slice_free(ObtDDParseGroup, g);
}

/* Displays a warning message including the file name and line number, and
   sets the boolean @error to true if it points to a non-NULL address.
*/
static void parse_error(const gchar *m, const ObtDDParse *const parse,
                        gboolean *error)
{
    if (!parse->filename)
        g_warning("%s at line %lu of input", m, parse->lineno);
    else
        g_warning("%s at line %lu of file %s",
                  m, parse->lineno, parse->filename);
    if (error) *error = TRUE;
}

/* reads an input string, strips out invalid stuff, and parses
   backslash-stuff
   if @nstrings is not NULL, then it splits the output string at ';'
   characters.  they are all returned in the same string with null zeros
   between them, @nstrings is set to the number of such strings.
 */
static gchar* parse_string(const gchar *in,
                           gboolean locale,
                           gulong *nstrings,
                           const ObtDDParse *const parse,
                           gboolean *error)
{
    const gint bytes = strlen(in);
    gboolean backslash;
    gchar *out, *o;
    const gchar *end, *i;

    g_return_val_if_fail(in != NULL, NULL);

    if (!locale) {
        end = in + bytes;
        for (i = in; i < end; ++i) {
            if ((guchar)*i > 126 || (guchar)*i < 32) {
                /* non-control character ascii */
                end = i;
                parse_error("Invalid bytes in string", parse, error);
                break;
            }
        }
    }
    else if (!g_utf8_validate(in, bytes, &end))
        parse_error("Invalid bytes in localestring", parse, error);

    if (nstrings) *nstrings = 1;

    out = g_new(char, bytes + 1);
    i = in; o = out;
    backslash = FALSE;
    while (i < end) {
        const gchar *next = locale ? g_utf8_find_next_char(i, end) : i+1;
        if (backslash) {
            switch(*i) {
            case 's': *o++ = ' '; break;
            case 'n': *o++ = '\n'; break;
            case 't': *o++ = '\t'; break;
            case 'r': *o++ = '\r'; break;
            case ';': *o++ = ';'; break;
            case '\\': *o++ = '\\'; break;
            default:
                parse_error((locale ?
                             "Invalid escape sequence in localestring" :
                             "Invalid escape sequence in string"),
                            parse, error);
            }
            backslash = FALSE;
        }
        else if (*i == '\\')
            backslash = TRUE;
        else if (*i == ';' && nstrings) {
            ++nstrings;
            *o = '\0';
        }
        else if ((guchar)*i >= 127 || (guchar)*i < 32) {
            /* avoid ascii control characters */
            parse_error("Found control character in string", parse, error);
            break;
        }
        else {
            memcpy(o, i, next-i);
            o += next-i;
        }
        i = next;
    }
    *o = '\0';
    return o;
}

static gboolean parse_bool(const gchar *in, const ObtDDParse *const parse,
                           gboolean *error)
{
    if (strcmp(in, "true") == 0)
        return TRUE;
    else if (strcmp(in, "false") != 0)
        parse_error("Invalid boolean value", parse, error);
    return FALSE;
}

static gfloat parse_numeric(const gchar *in, const ObtDDParse *const parse,
    gboolean *error)
{
    gfloat out = 0;
    if (sscanf(in, "%f", &out) == 0)
        parse_error("Invalid numeric value", parse, error);
    return out;
}

static void parse_group_desktop_entry(gchar *key, const gchar *val,
                                      ObtDDParse *parse, gboolean *error)
{
    ObtDDParseValue v, *pv;

    /* figure out value type */
    v.type = NUM_DATA_TYPES;

    /* parse the value */
    
    switch (v.type) {
    case DATA_STRING:
        v.value.string = parse_string(val, FALSE, NULL, parse, error);
        g_assert(v.value.string);
        break;
    case DATA_LOCALESTRING:
        v.value.string = parse_string(val, TRUE, NULL, parse, error);
        g_assert(v.value.string);
        break;
    case DATA_STRINGS:
        v.value.strings.s = parse_string(val, FALSE, &v.value.strings.n,
                                         parse, error);
        g_assert(v.value.strings.s);
        g_assert(v.value.strings.n);
        break;
    case DATA_LOCALESTRINGS:
        v.value.strings.s = parse_string(val, TRUE, &v.value.strings.n,
                                         parse, error);
        g_assert(v.value.strings.s);
        g_assert(v.value.strings.n);
        break;
    case DATA_BOOLEAN:
        v.value.boolean = parse_bool(val, parse, error);
        break;
    case DATA_NUMERIC:
        v.value.numeric = parse_numeric(val, parse, error);
        break;
    default:
        g_assert_not_reached();
    }

    pv = g_slice_new(ObtDDParseValue);
    *pv = v;
    g_hash_table_insert(parse->group->key_hash, key, pv);
}

static gboolean parse_file_line(FILE *f, gchar **buf,
                                gulong *size, gulong *read,
                                ObtDDParse *parse, gboolean *error)
{
    const gulong BUFMUL = 80;
    size_t ret;
    gulong i, null;

    if (*size == 0) {
        g_assert(*read == 0);
        *size = BUFMUL;
        *buf = g_new(char, *size);
    }

    /* remove everything up to a null zero already in the buffer and shift
       the rest to the front */
    null = *size;
    for (i = 0; i < *read; ++i) {
        if (null < *size)
            (*buf)[i-null-1] = (*buf)[i];
        else if ((*buf)[i] == '\0')
            null = i;
    }
    if (null < *size)
        *read -= null + 1;

    /* is there already a newline in the buffer? */
    for (i = 0; i < *read; ++i)
        if ((*buf)[i] == '\n') {
            /* turn it into a null zero and done */
            (*buf)[i] = '\0';
            return TRUE;
        }

    /* we need to read some more to find a newline */
    while (TRUE) {
        gulong eol;
        gchar *newread;

        newread = *buf + *read;
        ret = fread(newread, sizeof(char), *size-*read, f);
        if (ret < *size - *read && !feof(f)) {
            parse_error("Error reading", parse, error);
            return FALSE;
        }
        *read += ret;

        /* strip out null zeros in the input and look for an endofline */
        null = 0;
        eol = *size;
        for (i = newread-*buf; i < *read; ++i) {
            if (null > 0)
                (*buf)[i] = (*buf)[i+null];
            if ((*buf)[i] == '\0') {
                ++null;
                --(*read);
                --i; /* try again */
            }
            else if ((*buf)[i] == '\n' && eol == *size) {
                eol = i;
                /* turn it into a null zero */
                (*buf)[i] = '\0';
            }
        }

        if (eol != *size)
            /* found an endofline, done */
            break;
        else if (feof(f) && *read < *size) {
            /* found the endoffile, done (if there is space) */
            if (*read > 0) {
                /* stick a null zero on if there is test on the last line */
                (*buf)[(*read)++] = '\0';
            }
            break;
        }
        else {
            /* read more */
            size += BUFMUL;
            *buf = g_renew(char, *buf, *size);
        }
    }
    return *read > 0;
}

static void parse_group(const gchar *buf, gulong len,
                        ObtDDParse *parse, gboolean *error)
{
    ObtDDParseGroup *g;
    gchar *group;
    gulong i;

    /* get the group name */
    group = g_strndup(buf+1, len-2);
    for (i = 0; i < len-2; ++i)
        if ((guchar)group[i] < 32 || (guchar)group[i] >= 127) {
            /* valid ASCII only */
            parse_error("Invalid character found", parse, NULL);
            group[i] = '\0'; /* stopping before this character */
            break;
        }

    /* make sure it's a new group */
    g = g_hash_table_lookup(parse->group_hash, group);
    if (g && g->seen) {
        parse_error("Duplicate group found", parse, error);
        g_free(group);
        return;
    }
    /* if it's the first group, make sure it's named Desktop Entry */
    else if (!parse->group && strcmp(group, "Desktop Entry") != 0)
    {
        parse_error("Incorrect group found, "
                    "expected [Desktop Entry]",
                    parse, error);
        g_free(group);
        return;
    }
    else {
        if (!g) {
            g = group_new(group, NULL);
            g_hash_table_insert(parse->group_hash, g->name, g);
        }
        else
            g_free(group);

        g->seen = TRUE;
        parse->group = g;
        g_print("Found group %s\n", g->name);
    }
}

static void parse_key_value(const gchar *buf, gulong len,
                            ObtDDParse *parse, gboolean *error)
{
    gulong i, keyend, valstart, eq;
    char *key;

    /* find the end of the key */
    for (i = 0; i < len; ++i)
        if (!(((guchar)buf[i] >= 'A' && (guchar)buf[i] <= 'Z') ||
              ((guchar)buf[i] >= 'a' && (guchar)buf[i] <= 'z') ||
              ((guchar)buf[i] >= '0' && (guchar)buf[i] <= '9') ||
              ((guchar)buf[i] == '-'))) {
            /* not part of the key */
            keyend = i;
            break;
        }
    if (keyend < 1) {
        parse_error("Empty key", parse, error);
        return;
    }
    /* find the = character */
    for (i = keyend; i < len; ++i) {
        if (buf[i] == '=') {
            eq = i;
            break;
        }
        else if (buf[i] != ' ') {
            parse_error("Invalid character in key name", parse, error);
            return ;
        }
    }
    if (i == len) {
        parse_error("Key without value found", parse, error);
        return;
    }
    /* find the start of the value */
    for (i = eq+1; i < len; ++i)
        if (buf[i] != ' ') {
            valstart = i;
            break;
        }
    if (i == len) {
        parse_error("Empty value found", parse, error);
        return;
    }

    key = g_strndup(buf, keyend);
    if (g_hash_table_lookup(parse->group->key_hash, key)) {
        parse_error("Duplicate key found", parse, error);
        g_free(key);
        return;
    }
    g_print("Found key/value %s=%s.\n", key, buf+valstart);
    if (parse->group->key_func)
        parse->group->key_func(key, buf+valstart, parse, error);
}

static gboolean parse_file(ObtDDFile *dd, FILE *f, ObtDDParse *parse)
{
    gchar *buf = NULL;
    gulong bytes = 0, read = 0;
    gboolean error = FALSE;

    while (!error && parse_file_line(f, &buf, &bytes, &read, parse, &error)) {
        /* XXX use the string in buf */
        gulong len = strlen(buf);
        if (buf[0] == '#' || buf[0] == '\0')
            ; /* ignore comment lines */
        else if (buf[0] == '[' && buf[len-1] == ']')
            parse_group(buf, len, parse, &error);
        else if (!parse->group)
            /* just ignore keys outside of groups */
            parse_error("Key found before group", parse, NULL);
        else
            /* ignore errors in key-value pairs and continue */
            parse_key_value(buf, len, parse, NULL);
        ++parse->lineno;
    }

    if (buf) g_free(buf);
    return !error;
}

ObtDDFile* obt_ddfile_new_from_file(const gchar *name, GSList *paths)
{
    ObtDDFile *dd;
    ObtDDParse parse;
    ObtDDParseGroup *desktop_entry;
    GSList *it;
    FILE *f;
    gboolean success;

    dd = g_slice_new(ObtDDFile);
    dd->ref = 1;

    parse.filename = NULL;
    parse.lineno = 0;
    parse.group = NULL;
    parse.group_hash = g_hash_table_new_full(g_str_hash,
                                             g_str_equal,
                                             NULL,
                                             (GDestroyNotify)group_free);

    /* set up the groups (there's only one right now) */
    desktop_entry = group_new(g_strdup("Desktop Entry"),
                              parse_group_desktop_entry);
    g_hash_table_insert(parse.group_hash, desktop_entry->name, desktop_entry);

    success = FALSE;
    for (it = paths; it && !success; it = g_slist_next(it)) {
        gchar *path = g_strdup_printf("%s/%s", (char*)it->data, name);
        if ((f = fopen(path, "r"))) {
            parse.filename = path;
            parse.lineno = 1;
            success = parse_file(dd, f, &parse);
            fclose(f);
        }
        g_free(path);
    }
    if (!success) {
        obt_ddfile_unref(dd);
        dd = NULL;
    }

    g_hash_table_destroy(parse.group_hash);

    return dd;
}

void obt_ddfile_ref(ObtDDFile *dd)
{
    ++dd->ref;
}

void obt_ddfile_unref(ObtDDFile *dd)
{
    if (--dd->ref < 1) {
        g_slice_free(ObtDDFile, dd);
    }
}