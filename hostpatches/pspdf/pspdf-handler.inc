/* PSPDF: host-side PDF rendering (platforms/atari/pdf/pspdf.cpp).
 * Every call here is a plain function call into pspdf.c++ - the drawing
 * itself happens on pspdf's worker thread, so nothing below stalls the
 * guest for more than a mutex trylock. Safe under the JIT invariant. */
static uae_u32 nf_call_pspdf(uae_u32 subid, uaecptr params)
{
  char gpath[512];
  char host[PATH_MAX];

  switch (subid) {
    case PSPDF_VERSION:
      return PSPDF_API_VERSION;

    case PSPDF_OPEN: {
      uaecptr pathp = nf_get_param(params, 0);
      if (!pathp)
        return (uae_u32)PSPDF_ERR;
      nf_read_string(pathp, gpath, sizeof(gpath));
      if (!mp3_gemdos_to_host(gpath, host, sizeof(host))) {
        if (gpath[0] == '/')
          snprintf(host, sizeof(host), "%s", gpath);   /* raw host path */
        else
          return (uae_u32)PSPDF_ERR;
      }
      return (uae_u32)pspdf_open(host);
    }

    case PSPDF_OPENMEM: {
      uaecptr src = nf_get_param(params, 0);
      uae_u32 len = nf_get_param(params, 1);
      uae_u8 *p;
      if (!src || !len || !nf_host_ram_ptr(src, len, &p))
        return (uae_u32)PSPDF_ERR;
      return (uae_u32)pspdf_open_mem(p, len);
    }

    case PSPDF_CLOSE:
      pspdf_close((int)nf_get_param(params, 0));
      return 0;

    case PSPDF_INFO:
      return (uae_u32)pspdf_info((int)nf_get_param(params, 0),
                                 (int)nf_get_param(params, 1));

    case PSPDF_PAGESIZE:
      return (uae_u32)pspdf_page_size((int)nf_get_param(params, 0),
                                      (int)nf_get_param(params, 1),
                                      (int)nf_get_param(params, 2));

    case PSPDF_RENDER:
      return (uae_u32)pspdf_render((int)nf_get_param(params, 0),
                                   (int)nf_get_param(params, 1),
                                   (int)nf_get_param(params, 2),
                                   (int)nf_get_param(params, 3));

    case PSPDF_STATUS:
      return (uae_u32)pspdf_status((int)nf_get_param(params, 0));

    case PSPDF_FETCH: {
      int handle = (int)nf_get_param(params, 0);
      uaecptr dest = nf_get_param(params, 1);
      int x   = (int)nf_get_param(params, 2);
      int y   = (int)nf_get_param(params, 3);
      int w   = (int)nf_get_param(params, 4);
      int h   = (int)nf_get_param(params, 5);
      int bpp = (int)nf_get_param(params, 6);
      uae_u32 stride = nf_get_param(params, 7);
      uae_u32 bg = nf_get_param(params, 8);
      uae_u8 *p;

      if (!dest || w <= 0 || h <= 0 || (bpp != 16 && bpp != 32))
        return (uae_u32)PSPDF_ERR;
      if (!stride)
        stride = (uae_u32)w * (uae_u32)(bpp / 8);
      if (stride < (uae_u32)w * (uae_u32)(bpp / 8))
        return (uae_u32)PSPDF_ERR;

      /* The buffer has to be somewhere the host can write directly - in
       * practice TT-RAM, since a write below 4 MB goes through the DMA
       * mirror and the JIT's self-modifying-code check, byte by byte. */
      if (!nf_host_ram_ptr(dest, stride * (uae_u32)h, &p)) {
        printf("[PSPDF] fetch buffer at %08x is not host RAM "
               "(use Mxalloc(size, 1) for TT-RAM)\n", (unsigned)dest);
        return (uae_u32)PSPDF_ERR;
      }

      return (uae_u32)pspdf_fetch(handle, x, y, w, h, bpp, p, stride, bg);
    }

    case PSPDF_FIND: {
      int handle = (int)nf_get_param(params, 0);
      uaecptr needle = nf_get_param(params, 1);
      int page  = (int)nf_get_param(params, 2);
      int flags = (int)nf_get_param(params, 3);
      int zoom  = (int)nf_get_param(params, 4);
      uaecptr out = nf_get_param(params, 5);
      int max     = (int)nf_get_param(params, 6);
      char atari[256], utf8[768];
      int32_t rects[64 * 4];

      if (!needle || max <= 0)
        return (uae_u32)PSPDF_ERR;
      if (max > 64) max = 64;

      nf_read_string(needle, atari, sizeof(atari));
      pspdf_atari_to_utf8(atari, utf8, sizeof(utf8));

      int n = pspdf_find(handle, utf8, page, flags, zoom, rects, max);
      if (n > 0 && out)
        for (int i = 0; i < n * 4; i++)
          nf_write_long(out + (uaecptr)i * 4, (uae_u32)rects[i]);
      return (uae_u32)n;
    }

    case PSPDF_TEXT: {
      int handle = (int)nf_get_param(params, 0);
      int page   = (int)nf_get_param(params, 1);
      int zoom   = (int)nf_get_param(params, 2);
      uaecptr rp = nf_get_param(params, 3);
      uaecptr buf = nf_get_param(params, 4);
      int len     = (int)nf_get_param(params, 5);
      int32_t rect[4];
      char *tmp;
      int n;

      if (!buf || len <= 0)
        return (uae_u32)PSPDF_ERR;
      if (len > 32768) len = 32768;

      if (rp)
        for (int i = 0; i < 4; i++)
          rect[i] = (int32_t)nf_read_long(rp + (uaecptr)i * 4);

      tmp = (char *)malloc((size_t)len);
      if (!tmp)
        return (uae_u32)PSPDF_ERR;

      n = pspdf_text(handle, page, zoom, rp ? rect : NULL, tmp, len);
      if (n >= 0)
        nf_write_string(buf, (uae_u32)len, tmp);
      free(tmp);
      return (uae_u32)n;
    }

    case PSPDF_LINKS: {
      int handle = (int)nf_get_param(params, 0);
      int page   = (int)nf_get_param(params, 1);
      int zoom   = (int)nf_get_param(params, 2);
      uaecptr out = nf_get_param(params, 3);
      int max     = (int)nf_get_param(params, 4);
      int32_t links[32 * 6];

      if (max <= 0) return (uae_u32)PSPDF_ERR;
      if (max > 32) max = 32;

      int n = pspdf_links(handle, page, zoom, links, max);
      if (n > 0 && out)
        for (int i = 0; i < n * 6; i++)
          nf_write_long(out + (uaecptr)i * 4, (uae_u32)links[i]);
      return (uae_u32)n;
    }

    case PSPDF_LINKURI: {
      int handle = (int)nf_get_param(params, 0);
      int page   = (int)nf_get_param(params, 1);
      int index  = (int)nf_get_param(params, 2);
      uaecptr buf = nf_get_param(params, 3);
      int len     = (int)nf_get_param(params, 4);
      char uri[1024];

      if (!buf || len <= 0) return (uae_u32)PSPDF_ERR;
      if (len > (int)sizeof(uri)) len = (int)sizeof(uri);

      int n = pspdf_link_uri(handle, page, index, uri, len);
      if (n >= 0)
        nf_write_string(buf, (uae_u32)len, uri);
      return (uae_u32)n;
    }

    case PSPDF_OUTLINE: {
      int handle = (int)nf_get_param(params, 0);
      int index  = (int)nf_get_param(params, 1);
      uaecptr buf = nf_get_param(params, 2);
      int32_t depth = 0, page = 0;
      char title[PSPDF_TITLE_MAX];

      if (!buf) return (uae_u32)PSPDF_ERR;

      if (pspdf_outline(handle, index, &depth, &page,
                        title, sizeof(title)) != PSPDF_OK)
        return (uae_u32)PSPDF_ERR;

      nf_write_long(buf, (uae_u32)depth);
      nf_write_long(buf + 4, (uae_u32)page);
      nf_write_string(buf + 8, sizeof(title), title);
      return 0;
    }

    case PSPDF_META: {
      int handle = (int)nf_get_param(params, 0);
      int which  = (int)nf_get_param(params, 1);
      uaecptr buf = nf_get_param(params, 2);
      int len     = (int)nf_get_param(params, 3);
      char meta[512];

      if (!buf || len <= 0) return (uae_u32)PSPDF_ERR;
      if (len > (int)sizeof(meta)) len = (int)sizeof(meta);

      int n = pspdf_meta(handle, which, meta, len);
      if (n >= 0)
        nf_write_string(buf, (uae_u32)len, meta);
      return (uae_u32)n;
    }

    case PSPDF_HILITE: {
      int handle = (int)nf_get_param(params, 0);
      int page   = (int)nf_get_param(params, 1);
      uaecptr rp = nf_get_param(params, 2);
      int n      = (int)nf_get_param(params, 3);
      uae_u32 rgb = nf_get_param(params, 4);
      int32_t rects[PSPDF_HILITE_MAX * 4];

      if (n < 0) n = 0;
      if (n > PSPDF_HILITE_MAX) n = PSPDF_HILITE_MAX;
      if (n && !rp) return (uae_u32)PSPDF_ERR;
      for (int i = 0; i < n * 4; i++)
        rects[i] = (int32_t)nf_read_long(rp + (uaecptr)i * 4);
      return (uae_u32)pspdf_hilite(handle, page, rects, n, rgb);
    }

    case PSPDF_PREFETCH:
      return (uae_u32)pspdf_prefetch((int)nf_get_param(params, 0),
                                     (int)nf_get_param(params, 1),
                                     (int)nf_get_param(params, 2),
                                     (int)nf_get_param(params, 3));

    case PSPDF_CONTINUOUS:
      return (uae_u32)pspdf_continuous((int)nf_get_param(params, 0),
                                       (int)nf_get_param(params, 1),
                                       (int)nf_get_param(params, 2));
  }

  return (uae_u32)PSPDF_ERR;
}

