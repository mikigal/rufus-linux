/*
 * Rufus: minimal replacements for libcdio's device layer (device.c), which is
 * not built, as Rufus only ever accesses image files and never physical
 * CD/DVD drives through libcdio.
 *
 * udf_open() first tries cdio_open() and falls back to reading the path as a
 * plain stream when it fails, which is exactly what we want for ISO files.
 */

#include <cdio/cdio.h>

CdIo_t *cdio_open(const char *psz_source, driver_id_t driver_id)
{
	(void)psz_source;
	(void)driver_id;
	return NULL;
}

void cdio_destroy(CdIo_t *p_cdio)
{
	(void)p_cdio;
}
