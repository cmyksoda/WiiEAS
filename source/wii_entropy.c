/**
 * mbedtls_hardware_poll for the Wii Broadway timebase.
 * Required when mbedTLS is built with MBEDTLS_ENTROPY_HARDWARE_ALT
 * (as the WiiFin-configured portlibs libs are).
 *
 * Pattern taken from the VectrexWii / WiiFin work — spin on gettime() so the
 * LSBs actually vary between samples.
 */
#include <mbedtls/build_info.h>

#if defined(MBEDTLS_ENTROPY_HARDWARE_ALT)

#include <stddef.h>
#include <stdint.h>
#include <ogc/lwp_watchdog.h>

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
	(void)data;
	*olen = 0;
	size_t i = 0;
	uint64_t prev = gettime();

	while (i < len) {
		uint64_t t;
		do {
			t = gettime();
		} while ((t - prev) < 128);
		uint64_t delta = t - prev;
		prev = t;
		for (size_t j = 0; j < sizeof(t) && i < len; j++, i++)
			output[i] = ((const uint8_t *)&t)[j] ^ ((const uint8_t *)&delta)[j];
	}
	*olen = len;
	return 0;
}

#endif
