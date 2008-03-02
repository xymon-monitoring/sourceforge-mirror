#include <string.h>
#include <stdio.h>
#include <zlib.h>

int main(int argc, char **argv)
{
	z_stream strm;
	int n;

	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	n = inflateInit(&strm);
	if (n == Z_VERSION_ERROR) {
		printf("Version mismatch: Compiled with %s, runtime has %s\n",
			ZLIB_VERSION, zlibVersion());
	}
	else {
		printf("zlib version %s\n", zlibVersion());
	}

	return 0;
}

