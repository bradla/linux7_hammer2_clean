#include "hammer2.h"

#include <lz4.h>
#include <zlib.h>

#define DEBUFSIZE	HAMMER2_PBUFSIZE

static char OutBuf[DEBUFSIZE + 128];

void *
hammer2_decompress_LZ4(void *inbuf, size_t insize, size_t outsize,
		       int *statusp)
{
	int result;
	int cinsize;

	assert(outsize <= DEBUFSIZE);
	*statusp = 0;

	cinsize = *(const int *)inbuf;
	if (cinsize > (int)insize) {
		printf("FAIL1\n");
		bzero(OutBuf, outsize);
		return OutBuf;
	}

	result = LZ4_decompress_safe((char *)inbuf + 4, OutBuf,
				     cinsize, outsize);
	if (result < 0) {
		bzero(OutBuf, outsize);
	} else {
		*statusp = 1;
		if (result < (int)outsize)
			bzero(OutBuf + result, outsize - result);
	}
	if (result < 0)
		printf("LZ4 decompression failure\n");

	return OutBuf;
}

void *
hammer2_decompress_ZLIB(void *inbuf, size_t insize, size_t outsize,
			int *statusp)
{
	z_stream strm_decompress;
	int ret;

	assert(outsize <= DEBUFSIZE);
	*statusp = 0;

	bzero(&strm_decompress, sizeof(strm_decompress));
	strm_decompress.avail_in = 0;
	strm_decompress.next_in = Z_NULL;
	ret = inflateInit(&strm_decompress);

	if (ret != Z_OK) {
		bzero(OutBuf, outsize);
		printf("ZLIB1 decompression failure\n");
	} else {
		strm_decompress.next_in = inbuf;
		strm_decompress.next_out = OutBuf;
		strm_decompress.avail_in = insize;
		strm_decompress.avail_out = outsize;
		ret = inflate(&strm_decompress, Z_FINISH);
		if (ret != Z_STREAM_END) {
			bzero(OutBuf, outsize);
			printf("ZLIB2 decompression failure\n");
		} else {
			ret = outsize - strm_decompress.avail_out;
			if (ret < (int)outsize)
				bzero(OutBuf + ret, strm_decompress.avail_out);
			*statusp = 1;
		}
		ret = inflateEnd(&strm_decompress);
	}
	return OutBuf;
}
