#ifndef EFS_MPH_H
#define EFS_MPH_H

/* efs_mph.h - compile-time selection of the MPH implementation EFS is built
 * against (see UNIVERSAL-API-REVISED.md). Exactly one algorithm is compiled
 * into an image, so there is nothing to store in the image to identify it.
 *
 * Default is jmph.h (Jenkins). Define EFS_MPH_OOMPH to use oomph/mph.h
 * (BBHash) instead -- e.g. `make MPH=BBHASH`.
 *
 * Both headers expose the same unified API shape; this header just maps it
 * to the generic efs_mph_* names the EFS builder/reader use:
 *
 *   struct efs_mph_in  { uint32_t n; const char *const *keys; const uint32_t *kl; }
 *   struct efs_mph_out { uint8_t *data; uint32_t len, blen, shift, salt, w; }
 *   int      efs_mph_build(const struct efs_mph_in *, struct efs_mph_out *);
 *   uint32_t efs_mph_index(tab, blen, shift, salt, w, key, klen);
 *   uint32_t efs_mph_bytes(blen, w);
 */

#if defined(EFS_MPH_OOMPH)

#  include "bbhash.h"
#  define efs_mph_in      mph_in
#  define efs_mph_out     mph_out
#  define efs_mph_build   mph_build_u
#  define efs_mph_index   mph_index_p
#  define efs_mph_bytes   mph_bytes

#else

#  include "jmph.h"
#  define efs_mph_in      jmph_in
#  define efs_mph_out     jmph_out
#  define efs_mph_build   jmph_build
#  define efs_mph_index   jmph_index_p
#  define efs_mph_bytes   jmph_bytes

#endif

#endif /* EFS_MPH_H */
