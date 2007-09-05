/*
 * Audacious
 * Copyright (c) 2006-2007 Audacious team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; under version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses>.
 *
 * The Audacious team does not consider modular code linking to
 * Audacious or using our public API to be a derived work.
 */

#include <glib.h>
#include <mowgli.h>

#include "tuple.h"


const gchar *tuple_fields[FIELD_LAST] = {
    "artist",
    "title",
    "album",
    "comment",
    "genre",

    "track",
    "track-number",
    "length",
    "year",
    "quality",

    "codec",
    "file-name",
    "file-path",
    "file-ext",
    "song-artist",

    "mtime",
    "formatter",
    "performer",
    "copyright",
};


static mowgli_heap_t *tuple_heap = NULL;
static mowgli_heap_t *tuple_value_heap = NULL;
static mowgli_object_class_t tuple_klass;

/* iterative destructor of tuple values. */
static void
tuple_value_destroy(mowgli_dictionary_elem_t *delem, gpointer privdata)
{
    TupleValue *value = (TupleValue *) delem->data;

    if (value->type == TUPLE_STRING)
        g_free(value->value.string);

    mowgli_heap_free(tuple_value_heap, value);
}

static void
tuple_destroy(gpointer data)
{
    Tuple *tuple = (Tuple *) data;

    mowgli_dictionary_destroy(tuple->dict, tuple_value_destroy, NULL);
    mowgli_heap_free(tuple_heap, tuple);
}

Tuple *
tuple_new(void)
{
    Tuple *tuple;

    if (tuple_heap == NULL)
    {
        tuple_heap = mowgli_heap_create(sizeof(Tuple), 32, BH_NOW);
        tuple_value_heap = mowgli_heap_create(sizeof(TupleValue), 512, BH_NOW);
        mowgli_object_class_init(&tuple_klass, "audacious.tuple", tuple_destroy, FALSE);
    }

    /* FIXME: use mowgli_object_bless_from_class() in mowgli 0.4
       when it is released --nenolod */
    tuple = mowgli_heap_alloc(tuple_heap);
    mowgli_object_init(mowgli_object(tuple), NULL, &tuple_klass, NULL);

    tuple->dict = mowgli_dictionary_create(g_ascii_strcasecmp);

    return tuple;
}

Tuple *
tuple_new_from_filename(const gchar *filename)
{
    gchar *scratch, *ext, *realfn;
    Tuple *tuple;

    g_return_val_if_fail(filename != NULL, NULL);

    tuple = tuple_new();
    
    g_return_val_if_fail(tuple != NULL, NULL);

    realfn = g_filename_from_uri(filename, NULL, NULL);

    scratch = g_path_get_basename(realfn ? realfn : filename);
    tuple_associate_string(tuple, FIELD_FILE_NAME, NULL, scratch);
    g_free(scratch);

    scratch = g_path_get_dirname(realfn ? realfn : filename);
    tuple_associate_string(tuple, FIELD_FILE_PATH, NULL, scratch);
    g_free(scratch);

    g_free(realfn); realfn = NULL;

    ext = strrchr(filename, '.');
    if (ext != NULL) {
        ++ext;
        tuple_associate_string(tuple, FIELD_FILE_EXT, NULL, scratch);
    }

    return tuple;
}        


static gboolean
tuple_associate_data(const gchar **tfield, TupleValue **value, Tuple *tuple, gint *nfield, const gchar *field)
{
    g_return_val_if_fail(tuple != NULL, FALSE);
    g_return_val_if_fail(*nfield < FIELD_LAST, FALSE);

    /* Check for known fields */
    if (*nfield < 0) {
        gint i;
        *tfield = field;
        for (i = 0; i < FIELD_LAST && *nfield < 0; i++)
            if (!strcmp(field, tuple_fields[i])) *nfield = i;
    }

    if (*nfield >= 0) {
        *tfield = tuple_fields[*nfield];
        tuple->values[*nfield] = NULL;
    }
    
    if ((*value = mowgli_dictionary_delete(tuple->dict, *tfield)))
        tuple_disassociate_now(*value);

    return TRUE;
}

static void
tuple_associate_data2(Tuple *tuple, const gint nfield, const gchar *field, TupleValue *value)
{
    mowgli_dictionary_add(tuple->dict, field, value);

    if (nfield >= 0)
        tuple->values[nfield] = value;
}

gboolean
tuple_associate_string(Tuple *tuple, const gint nfield, const gchar *field, const gchar *string)
{
    TupleValue *value;
    const gchar *tfield;
    gint ifield = nfield;

    if (!tuple_associate_data(&tfield, &value, tuple, &ifield, field))
        return FALSE;

    if (string == NULL)
        return TRUE;

    value = mowgli_heap_alloc(tuple_value_heap);
    value->type = TUPLE_STRING;
    value->value.string = g_strdup(string);

    tuple_associate_data2(tuple, ifield, tfield, value);
    return TRUE;
}

gboolean
tuple_associate_int(Tuple *tuple, const gint nfield, const gchar *field, gint integer)
{
    TupleValue *value;
    const gchar *tfield;
    gint ifield = nfield;
    
    if (!tuple_associate_data(&tfield, &value, tuple, &ifield, field))
        return FALSE;

    value = mowgli_heap_alloc(tuple_value_heap);
    value->type = TUPLE_INT;
    value->value.integer = integer;

    tuple_associate_data2(tuple, ifield, tfield, value);
    return TRUE;
}

void
tuple_disassociate_now(TupleValue *value)
{
    if (value->type == TUPLE_STRING)
        g_free(value->value.string);

    mowgli_heap_free(tuple_value_heap, value);
}

void
tuple_disassociate(Tuple *tuple, const gint nfield, const gchar *field)
{
    TupleValue *value;
    const gchar *tfield;

    g_return_if_fail(tuple != NULL);
    g_return_if_fail(nfield < FIELD_LAST);

    if (nfield < 0)
        tfield = field;
    else {
        tfield = tuple_fields[nfield];
        tuple->values[nfield] = NULL;
    }

    /* why _delete()? because _delete() returns the dictnode's data on success */
    if ((value = mowgli_dictionary_delete(tuple->dict, tfield)) == NULL)
        return;

    tuple_disassociate_now(value);
}

TupleValueType
tuple_get_value_type(Tuple *tuple, const gint nfield, const gchar *field)
{
    TupleValue *value;

    g_return_val_if_fail(tuple != NULL, TUPLE_UNKNOWN);
    g_return_val_if_fail(nfield < FIELD_LAST, TUPLE_UNKNOWN);
    
    if (nfield < 0) {
        if ((value = mowgli_dictionary_retrieve(tuple->dict, field)) != NULL)
            return value->type;
    } else {
        if (tuple->values[nfield])
            return tuple->values[nfield]->type;
    }

    return TUPLE_UNKNOWN;
}

const gchar *
tuple_get_string(Tuple *tuple, const gint nfield, const gchar *field)
{
    TupleValue *value;

    g_return_val_if_fail(tuple != NULL, NULL);
    g_return_val_if_fail(nfield < FIELD_LAST, NULL);

    if (nfield < 0) {
        if ((value = mowgli_dictionary_retrieve(tuple->dict, field)) == NULL)
            return NULL;
    } else {
        if (tuple->values[nfield])
            value = tuple->values[nfield];
        else
            return NULL;
    }

    if (value->type != TUPLE_STRING)
        mowgli_throw_exception_val(audacious.tuple.invalid_type_request, NULL);

    return value->value.string;
}

gint
tuple_get_int(Tuple *tuple, const gint nfield, const gchar *field)
{
    TupleValue *value;

    g_return_val_if_fail(tuple != NULL, 0);
    g_return_val_if_fail(nfield < FIELD_LAST, 0);

    if (nfield < 0) {
        if ((value = mowgli_dictionary_retrieve(tuple->dict, field)) == NULL)
            return 0;
    } else {
        if (tuple->values[nfield])
            value = tuple->values[nfield];
        else
            return 0;
    }

    if (value->type != TUPLE_INT)
        mowgli_throw_exception_val(audacious.tuple.invalid_type_request, 0);

    return value->value.integer;
}
