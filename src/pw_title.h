/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_TITLE_H
#define PW_TITLE_H
#include <stddef.h>
#include <stdint.h>

/* Which title the runtime is running in, decided when it starts rather than
 * when it is built, so one released runtime serves every title that ships it
 * (orbis-win's GLDS10043, Playden's PLDN10001, ...).
 *
 * The title ID comes from the package's own /app0/sce_sys/param.sfo. The data
 * folder is /data/<TITLE_ID> when that folder exists, else the build's default
 * (/data/prospero-win); orbis-win-ps4's GPU driver (pw_vulkan_radv_ps4.c)
 * applies the same rule, so the runtime and the driver read one settings.conf.
 */

enum { PW_TITLE_ID_CAPACITY = 16, PW_TITLE_ROOT_CAPACITY = 64 };

/* TITLE_ID from a param.sfo image: 0 and out filled (upper-case letters and
 * digits only, e.g. "GLDS10043"), or -1 when the image is not a PSF, has no
 * TITLE_ID, or it is malformed. */
int pw_title_id_from_sfo(const uint8_t *sfo, size_t length, char out[PW_TITLE_ID_CAPACITY]);

/* The data folder for title: "/data/<title>" when is_directory says so, else
 * fallback. title may be NULL or "" (then fallback). out is NUL-terminated;
 * returns 0, or -1 when it does not fit (out then holds fallback, cut). */
int pw_title_data_root(const char *title, const char *fallback, int (*is_directory)(const char *path),
                       char *out, size_t capacity);

#endif
