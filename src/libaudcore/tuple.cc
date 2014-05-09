/*
 * tuple.c
 * Copyright 2007-2013 William Pitcock, Christian Birchinger, Matti Hämäläinen,
 *                     Giacomo Lozito, Eugene Zagidullin, and John Lindgren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include <glib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "audstrings.h"
#include "i18n.h"
#include "tinylock.h"
#include "tuple.h"

#if TUPLE_FIELDS > 64
#error The current tuple implementation is limited to 64 fields
#endif

#define BLOCK_VALS 4

struct TupleBasicType {
    const char *name;
    TupleValueType type;
};

union TupleVal
{
    String str;
    int x;

    // dummy constructor and destructor
    constexpr TupleVal () :
        str () {}
    ~TupleVal () {}
};

/**
 * Structure for holding and passing around miscellaneous track
 * metadata. This is not the same as a playlist entry, though.
 */
struct Tuple {
    uint64_t setmask;      // which fields are present
    Index<TupleVal> vals;  // ordered list of field values

    int *subtunes;                 /**< Array of int containing subtune index numbers.
                                         Can be NULL if indexing is linear or if
                                         there are no subtunes. */
    int nsubtunes;                 /**< Number of subtunes, if any. Values greater than 0
                                         mean that there are subtunes and #subtunes array
                                         may be set. */

    int refcount;
    TinyLock lock;
};

#define BIT(i) ((uint64_t) 1 << (i))

#define LOCK(t) tiny_lock ((TinyLock *) & t->lock)
#define UNLOCK(t) tiny_unlock ((TinyLock *) & t->lock)

/** Ordered table of basic #Tuple field names and their #TupleValueType.
 */
static const TupleBasicType tuple_fields[TUPLE_FIELDS] = {
    { "artist",         TUPLE_STRING },
    { "title",          TUPLE_STRING },
    { "album",          TUPLE_STRING },
    { "comment",        TUPLE_STRING },
    { "genre",          TUPLE_STRING },

    { "track-number",   TUPLE_INT },
    { "length",         TUPLE_INT },
    { "year",           TUPLE_INT },
    { "quality",        TUPLE_STRING },

    { "codec",          TUPLE_STRING },
    { "file-name",      TUPLE_STRING },
    { "file-path",      TUPLE_STRING },
    { "file-ext",       TUPLE_STRING },

    { "song-artist",    TUPLE_STRING },
    { "composer",       TUPLE_STRING },
    { "performer",      TUPLE_STRING },
    { "copyright",      TUPLE_STRING },
    { "date",           TUPLE_STRING },

    { "subsong-id",     TUPLE_INT },
    { "subsong-num",    TUPLE_INT },
    { "mime-type",      TUPLE_STRING },
    { "bitrate",        TUPLE_INT },

    { "segment-start",  TUPLE_INT },
    { "segment-end",    TUPLE_INT },

    { "gain-album-gain", TUPLE_INT },
    { "gain-album-peak", TUPLE_INT },
    { "gain-track-gain", TUPLE_INT },
    { "gain-track-peak", TUPLE_INT },
    { "gain-gain-unit", TUPLE_INT },
    { "gain-peak-unit", TUPLE_INT },
};

struct FieldDictEntry {
    const char * name;
    int field;
};

/* used for binary search, MUST be in alphabetical order */
static const FieldDictEntry field_dict[TUPLE_FIELDS] = {
 {"album", FIELD_ALBUM},
 {"artist", FIELD_ARTIST},
 {"bitrate", FIELD_BITRATE},
 {"codec", FIELD_CODEC},
 {"comment", FIELD_COMMENT},
 {"composer", FIELD_COMPOSER},
 {"copyright", FIELD_COPYRIGHT},
 {"date", FIELD_DATE},
 {"file-ext", FIELD_FILE_EXT},
 {"file-name", FIELD_FILE_NAME},
 {"file-path", FIELD_FILE_PATH},
 {"gain-album-gain", FIELD_GAIN_ALBUM_GAIN},
 {"gain-album-peak", FIELD_GAIN_ALBUM_PEAK},
 {"gain-gain-unit", FIELD_GAIN_GAIN_UNIT},
 {"gain-peak-unit", FIELD_GAIN_PEAK_UNIT},
 {"gain-track-gain", FIELD_GAIN_TRACK_GAIN},
 {"gain-track-peak", FIELD_GAIN_TRACK_PEAK},
 {"genre", FIELD_GENRE},
 {"length", FIELD_LENGTH},
 {"mime-type", FIELD_MIMETYPE},
 {"performer", FIELD_PERFORMER},
 {"quality", FIELD_QUALITY},
 {"segment-end", FIELD_SEGMENT_END},
 {"segment-start", FIELD_SEGMENT_START},
 {"song-artist", FIELD_SONG_ARTIST},
 {"subsong-id", FIELD_SUBSONG_ID},
 {"subsong-num", FIELD_SUBSONG_NUM},
 {"title", FIELD_TITLE},
 {"track-number", FIELD_TRACK_NUMBER},
 {"year", FIELD_YEAR}};

#define VALID_FIELD(f) ((f) >= 0 && (f) < TUPLE_FIELDS)
#define FIELD_TYPE(f) (tuple_fields[f].type)

static int bitcount (uint64_t x)
{
    /* algorithm from http://en.wikipedia.org/wiki/Hamming_weight */
    x -= (x >> 1) & 0x5555555555555555;
    x = (x & 0x3333333333333333) + ((x >> 2) & 0x3333333333333333);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0f;
    return (x * 0x0101010101010101) >> 56;
}

static int field_dict_compare (const void * a, const void * b)
{
    return strcmp (((FieldDictEntry *) a)->name, ((FieldDictEntry *) b)->name);
}

EXPORT int tuple_field_by_name (const char * name)
{
    FieldDictEntry find = {name, -1};
    FieldDictEntry * found = (FieldDictEntry *) bsearch (& find, field_dict,
     TUPLE_FIELDS, sizeof (FieldDictEntry), field_dict_compare);

    return found ? found->field : -1;
}

EXPORT const char * tuple_field_get_name (int field)
{
    g_return_val_if_fail (VALID_FIELD (field), NULL);
    return tuple_fields[field].name;
}

EXPORT TupleValueType tuple_field_get_type (int field)
{
    g_return_val_if_fail (VALID_FIELD (field), TUPLE_UNKNOWN);
    return tuple_fields[field].type;
}

static TupleVal * lookup_val (Tuple * tuple, int field, bool add, bool remove)
{
    /* calculate number of preceding fields */
    int pos = bitcount (tuple->setmask & (BIT (field) - 1));

    if ((tuple->setmask & BIT (field)))
    {
        if (remove)
        {
            if (FIELD_TYPE (field) == TUPLE_STRING)
                tuple->vals[pos].str.~String ();

            tuple->setmask &= ~BIT (field);
            tuple->vals.remove (pos, 1);
            return nullptr;
        }

        return & tuple->vals[pos];
    }

    if (! add)
        return nullptr;

    tuple->setmask |= BIT (field);
    tuple->vals.insert (pos, 1);
    return & tuple->vals[pos];
}

static void tuple_destroy (Tuple * tuple)
{
    auto iter = tuple->vals.begin ();

    for (int f = 0; f < TUPLE_FIELDS; f ++)
    {
        if (tuple->setmask & BIT (f))
        {
            if (FIELD_TYPE (f) == TUPLE_STRING)
                iter->str.~String ();

            iter ++;
        }
    }

    tuple->vals.clear ();

    g_free (tuple->subtunes);
    g_slice_free (Tuple, tuple);
}

EXPORT Tuple * tuple_new (void)
{
    Tuple * tuple = g_slice_new0 (Tuple);
    tuple->refcount = 1;
    return tuple;
}

EXPORT Tuple * tuple_ref (Tuple * tuple)
{
    __sync_fetch_and_add (& tuple->refcount, 1);

    return tuple;
}

EXPORT void tuple_unref (Tuple * tuple)
{
    if (! tuple)
        return;

    if (! __sync_sub_and_fetch (& tuple->refcount, 1))
        tuple_destroy (tuple);
}

EXPORT void tuple_set_filename (Tuple * tuple, const char * filename)
{
    const char * base, * ext, * sub;
    int isub;

    uri_parse (filename, & base, & ext, & sub, & isub);

    char path[base - filename + 1];
    str_decode_percent (filename, base - filename, path);
    tuple_set_str (tuple, FIELD_FILE_PATH, path);

    char name[ext - base + 1];
    str_decode_percent (base, ext - base, name);
    tuple_set_str (tuple, FIELD_FILE_NAME, name);

    if (ext < sub)
    {
        char extbuf[sub - ext];
        str_decode_percent (ext + 1, sub - ext - 1, extbuf);
        tuple_set_str (tuple, FIELD_FILE_EXT, extbuf);
    }

    if (sub[0])
        tuple_set_int (tuple, FIELD_SUBSONG_ID, isub);
}

EXPORT Tuple * tuple_copy (const Tuple * old)
{
    Tuple * tuple = tuple_new ();
    LOCK (old);

    tuple->setmask = old->setmask;
    tuple->vals.insert (0, old->vals.len ());

    auto get = old->vals.begin ();
    auto set = tuple->vals.begin ();

    for (int f = 0; f < TUPLE_FIELDS; f ++)
    {
        if (old->setmask & BIT (f))
        {
            if (FIELD_TYPE (f) == TUPLE_STRING)
                set->str = get->str;
            else
                set->x = get->x;

            get ++;
            set ++;
        }
    }

    tuple->nsubtunes = old->nsubtunes;

    if (old->subtunes)
        tuple->subtunes = (int *) g_memdup (old->subtunes, sizeof (int) * old->nsubtunes);

    UNLOCK (old);
    return tuple;
}

EXPORT Tuple * tuple_new_from_filename (const char * filename)
{
    Tuple * tuple = tuple_new ();
    tuple_set_filename (tuple, filename);
    return tuple;
}

EXPORT void tuple_set_int (Tuple * tuple, int field, int x)
{
    g_return_if_fail (VALID_FIELD (field) && FIELD_TYPE (field) == TUPLE_INT);
    LOCK (tuple);

    TupleVal * val = lookup_val (tuple, field, true, false);
    val->x = x;

    UNLOCK (tuple);
}

EXPORT void tuple_set_str (Tuple * tuple, int field, const char * str)
{
    g_return_if_fail (VALID_FIELD (field) && FIELD_TYPE (field) == TUPLE_STRING);

    if (! str)
    {
        tuple_unset (tuple, field);
        return;
    }

    LOCK (tuple);

    TupleVal * val = lookup_val (tuple, field, true, false);
    val->str = str_to_utf8 (str, -1);

    UNLOCK (tuple);
}

EXPORT void tuple_unset (Tuple * tuple, int field)
{
    g_return_if_fail (VALID_FIELD (field));
    LOCK (tuple);

    lookup_val (tuple, field, false, true);

    UNLOCK (tuple);
}

EXPORT TupleValueType tuple_get_value_type (const Tuple * tuple, int field)
{
    g_return_val_if_fail (VALID_FIELD (field), TUPLE_UNKNOWN);
    LOCK (tuple);

    TupleVal * val = lookup_val ((Tuple *) tuple, field, false, false);
    TupleValueType type = val ? FIELD_TYPE (field) : TUPLE_UNKNOWN;

    UNLOCK (tuple);
    return type;
}

EXPORT String tuple_get_str (const Tuple * tuple, int field)
{
    g_return_val_if_fail (VALID_FIELD (field) && FIELD_TYPE (field) == TUPLE_STRING, String ());
    LOCK (tuple);

    TupleVal * val = lookup_val ((Tuple *) tuple, field, false, false);
    String str = val ? val->str : String ();

    UNLOCK (tuple);
    return str;
}

EXPORT int tuple_get_int (const Tuple * tuple, int field)
{
    g_return_val_if_fail (VALID_FIELD (field) && FIELD_TYPE (field) == TUPLE_INT, -1);
    LOCK (tuple);

    TupleVal * val = lookup_val ((Tuple *) tuple, field, false, false);
    int x = val ? val->x : -1;

    UNLOCK (tuple);
    return x;
}

#define APPEND(b, ...) snprintf (b + strlen (b), sizeof b - strlen (b), __VA_ARGS__)

EXPORT void tuple_set_format (Tuple * t, const char * format, int chans, int rate,
 int brate)
{
    if (format)
        tuple_set_str (t, FIELD_CODEC, format);

    char buf[32];
    buf[0] = 0;

    if (chans > 0)
    {
        if (chans == 1)
            APPEND (buf, _("Mono"));
        else if (chans == 2)
            APPEND (buf, _("Stereo"));
        else
            APPEND (buf, dngettext (PACKAGE, "%d channel", "%d channels", chans), chans);

        if (rate > 0)
            APPEND (buf, ", ");
    }

    if (rate > 0)
        APPEND (buf, "%d kHz", rate / 1000);

    if (buf[0])
        tuple_set_str (t, FIELD_QUALITY, buf);

    if (brate > 0)
        tuple_set_int (t, FIELD_BITRATE, brate);
}

EXPORT void tuple_set_subtunes (Tuple * tuple, int n_subtunes, const int * subtunes)
{
    LOCK (tuple);

    g_free (tuple->subtunes);
    tuple->subtunes = NULL;

    tuple->nsubtunes = n_subtunes;
    if (subtunes)
        tuple->subtunes = (int *) g_memdup (subtunes, sizeof (int) * n_subtunes);

    UNLOCK (tuple);
}

EXPORT int tuple_get_n_subtunes (Tuple * tuple)
{
    LOCK (tuple);

    int n_subtunes = tuple->nsubtunes;

    UNLOCK (tuple);
    return n_subtunes;
}

EXPORT int tuple_get_nth_subtune (Tuple * tuple, int n)
{
    LOCK (tuple);

    int subtune = -1;
    if (n >= 0 && n < tuple->nsubtunes)
        subtune = tuple->subtunes ? tuple->subtunes[n] : 1 + n;

    UNLOCK (tuple);
    return subtune;
}