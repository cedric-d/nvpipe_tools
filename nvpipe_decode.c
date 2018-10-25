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
#include <sys/types.h>
#include <sys/socket.h>

#include <NvPipe.h>

#include "common.h"


//! Number of buffers to transfer frames between threads.
#define BUFFER_COUNT 8


typedef struct {
	void *encdata;
	void *rawdata;
	uint64_t index;
	struct timespec cap_ts;
	struct timespec enc_ts;
	struct timespec recv_ts;
	struct timespec dec_ts;
	int32_t encsize; // -1 means unused, 0 means EOF; protected by the mutex
	int32_t rawsize; // -1 means unused, 0 means EOF; protected by the mutex
} S_Buffer;

typedef struct {
	// command line parameters
	NvPipe_Codec codec;
	NvPipe_Format format;
	int width;
	int height;
	int framerate;
	bool auto_decode;

	// internal members
	uint32_t raw_size;
	uint32_t pitch_size;
	NvPipe *decoder;
	volatile bool end; // termination request for input and main threads

	S_Buffer buffers[BUFFER_COUNT];
	pthread_mutex_t mutex;
	pthread_cond_t cond_free;
	pthread_cond_t cond_read;
	pthread_cond_t cond_dec;
	pthread_t thread_input;
	pthread_t thread_output;
} S_Params;

enum {
	ARG_CODEC = 1,
	ARG_WIDTH,
	ARG_HEIGHT,
	ARG_FRAMERATE,

	ARG_EXTRA
};

static bool parse_args(int argc, char *argv[], S_Params *params)
{
	if (argc < 5) {
		fprintf(stderr, "Usage: %s h264|h265 WIDTH HEIGHT FRAMERATE\n", argv[0]);
		return false;
	}

	// parse input codec
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

	return true;
}


static void *func_input(void *arg)
{
	S_Params *params = (S_Params*)arg;
	S_Header header;
	uint64_t counter = 0;
	ssize_t len;
	bool input_is_stream = true;
	int res;
	int optval;
	socklen_t optlen = sizeof(optval);

	// detect input type
	res = getsockopt(STDIN_FILENO, SOL_SOCKET, SO_TYPE, &optval, &optlen);
	if (res == -1) {
		if (errno != ENOTSOCK) {
			perror("Failed to get input type");
			params->end = true;
		}
	} else {
		switch (optval) {
		case SOCK_DGRAM:
		case SOCK_SEQPACKET:
			input_is_stream = false;
			break;
		}
	}

	for (int cur = 0; !params->end; cur = (cur + 1) % BUFFER_COUNT) {
		// ensure current buffer is free
		pthread_mutex_lock(&params->mutex);
		while (params->buffers[cur].encsize != -1 && !params->end)
			pthread_cond_wait(&params->cond_free, &params->mutex);
		pthread_mutex_unlock(&params->mutex);

		// check for termination request
		if (params->end)
			break;

		uint32_t total = 0;

		// read next header for stream-based input
		if (input_is_stream) {
			do {
				len = read(STDIN_FILENO, &header, sizeof(header));
				if (len <= 0) {
					if (len == -1) {
						if (errno == EINTR)
							continue;
						perror("Failed to get frame header");
					}
				}
			} while (len == -1 && errno == EINTR && !params->end);
		// read complete packet for message-based input
		} else {
			do {
				struct iovec iov[2] = {
					{ .iov_base = &header,
					  .iov_len = sizeof(header) },
					{ .iov_base = params->buffers[cur].encdata,
					  .iov_len = params->raw_size }
				};
				struct msghdr msgh = {
					.msg_name = NULL,
					.msg_namelen = 0,
					.msg_iov = iov,
					.msg_iovlen = 2,
					.msg_control = NULL,
					.msg_controllen = 0,
					.msg_flags = 0,
				};

				len = recvmsg(STDIN_FILENO, &msgh, 0);
				if (len <= 0) {
					if (len == -1) {
						if (errno == EINTR)
							continue;
						perror("Failed to read frame header");
					}
					break;
				}
				total = len;
			} while (len <= 0 && !params->end);
		}

		if (len > 0 && !params->end) {
			// extract data from header
			header.codec = be32toh(header.codec);
			header.format = be32toh(header.format);
			header.width = be16toh(header.width);
			header.height = be16toh(header.height);
			header.framerate = be16toh(header.framerate);
			header.index = be64toh(header.index);
			header.capture_ts = be64toh(header.capture_ts);
			header.encode_ts = be64toh(header.encode_ts);
			header.size = be32toh(header.size);

			// check compatibility
			if (header.codec != params->codec ||
			    header.format != params->format ||
			    header.width != params->width ||
			    header.height != params->height ||
			    header.framerate != params->framerate) {
				fprintf(stderr, "Input frame is incompatible with current decoder\n");
				break;
			}

			// read packet for stream-based input
			if (input_is_stream) {
				do {
					ssize_t len = read(STDIN_FILENO,
						           params->buffers[cur].encdata + total,
						           header.size - total);
					if (len <= 0) {
						if (len == -1) {
							if (errno == EINTR)
								continue;
							perror("Failed to read frame header");
						}
						break;
					}

					total += len;
				} while (total < header.size && !params->end);
			}

			// store original timestamps
			params->buffers[cur].cap_ts.tv_sec = header.capture_ts / 1000000000;
			params->buffers[cur].cap_ts.tv_nsec = header.capture_ts % 1000000000;
			params->buffers[cur].enc_ts.tv_sec = header.encode_ts / 1000000000;
			params->buffers[cur].enc_ts.tv_nsec = header.encode_ts % 1000000000;
		}

		// get read timestamp
		clock_gettime(CLOCK_REALTIME, &params->buffers[cur].recv_ts);
		memset(&params->buffers[cur].dec_ts, 0, sizeof(params->buffers[cur].dec_ts));

		// check for termination request
		if (params->end)
			break;

		// check for EOF (partial frame are not possible)
		if (total != params->raw_size)
			total = 0;

		if (header.index < counter) {
			fprintf(stderr, "Old frame received\n");
		} else {
			if (header.index > counter) {
				fprintf(stderr, "%u frames lost\n", (unsigned int)(header.index - counter));
				counter = header.index;
			}

			// send buffer to the main thread for decoding
			pthread_mutex_lock(&params->mutex);
			params->buffers[cur].encsize = total;
			pthread_cond_signal(&params->cond_read);
			pthread_mutex_unlock(&params->mutex);

			++counter;
		}

		// stop the loop on EOF
		if (total == 0)
			break;
	}

	return NULL;
}

static void *func_output(void *arg)
{
	S_Params *params = (S_Params*)arg;
	struct timespec sent_ts, prev_ts;

	// initialize the previous timestamp not to have big first interval
	clock_gettime(CLOCK_REALTIME, &prev_ts);

	// loop until EOF
	for (int cur = 0; ; cur = (cur + 1) % BUFFER_COUNT) {
		// wait for next frame
		pthread_mutex_lock(&params->mutex);
		while (params->buffers[cur].rawsize == -1)
			pthread_cond_wait(&params->cond_dec, &params->mutex);
		pthread_mutex_unlock(&params->mutex);

		// stop on EOF
		if (params->buffers[cur].rawsize == 0)
			break;

		const uint64_t cap_ts = params->buffers[cur].cap_ts.tv_sec * UINT64_C(1000000000)
		                         + params->buffers[cur].cap_ts.tv_nsec;
		const uint64_t enc_ts = params->buffers[cur].enc_ts.tv_sec * UINT64_C(1000000000)
		                         + params->buffers[cur].enc_ts.tv_nsec;
		const uint64_t recv_ts = params->buffers[cur].recv_ts.tv_sec * UINT64_C(1000000000)
		                         + params->buffers[cur].recv_ts.tv_nsec;
		const uint64_t dec_ts = params->buffers[cur].dec_ts.tv_sec * UINT64_C(1000000000)
		                         + params->buffers[cur].dec_ts.tv_nsec;

		// write the frame
		uint32_t total = 0;
		do {
			const ssize_t len = write(STDOUT_FILENO,
			                          params->buffers[cur].rawdata,
			                          params->buffers[cur].rawsize);
			if (len == -1) {
				if (errno == EINTR)
					continue;
				perror("Failed to write decoded frame");
				params->end = true;
				goto end;
			}
			total += len;
		} while (total < params->buffers[cur].rawsize);

		clock_gettime(CLOCK_REALTIME, &sent_ts);

		// print statistics
		fprintf(stderr, "Frame %" PRIu64 ": "
		                "interval=%" PRIu32 "ms, "
		                "encode duration=%" PRIu32 "ms, "
		                "decode duration=%" PRIu32 "ms, "
		                "encoded size=%" PRIu32 "\n",
		        params->buffers[cur].index,
		        (uint32_t)(((sent_ts.tv_sec * UINT64_C(1000000000) + sent_ts.tv_nsec)
		         - (prev_ts.tv_sec * UINT64_C(1000000000) + prev_ts.tv_nsec)) / 1000000),
		        (uint32_t)((enc_ts - cap_ts) / 1000000),
		        (uint32_t)((dec_ts - recv_ts) / 1000000),
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
		.decoder = NULL,
		.end = false,
		.mutex = PTHREAD_MUTEX_INITIALIZER,
		.cond_free = PTHREAD_COND_INITIALIZER,
		.cond_read = PTHREAD_COND_INITIALIZER,
		.cond_dec = PTHREAD_COND_INITIALIZER,
	};
	int res;
	int retval = EXIT_FAILURE;

	// parse arguments
	if (!parse_args(argc, argv, &params))
		goto error;

	// prepare the decoder
	params.decoder = NvPipe_CreateDecoder(params.format, params.codec);
	if (!params.decoder) {
		fprintf(stderr, "Failed to create decoder: %s\n", NvPipe_GetError(NULL));
		goto error;
	}

	// allocate buffers
	for (int i = 0; i < BUFFER_COUNT; ++i) {
		params.buffers[i].encsize = -1;
		params.buffers[i].rawsize = -1;
		params.buffers[i].encdata = malloc(params.raw_size);
		params.buffers[i].rawdata = malloc(params.raw_size);
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
		while (params.buffers[cur].encsize == -1 && !params.end)
			pthread_cond_wait(&params.cond_read, &params.mutex);
		pthread_mutex_unlock(&params.mutex);

		int32_t rawsize = 0;
		// check for EOF
		if (params.buffers[cur].encsize > 0) {
			rawsize = NvPipe_Decode(params.decoder,
			                        params.buffers[cur].encdata,
			                        params.buffers[cur].encsize,
			                        params.buffers[cur].rawdata,
			                        params.width,
			                        params.height);
			if (rawsize == 0) {
				fprintf(stderr,
				        "Failed to decode frame %" PRIu64 ": %s\n",
				        params.buffers[cur].index,
				        NvPipe_GetError(params.decoder));
			}

			clock_gettime(CLOCK_REALTIME, &params.buffers[cur].dec_ts);
		}

		if (rawsize == 0)
			params.end = true;

		// send the frame to the output thread
		pthread_mutex_lock(&params.mutex);
		params.buffers[cur].rawsize = rawsize;
		pthread_cond_signal(&params.cond_dec);
		pthread_mutex_unlock(&params.mutex);
	}

	pthread_cond_signal(&params.cond_free);
	pthread_cond_signal(&params.cond_dec);
	pthread_join(params.thread_input, NULL);
	pthread_join(params.thread_output, NULL);

	retval = EXIT_SUCCESS;

error:
	// clean up
	if (params.decoder)
		NvPipe_Destroy(params.decoder);
	for (int i = 0; i < BUFFER_COUNT; ++i) {
		free(params.buffers[i].rawdata);
		free(params.buffers[i].encdata);
	}

	return retval;
}
