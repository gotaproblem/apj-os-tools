/* PSWEB: the web browser's engine link (platforms/atari/web/psweb_client.c).
 * The engine is a separate process; every call here pushes a record into a
 * ring, reads a few words the connector thread keeps current, or copies
 * pixels out of shared memory. Nothing waits. Safe under the JIT invariant. */
/* A GEMDOS path handed to LOAD (TeraDesk opening an .HTM with WEBGEM, or
 * one typed into the address field) can only be read by the Pi when it is
 * on a HOSTFS drive: map it with the shared GEMDOS->host translation and
 * hand WebKit a file:// URL. Anything else answers PSWEB_ERR and WEBGEM
 * says so. */
static bool psweb_gemdos_to_file_url(const char *gem, char *out, size_t outsz)
{
  char host[512];
  static const char hex[] = "0123456789ABCDEF";
  size_t n = 0;

  if (!mp3_gemdos_to_host(gem, host, sizeof host))
    return false;
  if (outsz < 8)
    return false;
  memcpy(out, "file://", 7);
  n = 7;
  for (const unsigned char *p = (const unsigned char *)host; *p && n + 4 < outsz; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
        (*p >= '0' && *p <= '9') || strchr("/-._~", *p)) {
      out[n++] = (char)*p;
    } else {
      out[n++] = '%';
      out[n++] = hex[*p >> 4];
      out[n++] = hex[*p & 15];
    }
  }
  out[n] = 0;
  return true;
}

static uae_u32 nf_call_psweb(uae_u32 subid, uaecptr params)
{
  char str[PSWEB_STR_MAX];

  switch (subid) {
    case PSWEB_VERSION:
      return PSWEB_API_VERSION;

    case PSWEB_STATUS:
      return (uae_u32)psweb_status();

    case PSWEB_VIEW_NEW:
      return (uae_u32)(psweb_cmd(PSWEB_CMD_VIEW_NEW,
                                 (int32_t)nf_get_param(params, 0),
                                 (int32_t)nf_get_param(params, 1),
                                 (int32_t)nf_get_param(params, 2),
                                 (int32_t)nf_get_param(params, 3), 0, 0, NULL) == PSWEB_OK ? 1 : PSWEB_NOTCONN);

    case PSWEB_VIEW_FREE:
      return (uae_u32)psweb_cmd(PSWEB_CMD_VIEW_FREE, 0, 0, 0, 0, 0, 0, NULL);

    case PSWEB_VIEW_SIZE:
      return (uae_u32)psweb_cmd(PSWEB_CMD_VIEW_SIZE,
                                (int32_t)nf_get_param(params, 1),
                                (int32_t)nf_get_param(params, 2), 0, 0, 0, 0, NULL);

    case PSWEB_VIEW_STATE:
      return (uae_u32)psweb_cmd(PSWEB_CMD_VIEW_STATE,
                                (int32_t)nf_get_param(params, 1), 0, 0, 0, 0, 0, NULL);

    case PSWEB_LOAD:
      nf_read_string(nf_get_param(params, 1), str, sizeof(str));
      if (((str[0] >= 'A' && str[0] <= 'Z') || (str[0] >= 'a' && str[0] <= 'z')) &&
          str[1] == ':' && (str[2] == '\\' || str[2] == '/')) {
        char url[PSWEB_STR_MAX];
        if (!psweb_gemdos_to_file_url(str, url, sizeof url))
          return (uae_u32)PSWEB_ERR;          /* not on a HOSTFS drive */
        return (uae_u32)psweb_cmd(PSWEB_CMD_LOAD, 0, 0, 0, 0, 0, 0, url);
      }
      return (uae_u32)psweb_cmd(PSWEB_CMD_LOAD, 0, 0, 0, 0, 0, 0, str);

    case PSWEB_NAV:
      return (uae_u32)psweb_cmd(PSWEB_CMD_NAV,
                                (int32_t)nf_get_param(params, 1), 0, 0, 0, 0, 0, NULL);

    case PSWEB_POLL: {
      uaecptr out = nf_get_param(params, 1);
      struct psweb_pollstate st;
      int rc = psweb_poll(&st);
      if (out) {
        nf_write_long(out + 0,  st.frame_serial);
        nf_write_long(out + 4,  st.n_damage);
        nf_write_long(out + 8,  st.progress);
        nf_write_long(out + 12, st.flags);
        nf_write_long(out + 16, st.cursor);
        nf_write_long(out + 20, st.title_serial);
        nf_write_long(out + 24, st.uri_serial);
        nf_write_long(out + 28, st.dialog);
      }
      return (uae_u32)rc;
    }

    case PSWEB_FETCH: {
      uaecptr dest = nf_get_param(params, 1);
      uae_u32 stride = nf_get_param(params, 2);
      uae_u32 rows = nf_get_param(params, 3);
      uaecptr rectp = nf_get_param(params, 4);
      int max = (int)nf_get_param(params, 5);
      uae_u8 *p;
      int32_t rects[PSWEB_DAMAGE_MAX * 4];

      if (!dest || !stride || !rows)
        return (uae_u32)PSWEB_ERR;
      if (!nf_host_ram_ptr(dest, stride * rows, &p)) {
        printf("[PSWEB] fetch buffer at %08x is not host RAM "
               "(use Mxalloc(size, 1) for TT-RAM)\n", (unsigned)dest);
        return (uae_u32)PSWEB_ERR;
      }
      if (max > PSWEB_DAMAGE_MAX) max = PSWEB_DAMAGE_MAX;
      int n = psweb_fetch(p, stride, rows, rects, max);
      if (n > 0 && rectp) {
        int lim = n < max ? n : max;
        for (int i = 0; i < lim * 4; i++)
          nf_write_long(rectp + (uaecptr)i * 4, (uae_u32)rects[i]);
      }
      return (uae_u32)n;
    }

    case PSWEB_POINTER:
      return (uae_u32)psweb_cmd(PSWEB_CMD_POINTER,
                                (int32_t)nf_get_param(params, 1),
                                (int32_t)nf_get_param(params, 2),
                                (int32_t)nf_get_param(params, 3),
                                (int32_t)nf_get_param(params, 4),
                                (int32_t)nf_get_param(params, 5), 0, NULL);

    case PSWEB_SCROLL:
      return (uae_u32)psweb_cmd(PSWEB_CMD_SCROLL,
                                (int32_t)nf_get_param(params, 1),
                                (int32_t)nf_get_param(params, 2),
                                (int32_t)nf_get_param(params, 3),
                                (int32_t)nf_get_param(params, 4),
                                (int32_t)nf_get_param(params, 5), 0, NULL);

    case PSWEB_KEY:
      return (uae_u32)psweb_cmd(PSWEB_CMD_KEY,
                                (int32_t)nf_get_param(params, 1),
                                (int32_t)nf_get_param(params, 2),
                                (int32_t)nf_get_param(params, 3), 0, 0, 0, NULL);

    case PSWEB_TEXT:
      nf_read_string(nf_get_param(params, 1), str, sizeof(str));
      return (uae_u32)psweb_cmd(PSWEB_CMD_TEXT, 0, 0, 0, 0, 0, 0, str);

    case PSWEB_GETSTR: {
      int which = (int)nf_get_param(params, 1);
      uaecptr buf = nf_get_param(params, 2);
      int len = (int)nf_get_param(params, 3);
      if (!buf || len <= 0)
        return (uae_u32)PSWEB_ERR;
      if (len > (int)sizeof(str)) len = (int)sizeof(str);
      int n = psweb_getstr(which, str, len);
      if (n < 0)
        return (uae_u32)n;
      nf_write_string(buf, (uae_u32)len, str);
      return (uae_u32)n;
    }

    case PSWEB_ZOOM:
      return (uae_u32)psweb_cmd(PSWEB_CMD_ZOOM,
                                (int32_t)nf_get_param(params, 1), 0, 0, 0, 0, 0, NULL);

    case PSWEB_SETTING: {
      uaecptr sp = nf_get_param(params, 3);
      str[0] = 0;
      if (sp)
        nf_read_string(sp, str, sizeof(str));
      return (uae_u32)psweb_cmd(PSWEB_CMD_SETTING,
                                (int32_t)nf_get_param(params, 1),
                                (int32_t)nf_get_param(params, 2), 0, 0, 0, 0,
                                str[0] ? str : NULL);
    }

    default:
      return (uae_u32)PSWEB_ERR;
  }
}

