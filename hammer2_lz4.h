#pragma once

#if defined (__cplusplus)
extern "C" {
#endif

int LZ4_decompress_safe (char* source, char* dest, int inputSize,
						int maxOutputSize);

int LZ4_compress_limitedOutput(char* source, char* dest, int inputSize,
						int maxOutputSize);

#if defined (__cplusplus)
}
#endif
