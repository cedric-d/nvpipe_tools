#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <endian.h>
#include <sys/uio.h>
#include <pthread.h>

#include <NvPipe.h>

#include "common.h"


//! Number of buffers to transfer frames between threads.
#define BUFFER_COUNT 8

//! Interval (in secs) between IDR frames.
#define IDR_INTERVAL 2


typedef struct {
	void *rawdata;
	void *encdata;
	uint64_t index;
	struct timespec cap_ts;
	struct timespec enc_ts;
	int32_t rawsize; // -1 means unused, 0 means EOF; protected by the mutex
	int32_t encsize; // -1 means unused, 0 means EOF; protected by the mutex
} S_Buffer;

typedef struct {
	// command line parameters
	NvPipe_Codec codec;
	NvPipe_Format format;
	int width;
	int height;
	int framerate;
	int32_t bitrate;
	bool raw_output;

	// internal members
	uint32_t raw_size;
	uint32_t pitch_size;
	NvPipe *encoder;
	volatile bool end; // termination request for input and main threads

	S_Buffer buffers[BUFFER_COUNT];
	pthread_mutex_t mutex;
	pthread_cond_t cond_free;
	pthread_cond_t cond_read;
	pthread_cond_t cond_enc;
	pthread_t thread_input;
	pthread_t thread_output;
} S_Params;

enum {
	ARG_CODEC = 1,
	ARG_WIDTH,
	ARG_HEIGHT,
	ARG_FRAMERATE,
	ARG_BITRATE,

	ARG_EXTRA
};

static bool parse_args(int argc, char *argv[], S_Params *params)
{
	if (argc < 6) {
		fprintf(stderr, "Usage: %s h264|h265 WIDTH HEIGHT FRAMERATE BITRATE|lossless [raw]\n", argv[0]);
		return false;
	}

	// parse output codec
	if (!strcasecmp(argv[ARG_CODEC], "h264") || !strcasecmp(argv[ARG_CODEC], "avc"))
		params->codec = NVPIPE_H264;
	else if (!strcasecmp(argv[ARG_CODEC], "h265") || !strcasecmp(argv[ARG_CODEC], "hevc"))
		params->codec = NVPIPE_HEVC;
	else {
		fprintf(stderr, "Bad codec specified\n");
		return false;
	}

	// parse input video parameters
	params->width = atoi(argv[ARG_WIDTH]);
	params->height = atoi(argv[ARG_HEIGHT]);
	params->framerate = atoi(argv[ARG_FRAMERATE]);
	if (params->width <= 0 || params->height <= 0 || params->framerate <= 0) {
		fprintf(stderr, "Bad input arguments specified\n");
		return false;
	}

	// parse output data stream parameters
	if (!strcasecmp(argv[ARG_BITRATE], "lossless"))
		params->bitrate = INT32_MAX;
	else
		params->bitrate = atol(argv[ARG_BITRATE]);

	if (params->bitrate <= 0) {
		fprintf(stderr, "Bad output bitrate specified\n");
		return false;
	}

	// TODO: support other formats
	params->format = NVPIPE_BGRA32;

	// compute raw input image size
	params->raw_size = (uint32_t)params->width * (uint32_t)params->height;
	params->pitch_size = params->width;
	switch (params->format) {
	case NVPIPE_BGRA32:
		params->raw_size *= 4;
		params->pitch_size *= 4;
		break;
	default:
		break;
	}

	// parse extra parameters
	params->raw_output = false;

	for (int i = ARG_EXTRA; i < argc; ++i) {
		if (!strcasecmp(argv[i], "raw"))
			params->raw_output = true;
	}

	return true;
}


static void *func_input(void *arg)
{
	S_Params *params = (S_Params*)arg;
	uint64_t counter = 0;

	for (int cur = 0; !params->end; cur = (cur + 1) % BUFFER_COUNT) {
		// ensure current buffer is free
		pthread_mutex_lock(&params->mutex);
		while (params->buffers[cur].rawsize != -1 && !params->end)
			pthread_cond_wait(&params->cond_free, &params->mutex);
		pthread_mutex_unlock(&params->mutex);

		// check for termination request
		if (params->end)
			break;

		// read next buffer
		params->buffers[cur].index = counter;
		uint32_t total = 0;
		do {
			ssize_t len = read(STDIN_FILENO,
			                   params->buffers[cur].rawdata + total,
			                   params->raw_size - total);
			if (len <= 0) {
				if (len == -1) {
					if (errno == EINTR)
						continue;
					perror("Failed to read raw frame");
				}
				break;
			}

			total += len;
		} while (total < params->raw_size && !params->end);

		// get read timestamp
		clock_gettime(CLOCK_REALTIME, &params->buffers[cur].cap_ts);
		memset(&params->buffers[cur].enc_ts, 0, sizeof(params->buffers[cur].enc_ts));

		// check for termination request
		if (params->end)
			break;

		// check for EOF (partial frame are not possible)
		if (total != params->raw_size)
			total = 0;

		// send buffer to the main thread for encoding
		pthread_mutex_lock(&params->mutex);
		params->buffers[cur].rawsize = total;
		pthread_cond_signal(&params->cond_read);
		pthread_mutex_unlock(&params->mutex);

		// stop the loop on EOF
		if (total == 0)
			break;

		++counter;
	}

	return NULL;
}

static void *func_output(void *arg)
{
	S_Params *params = (S_Params*)arg;
	S_Header header;
	struct iovec iov[2];
	int iovcnt;
	struct timespec sent_ts, prev_ts;

	// prefill fixed parts of the header
	header.codec = htobe32(params->codec);
	header.format = htobe32(params->format);
	header.width = htobe16(params->width);
	header.height = htobe16(params->height);
	header.framerate = htobe16(params->framerate);

	// initialize the previous timestamp not to have big first interval
	clock_gettime(CLOCK_REALTIME, &prev_ts);

	// loop until EOF
	for (int cur = 0; ; cur = (cur + 1) % BUFFER_COUNT) {
		// wait for next frame
		pthread_mutex_lock(&params->mutex);
		while (params->buffers[cur].encsize == -1)
			pthread_cond_wait(&params->cond_enc, &params->mutex);
		pthread_mutex_unlock(&params->mutex);

		// stop on EOF
		if (params->buffers[cur].encsize == 0)
			break;

		const uint64_t capture_ts = params->buffers[cur].cap_ts.tv_sec * UINT64_C(1000000000)
		                            + params->buffers[cur].cap_ts.tv_nsec;
		const uint64_t encode_ts = params->buffers[cur].enc_ts.tv_sec * UINT64_C(1000000000)
		                           + params->buffers[cur].enc_ts.tv_nsec;

		// fill the variable parts of the header if needed
		header.index = htobe64(params->buffers[cur].index);
		header.capture_ts = htobe64(capture_ts);
		header.encode_ts = htobe64(encode_ts);
		header.size = htobe32(params->buffers[cur].encsize);

		// write the frame
		uint32_t total = 0;
		do {
			if (params->raw_output) {
				iov[0].iov_base = params->buffers[cur].encdata + total;
				iov[0].iov_len = params->buffers[cur].encsize - total;
				iovcnt = 1;
			} else {
				if (total < sizeof(header)) {
					iov[0].iov_base = (void *)&header + total;
					iov[0].iov_len = sizeof(header) - total;
					iov[1].iov_base = params->buffers[cur].encdata;
					iov[1].iov_len = params->buffers[cur].encsize;
					iovcnt = 2;
				} else {
					iov[0].iov_base = params->buffers[cur].encdata + (total - sizeof(header));
					iov[0].iov_len = params->buffers[cur].encsize - (total - sizeof(header));
					iovcnt = 1;
				}
			}

			const ssize_t len = writev(STDOUT_FILENO, iov, iovcnt);
			if (len == -1) {
				if (errno == EINTR)
					continue;
				perror("Failed to write encoded frame");
				params->end = true;
				goto end;
			}
			total += len;
		} while (total < ((params->raw_output ? 0 : sizeof(header)) + params->buffers[cur].encsize));

		clock_gettime(CLOCK_REALTIME, &sent_ts);

		// print statistics
		fprintf(stderr, "Frame %" PRIu64 ": "
		                "interval=%" PRIu32 "ms, "
		                "encode duration=%" PRIu32 "ms, "
		                "size=%" PRIu32 "\n",
		        params->buffers[cur].index,
		        (uint32_t)(((sent_ts.tv_sec * UINT64_C(1000000000) + sent_ts.tv_nsec)
		                   - (prev_ts.tv_sec * UINT64_C(1000000000) + prev_ts.tv_nsec)) / 1000000),
		        (uint32_t)((encode_ts - capture_ts) / 1000000),
		        params->buffers[cur].encsize);

		// release the buffer
		pthread_mutex_lock(&params->mutex);
		params->buffers[cur].rawsize = -1;
		params->buffers[cur].encsize = -1;
		pthread_cond_signal(&params->cond_free);
		pthread_mutex_unlock(&params->mutex);

		prev_ts = sent_ts;
	}

end:
	return NULL;
}

int main(int argc, char *argv[])
{
	S_Params params = {
		.encoder = NULL,
		.end = false,
		.mutex = PTHREAD_MUTEX_INITIALIZER,
		.cond_free = PTHREAD_COND_INITIALIZER,
		.cond_read = PTHREAD_COND_INITIALIZER,
		.cond_enc = PTHREAD_COND_INITIALIZER,
	};
	int res;
	int retval = EXIT_FAILURE;

	// parse arguments
	if (!parse_args(argc, argv, &params))
		goto error;

	// prepare the encoder
	params.encoder = NvPipe_CreateEncoder(params.format,
	                                      params.codec,
	                                      (params.bitrate < INT32_MAX ? NVPIPE_LOSSY : NVPIPE_LOSSLESS),
	                                      params.bitrate,
	                                      params.framerate);
	if (!params.encoder) {
		fprintf(stderr, "Failed to create encoder: %s\n", NvPipe_GetError(NULL));
		goto error;
	}

	// allocate buffers
	for (int i = 0; i < BUFFER_COUNT; ++i) {
		params.buffers[i].rawsize = -1;
		params.buffers[i].encsize = -1;
		params.buffers[i].rawdata = malloc(params.raw_size);
		params.buffers[i].encdata = malloc(params.raw_size);
		if (!params.buffers[i].rawdata || !params.buffers[i].encdata) {
			fprintf(stderr, "Failed to allocate buffers\n");
			goto error;
		}
	}

	// create input and output threads
	res = pthread_create(&params.thread_input, NULL, &func_input, &params);
	if (res != 0) {
		fprintf(stderr, "Failed to create input thread: %s\n", strerror(res));
		goto error;
	}
	res = pthread_create(&params.thread_output, NULL, &func_output, &params);
	if (res != 0) {
		fprintf(stderr, "Failed to create output thread: %s\n", strerror(res));
		goto error;
	}

	// main loop
	for (int cur = 0; !params.end; cur = (cur + 1) % BUFFER_COUNT) {
		// wait for next frame
		pthread_mutex_lock(&params.mutex);
		while (params.buffers[cur].rawsize == -1 && !params.end)
			pthread_cond_wait(&params.cond_read, &params.mutex);
		pthread_mutex_unlock(&params.mutex);

		int32_t encsize = 0;
		// check for EOF
		if (params.buffers[cur].rawsize > 0) {
			encsize = NvPipe_Encode(params.encoder,
			                        params.buffers[cur].rawdata,
			                        params.pitch_size,
			                        params.buffers[cur].encdata,
			                        params.raw_size,
			                        params.width,
			                        params.height,
			                        (params.buffers[cur].index % (IDR_INTERVAL * params.framerate) == 0));
			if (encsize == 0) {
				fprintf(stderr,
				        "Failed to encode frame %" PRIu64 ": %s\n",
				        params.buffers[cur].index,
				        NvPipe_GetError(params.encoder));
			}

			clock_gettime(CLOCK_REALTIME, &params.buffers[cur].enc_ts);
		}

		if (encsize == 0)
			params.end = true;

		// send the frame to the output thread
		pthread_mutex_lock(&params.mutex);
		params.buffers[cur].encsize = encsize;
		pthread_cond_signal(&params.cond_enc);
		pthread_mutex_unlock(&params.mutex);
	}

	pthread_cond_signal(&params.cond_free);
	pthread_cond_signal(&params.cond_enc);
	pthread_join(params.thread_input, NULL);
	pthread_join(params.thread_output, NULL);

	retval = EXIT_SUCCESS;

error:
	// clean up
	if (params.encoder)
		NvPipe_Destroy(params.encoder);
	for (int i = 0; i < BUFFER_COUNT; ++i) {
		free(params.buffers[i].rawdata);
		free(params.buffers[i].encdata);
	}

	return retval;
}
