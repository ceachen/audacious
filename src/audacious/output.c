/*  Audacious - Cross-platform multimedia player
 *  Copyright (C) 2005-2007  Audacious team
 *
 *  Based on BMP:
 *  Copyright (C) 2003-2004  BMP development team.
 *
 *  Based on XMMS:
 *  Copyright (C) 1998-2003  XMMS development team.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "output.h"
#include "iir.h"
#include "main.h"
#include "input.h"
#include "playback.h"

#include "playlist.h"
#include "libaudacious/util.h"

OutputPluginData op_data = {
    NULL,
    NULL
};

OutputPluginState op_state = {
    0,
    0,
    0
};

OutputPlugin psuedo_output_plugin = {
    NULL,
    NULL,
    "XMMS reverse compatibility output plugin",
    NULL,
    NULL,
    NULL,
    NULL,
    output_get_volume,
    output_set_volume,
    output_open_audio,
    output_write_audio,
    output_close_audio,

    output_flush,
    output_pause,
    output_buffer_free,
    output_buffer_playing,
    get_output_time,
    get_written_time,
    NULL
};

OutputPlugin *
get_current_output_plugin(void)
{
    return op_data.current_output_plugin;
}

void
set_current_output_plugin(gint i)
{
    gboolean playing;
    OutputPlugin *op = get_current_output_plugin();

    GList *node = g_list_nth(op_data.output_list, i);
    if (!node) {
        op_data.current_output_plugin = NULL;
        return;
    }

    op_data.current_output_plugin = node->data;

    playing = playback_get_playing();

    if (playing == TRUE)
    {
        guint time, pos;
	PlaylistEntry *entry;

	/* don't stop yet, get the seek time and playlist position first */
        pos = playlist_get_position(playlist_get_active());
        time = op->output_time();

	/* reset the audio system */
        mainwin_stop_pushed();
        op->close_audio();

	g_usleep(300000);

	/* wait for the playback thread to come online */
        while (playback_get_playing())
            g_message("waiting for audio system shutdown...");

	/* wait for the playback thread to come online */
        playlist_set_position(playlist_get_active(), pos);
	entry = playlist_get_entry_to_play(playlist_get_active());
        playback_play_file(entry);

	while (!playback_get_playing())
	{
	    gtk_main_iteration();
            g_message("waiting for audio system startup...");
	}

	/* and signal a reseek */
        if (playlist_get_current_length(playlist_get_active()) > -1 &&
            time <= (playlist_get_current_length(playlist_get_active())))
	{
            playback_seek(time / 1000);
        }
    }
}

GList *
get_output_list(void)
{
    return op_data.output_list;
}

void
output_about(gint i)
{
    OutputPlugin *out = g_list_nth(op_data.output_list, i)->data;
    if (out && out->about)
        out->about();
}

void
output_configure(gint i)
{
    OutputPlugin *out = g_list_nth(op_data.output_list, i)->data;
    if (out && out->configure)
        out->configure();
}

void
output_get_volume(gint * l, gint * r)
{
    *l = *r = -1;

    if (!op_data.current_output_plugin)
        return;

    if (!op_data.current_output_plugin->get_volume)
        return;

    op_data.current_output_plugin->get_volume(l, r);
}

void
output_set_volume(gint l, gint r)
{
    if (!op_data.current_output_plugin)
        return;

    if (!op_data.current_output_plugin->set_volume)
        return;

    op_data.current_output_plugin->set_volume(l, r);
}

void
output_set_eq(gboolean active, gfloat pre, gfloat * bands)
{
    int i;
    preamp[0] = 1.0 + 0.0932471 * pre + 0.00279033 * pre * pre;
    preamp[1] = 1.0 + 0.0932471 * pre + 0.00279033 * pre * pre;

    for (i = 0; i < 10; ++i)
    {
	set_gain(i, 0, 0.03 * bands[i] + 0.000999999 * bands[i] * bands[i]);
	set_gain(i, 1, 0.03 * bands[i] + 0.000999999 * bands[i] * bands[i]);
    }
}

/* this should be in BYTES, NOT gint16s */
static void
byteswap(size_t size,
         gint16 * buf)
{
    gint16 *it;
    size &= ~1;                  /* must be multiple of 2  */
    for (it = buf; it < buf + size / 2; ++it)
        *(guint16 *) it = GUINT16_SWAP_LE_BE(*(guint16 *) it);
}

/* called by input plugin to peek at the output plugin's write progress */
gint
get_written_time(void)
{
    OutputPlugin *op = get_current_output_plugin();

    return op->written_time();
}

/* called by input plugin to peek at the output plugin's output progress */
gint
get_output_time(void)
{
    OutputPlugin *op = get_current_output_plugin();

    return op->output_time();
}

gint
output_open_audio(AFormat fmt, gint rate, gint nch)
{
    gint ret;
    OutputPlugin *op;
    
    op = get_current_output_plugin();

    if (op == NULL)
        return -1;

    /* Is our output port already open? */
    if ((op_state.rate != 0 && op_state.nch != 0) &&
	(op_state.rate == rate && op_state.nch == nch && op_state.fmt == fmt))
    {
	/* Yes, and it's the correct sampling rate. Reset the counter and go. */
	op->flush(0);
        return 1;
    }
    else if (op_state.rate != 0 && op_state.nch != 0)
	op->close_audio();

    ret = op->open_audio(fmt, rate, nch);

    if (ret == 1)			 /* Success? */
    {
        op_state.fmt = fmt;
        op_state.rate = rate;
        op_state.nch = nch;
    }

    return ret;
}

void
output_write_audio(gpointer ptr, gint length)
{
    OutputPlugin *op = get_current_output_plugin();

    /* Sanity check. */
    if (op == NULL)
        return;

    op->write_audio(ptr, length);
}

void
output_close_audio(void)
{
    OutputPlugin *op = get_current_output_plugin();

    /* Do not close if there are still songs to play and the user has 
     * not requested a stop.  --nenolod
     */
    if (ip_data.stop == FALSE && 
	(playlist_get_position_nolock(playlist_get_active()) < playlist_get_length_nolock(playlist_get_active()) - 1))
        return;

    /* Sanity check. */
    if (op == NULL)
        return;

    op->close_audio();

    /* Reset the op_state. */
    op_state.fmt = op_state.rate = op_state.nch = 0;
}

void
output_flush(gint time)
{
    OutputPlugin *op = get_current_output_plugin();

    if (op == NULL)
        return;

    op->flush(time);
}

void
output_pause(gshort paused)
{
    OutputPlugin *op = get_current_output_plugin();

    if (op == NULL)
        return;

    op->pause(paused);
}

gint
output_buffer_free(void)
{
    OutputPlugin *op = get_current_output_plugin();

    if (op == NULL)
        return 0;

    return op->buffer_free();
}

gint
output_buffer_playing(void)
{
    OutputPlugin *op = get_current_output_plugin();

    if (op == NULL)
        return 0;

    return op->buffer_playing();
}

/* called by input plugin when data is ready */
void
produce_audio(gint time,        /* position             */
              AFormat fmt,      /* output format        */
              gint nch,         /* channels             */
              gint length,      /* length of sample     */
              gpointer ptr,     /* data                 */
              int *going        /* 0 when time to stop  */
              )
{
    static int init = 0;
    int swapped = 0;
    guint myorder = G_BYTE_ORDER == G_LITTLE_ENDIAN ? FMT_S16_LE : FMT_S16_BE;
    int caneq = (fmt == FMT_S16_NE || fmt == myorder);
    OutputPlugin *op = get_current_output_plugin();
    int writeoffs;

    if (!caneq && cfg.equalizer_active) {    /* wrong byte order         */
        byteswap(length, ptr);               /*  so convert              */
        ++swapped;
        ++caneq;
    }                                        /*  can eq now, mark swapd  */
    else if (caneq && !cfg.equalizer_active) /* right order but no eq    */
        caneq = 0;                           /*  so don't eq             */

    if (caneq) {                /* if eq enab               */
        if (!init) {            /*  if first run            */
            init_iir();         /*   then init eq           */
            ++init;
        }

        iir(&ptr, length, nch);

        if (swapped)               /* if was swapped          */
            byteswap(length, ptr); /*  swap back for output   */
    }                           

    /* do vis plugin(s) */
    input_add_vis_pcm(time, fmt, nch, length, ptr);

    writeoffs = 0;
    while (writeoffs < length) {
	int writable = length - writeoffs;

	if (writable > 2048)
	    writable = 2048;

        if (writable == 0)
	    return;

	while (op->buffer_free() < writable) { /* wait output buf            */
	    if (going && !*going)              /*   thread stopped?          */
		return;                        /*     so finish              */

            if (ip_data.stop)                  /* has a stop been requested? */
	        return;                        /*     yes, so finish         */

	    g_usleep(10000);                   /*   else sleep for retry     */
	}

	if (ip_data.stop)
	    return;

	if (going && !*going)                  /*   thread stopped?          */
	    return;                            /*     so finish              */

	/* do output */
	op->write_audio(((guint8 *) ptr) + writeoffs, writable);

	writeoffs += writable;
    }
}