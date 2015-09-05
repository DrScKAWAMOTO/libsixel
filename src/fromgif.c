/*
 * This file is derived from "stb_image.h" that is in public domain.
 * https://github.com/nothings/stb
 *
 * Hayaki Saito <saitoha@me.com> modified this and re-licensed
 * it under the MIT license.
 *
 * Copyright (c) 2015 Hayaki Saito
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "frame.h"
#include "fromgif.h"

/*
 * gif_context_t struct and start_xxx functions
 *
 * gif_context_t structure is our basic context used by all images, so it
 * contains all the IO context, plus some basic image information
 */
typedef struct
{
   unsigned int img_x, img_y;
   int img_n, img_out_n;

   int buflen;
   unsigned char buffer_start[128];

   unsigned char *img_buffer, *img_buffer_end;
   unsigned char *img_buffer_original;
} gif_context_t;

typedef struct
{
   signed short prefix;
   unsigned char first;
   unsigned char suffix;
} gif_lzw;

typedef struct
{
   int w, h;
   unsigned char *out;  /* output buffer (always 4 components) */
   int flags, bgindex, ratio, transparent, eflags;
   unsigned char pal[256][3];
   unsigned char lpal[256][3];
   gif_lzw codes[4096];
   unsigned char *color_table;
   int parse, step;
   int lflags;
   int start_x, start_y;
   int max_x, max_y;
   int cur_x, cur_y;
   int line_size;
   int loop_count;
   int delay;
   int is_multiframe;
   int is_terminated;
} gif_t;


/* initialize a memory-decode context */
static unsigned char
gif_get8(gif_context_t *s)
{
    if (s->img_buffer < s->img_buffer_end) {
        return *s->img_buffer++;
    }
    return 0;
}


static int
gif_get16le(gif_context_t *s)
{
    int z = gif_get8(s);
    return z + (gif_get8(s) << 8);
}


static void
gif_parse_colortable(
    gif_context_t /* in */ *s,
    unsigned char       /* in */ pal[256][3],
    int           /* in */ num_entries)
{
    int i;

    for (i = 0; i < num_entries; ++i) {
        pal[i][2] = gif_get8(s);
        pal[i][1] = gif_get8(s);
        pal[i][0] = gif_get8(s);
    }
}


static SIXELSTATUS
gif_load_header(
    gif_context_t /* in */ *s,
    gif_t         /* in */ *g)
{
    SIXELSTATUS status = SIXEL_FALSE;
    unsigned char version;
    if (gif_get8(s) != 'G') {
        goto end;
    }
    if (gif_get8(s) != 'I') {
        goto end;
    }
    if (gif_get8(s) != 'F') {
        goto end;
    }
    if (gif_get8(s) != '8') {
        goto end;
    }

    version = gif_get8(s);

    if (version != '7' && version != '9') {
        goto end;
    }
    if (gif_get8(s) != 'a') {
        goto end;
    }

    g->w = gif_get16le(s);
    g->h = gif_get16le(s);
    g->flags = gif_get8(s);
    g->bgindex = gif_get8(s);
    g->ratio = gif_get8(s);
    g->transparent = (-1);
    g->loop_count = (-1);

    if (g->flags & 0x80) {
        gif_parse_colortable(s, g->pal, 2 << (g->flags & 7));
    }

    status = SIXEL_OK;

end:
    return status;
}


static SIXELSTATUS
gif_init_frame(
    sixel_frame_t /* in */ *frame,
    gif_t         /* in */ *pg,
    unsigned char /* in */ *bgcolor,
    int           /* in */ reqcolors,
    int           /* in */ fuse_palette)
{
    SIXELSTATUS status = SIXEL_OK;
    int i;
    int ncolors;

    frame->delay = pg->delay;
    ncolors = 2 << (pg->flags & 7);
    if (frame->palette == NULL) {
        frame->palette = (unsigned char *)sixel_allocator_malloc(frame->allocator, ncolors * 3);
    } else if (frame->ncolors < ncolors) {
        sixel_allocator_free(frame->allocator, frame->palette);
        frame->palette = (unsigned char *)sixel_allocator_malloc(frame->allocator, ncolors * 3);
    }
    if (frame->palette == NULL) {
        sixel_helper_set_additional_message(
            "gif_init_frame: sixel_allocator_malloc() failed.");
        status = SIXEL_BAD_ALLOCATION;
        goto end;
    }
    frame->ncolors = ncolors;
    if (frame->ncolors <= reqcolors && fuse_palette) {
        frame->pixelformat = SIXEL_PIXELFORMAT_PAL8;
        sixel_allocator_free(frame->allocator, frame->pixels);
        frame->pixels = (unsigned char *)sixel_allocator_malloc(frame->allocator,
                                                                frame->width * frame->height);
        if (frame->pixels == NULL) {
            sixel_helper_set_additional_message(
                "sixel_allocator_malloc() failed in gif_init_frame().");
            status = SIXEL_BAD_ALLOCATION;
            goto end;
        }
        memcpy(frame->pixels, pg->out, frame->width * frame->height);

        for (i = 0; i < frame->ncolors; ++i) {
            frame->palette[i * 3 + 0] = pg->color_table[i * 3 + 2];
            frame->palette[i * 3 + 1] = pg->color_table[i * 3 + 1];
            frame->palette[i * 3 + 2] = pg->color_table[i * 3 + 0];
        }
        if (pg->lflags & 0x80) {
            if (pg->eflags & 0x01) {
                if (bgcolor) {
                    frame->palette[pg->transparent * 3 + 0] = bgcolor[0];
                    frame->palette[pg->transparent * 3 + 1] = bgcolor[1];
                    frame->palette[pg->transparent * 3 + 2] = bgcolor[2];
                } else {
                    frame->transparent = pg->transparent;
                }
            }
        } else if (pg->flags & 0x80) {
            if (pg->eflags & 0x01) {
                if (bgcolor) {
                    frame->palette[pg->transparent * 3 + 0] = bgcolor[0];
                    frame->palette[pg->transparent * 3 + 1] = bgcolor[1];
                    frame->palette[pg->transparent * 3 + 2] = bgcolor[2];
                } else {
                    frame->transparent = pg->transparent;
                }
            }
        }
    } else {
        frame->pixelformat = SIXEL_PIXELFORMAT_RGB888;
        frame->pixels = (unsigned char *)sixel_allocator_malloc(frame->allocator,
                                                                pg->w * pg->h * 3);
        if (frame->pixels == NULL) {
            sixel_helper_set_additional_message(
                "sixel_allocator_malloc() failed in gif_init_frame().");
            status = SIXEL_BAD_ALLOCATION;
            goto end;
        }
        for (i = 0; i < pg->w * pg->h; ++i) {
            frame->pixels[i * 3 + 0] = pg->color_table[pg->out[i] * 3 + 2];
            frame->pixels[i * 3 + 1] = pg->color_table[pg->out[i] * 3 + 1];
            frame->pixels[i * 3 + 2] = pg->color_table[pg->out[i] * 3 + 0];
        }
    }
    frame->multiframe = (pg->loop_count != (-1));

    status = SIXEL_OK;

end:
    return status;
}


static void
gif_out_code(
    gif_t           /* in */ *g,
    unsigned short  /* in */ code
)
{
    /* recurse to decode the prefixes, since the linked-list is backwards,
       and working backwards through an interleaved image would be nasty */
    if (g->codes[code].prefix >= 0) {
        gif_out_code(g, g->codes[code].prefix);
    }

    if (g->cur_y >= g->max_y) {
        return;
    }

    g->out[g->cur_x + g->cur_y] = g->codes[code].suffix;
    g->cur_x++;

    if (g->cur_x >= g->max_x) {
        g->cur_x = g->start_x;
        g->cur_y += g->step;

        while (g->cur_y >= g->max_y && g->parse > 0) {
            g->step = (1 << g->parse) * g->line_size;
            g->cur_y = g->start_y + (g->step >> 1);
            --g->parse;
        }
    }
}


static SIXELSTATUS
gif_process_raster(
    gif_context_t /* in */ *s,
    gif_t         /* in */ *g
)
{
    SIXELSTATUS status = SIXEL_FALSE;
    unsigned char lzw_cs;
    signed int len, code;
    unsigned int first;
    signed int codesize, codemask, avail, oldcode, bits, valid_bits, clear;
    gif_lzw *p;

    lzw_cs = gif_get8(s);
    clear = 1 << lzw_cs;
    first = 1;
    codesize = lzw_cs + 1;
    codemask = (1 << codesize) - 1;
    bits = 0;
    valid_bits = 0;
    for (code = 0; code < clear; code++) {
        g->codes[code].prefix = -1;
        g->codes[code].first = (unsigned char) code;
        g->codes[code].suffix = (unsigned char) code;
    }

    /* support no starting clear code */
    avail = clear + 2;
    oldcode = (-1);

    len = 0;
    for(;;) {
        if (valid_bits < codesize) {
            if (len == 0) {
                len = gif_get8(s); /* start new block */
                if (len == 0) {
                    return SIXEL_OK;
                }
            }
            --len;
            bits |= (signed int) gif_get8(s) << valid_bits;
            valid_bits += 8;
        } else {
            code = bits & codemask;
            bits >>= codesize;
            valid_bits -= codesize;
            /* @OPTIMIZE: is there some way we can accelerate the non-clear path? */
            if (code == clear) {  /* clear code */
                codesize = lzw_cs + 1;
                codemask = (1 << codesize) - 1;
                avail = clear + 2;
                oldcode = -1;
                first = 0;
            } else if (code == clear + 1) { /* end of stream code */
                s->img_buffer += len;
                while ((len = gif_get8(s)) > 0) {
                    s->img_buffer += len;
                }
                return SIXEL_OK;
            } else if (code <= avail) {
                if (first) {
                    sixel_helper_set_additional_message(
                        "corrupt GIF (reason: no clear code).");
                    status = SIXEL_RUNTIME_ERROR;
                    goto end;
                }
                if (oldcode >= 0) {
                    p = &g->codes[avail++];
                    if (avail > 4096) {
                        sixel_helper_set_additional_message(
                            "corrupt GIF(reason: too many codes).");
                        status = SIXEL_RUNTIME_ERROR;
                        goto end;
                    }
                    p->prefix = (signed short) oldcode;
                    p->first = g->codes[oldcode].first;
                    p->suffix = (code == avail) ? p->first : g->codes[code].first;
                } else if (code == avail) {
                    sixel_helper_set_additional_message(
                        "corrupt GIF (reason: illegal code in raster).");
                    status = SIXEL_RUNTIME_ERROR;
                    goto end;
                }

                gif_out_code(g, (unsigned short) code);

                if ((avail & codemask) == 0 && avail <= 0x0FFF) {
                    codesize++;
                    codemask = (1 << codesize) - 1;
                }

                oldcode = code;
            } else {
                sixel_helper_set_additional_message(
                    "corrupt GIF (reason: illegal code in raster).");
                status = SIXEL_RUNTIME_ERROR;
                goto end;
            }
        }
    }

    status = SIXEL_OK;

end:
    return status;
}


/* this function is ported from stb_image.h */
static SIXELSTATUS
gif_load_next(
    gif_context_t /* in */ *s,
    gif_t         /* in */ *g,
    unsigned char /* in */ *bgcolor
)
{
    SIXELSTATUS status = SIXEL_FALSE;
    unsigned char buffer[256];
    int x;
    int y;
    int w;
    int h;
    int len;

    for (;;) {
        switch (gif_get8(s)) {
        case 0x2C: /* Image Descriptor */
            x = gif_get16le(s);
            y = gif_get16le(s);
            w = gif_get16le(s);
            h = gif_get16le(s);
            if (((x + w) > (g->w)) || ((y + h) > (g->h))) {
                sixel_helper_set_additional_message(
                    "corrupt GIF (reason: bad Image Descriptor).");
                status = SIXEL_RUNTIME_ERROR;
                goto end;
            }

            g->line_size = g->w;
            g->start_x = x;
            g->start_y = y * g->line_size;
            g->max_x   = g->start_x + w;
            g->max_y   = g->start_y + h * g->line_size;
            g->cur_x   = g->start_x;
            g->cur_y   = g->start_y;

            g->lflags = gif_get8(s);

            if (g->lflags & 0x40) {
                g->step = 8 * g->line_size; /* first interlaced spacing */
                g->parse = 3;
            } else {
                g->step = g->line_size;
                g->parse = 0;
            }

            if (g->lflags & 0x80) {
                gif_parse_colortable(s,
                                     g->lpal,
                                     2 << (g->lflags & 7));
                g->color_table = (unsigned char *) g->lpal;
            } else if (g->flags & 0x80) {
                if (g->transparent >= 0 && (g->eflags & 0x01)) {
                   if (bgcolor) {
                       g->pal[g->transparent][0] = bgcolor[2];
                       g->pal[g->transparent][1] = bgcolor[1];
                       g->pal[g->transparent][2] = bgcolor[0];
                   }
                }
                g->color_table = (unsigned char *)g->pal;
            } else {
                sixel_helper_set_additional_message(
                    "corrupt GIF (reason: missing color table).");
                status = SIXEL_RUNTIME_ERROR;
                goto end;
            }

            status = gif_process_raster(s, g);
            if (SIXEL_FAILED(status)) {
                goto end;
            }
            goto end;

        case 0x21: /* Comment Extension. */
            switch (gif_get8(s)) {
            case 0x01: /* Plain Text Extension */
                break;
            case 0x21: /* Comment Extension */
                break;
            case 0xF9: /* Graphic Control Extension */
                len = gif_get8(s); /* block size */
                if (len == 4) {
                    g->eflags = gif_get8(s);
                    g->delay = gif_get16le(s); /* delay */
                    g->transparent = gif_get8(s);
                } else {
                    s->img_buffer += len;
                    break;
                }
                break;
            case 0xFF: /* Application Extension */
                len = gif_get8(s); /* block size */
                if (s->img_buffer + len > s->img_buffer_end) {
                    status = SIXEL_RUNTIME_ERROR;
                    goto end;
                }
                memcpy(buffer, s->img_buffer, len);
                s->img_buffer += len;
                buffer[len] = 0;
                if (len == 11 && strcmp((char *)buffer, "NETSCAPE2.0") == 0) {
                    if (gif_get8(s) == 0x03) {
                        /* loop count */
                        switch (gif_get8(s)) {
                        case 0x00:
                            g->loop_count = 1;
                            break;
                        case 0x01:
                            g->loop_count = gif_get16le(s);
                            break;
                        }
                    }
                }
                break;
            default:
                break;
            }
            while ((len = gif_get8(s)) != 0) {
                s->img_buffer += len;
            }
            break;

        case 0x3B: /* gif stream termination code */
            g->is_terminated = 1;
            status = SIXEL_OK;
            goto end;

        default:
            sixel_helper_set_additional_message(
                "corrupt GIF (reason: unknown code).");
            status = SIXEL_RUNTIME_ERROR;
            goto end;
        }
    }

    status = SIXEL_OK;

end:
    return status;
}


SIXELSTATUS
load_gif(
    unsigned char       /* in */ *buffer,
    int                 /* in */ size,
    unsigned char       /* in */ *bgcolor,
    int                 /* in */ reqcolors,
    int                 /* in */ fuse_palette,
    int                 /* in */ fstatic,
    int                 /* in */ loop_control,
    void                /* in */ *fn_load,     /* callback */
    void                /* in */ *context,     /* private data for callback */
    sixel_allocator_t   /* in */ *allocator)   /* allocator object */
{
    gif_context_t s;
    gif_t g;
    SIXELSTATUS status = SIXEL_FALSE;
    sixel_frame_t *frame;

    g.out = NULL;

    status = sixel_frame_new(&frame, allocator);
    if (SIXEL_FAILED(status)) {
        goto end;
    }
    s.img_buffer = s.img_buffer_original = (unsigned char *)buffer;
    s.img_buffer_end = (unsigned char *)buffer + size;
    memset(&g, 0, sizeof(g));
    status = gif_load_header(&s, &g);
    if (status != SIXEL_OK) {
        goto end;
    }
    frame->width = g.w,
    frame->height = g.h,
    g.out = (unsigned char *)sixel_allocator_malloc(allocator, g.w * g.h);
    if (g.out == NULL) {
        sixel_helper_set_additional_message(
            "load_gif: sixel_allocator_malloc() failed.");
        status = SIXEL_BAD_ALLOCATION;
        goto end;
    }

    frame->loop_count = 0;

    for (;;) { /* per loop */

        frame->frame_no = 0;

        s.img_buffer = s.img_buffer_original;
        status = gif_load_header(&s, &g);
        if (status != SIXEL_OK) {
            goto end;
        }

        g.is_terminated = 0;

        for (;;) { /* per frame */
            status = gif_load_next(&s, &g, bgcolor);
            if (status != SIXEL_OK) {
                goto end;
            }
            if (g.is_terminated) {
                break;
            }

            status = gif_init_frame(frame, &g, bgcolor, reqcolors, fuse_palette);
            if (status != SIXEL_OK) {
                goto end;
            }

            status = ((sixel_load_image_function)fn_load)(frame, context);
            if (status != SIXEL_OK) {
                goto end;
            }

            if (fstatic) {
                break;
            }
            ++frame->frame_no;
        }

        ++frame->loop_count;

        if (g.loop_count < 0) {
            break;
        }
        if (loop_control == SIXEL_LOOP_DISABLE || frame->frame_no == 1) {
            break;
        }
        if (loop_control == SIXEL_LOOP_AUTO) {
            if (frame->loop_count == g.loop_count) {
                break;
            }
        }
    }

end:
    sixel_frame_unref(frame);
    sixel_allocator_free(frame->allocator, g.out);

    return status;
}


#if HAVE_TESTS
static int
test1(void)
{
    int nret = EXIT_FAILURE;

    nret = EXIT_SUCCESS;

    return nret;
}


int
sixel_fromgif_tests_main(void)
{
    int nret = EXIT_FAILURE;
    size_t i;
    typedef int (* testcase)(void);

    static testcase const testcases[] = {
        test1,
    };

    for (i = 0; i < sizeof(testcases) / sizeof(testcase); ++i) {
        nret = testcases[i]();
        if (nret != EXIT_SUCCESS) {
            goto error;
        }
    }

    nret = EXIT_SUCCESS;

error:
    return nret;
}
#endif  /* HAVE_TESTS */


/* emacs, -*- Mode: C; tab-width: 4; indent-tabs-mode: nil -*- */
/* vim: set expandtab ts=4 : */
/* EOF */