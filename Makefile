BINS := nvpipe_encode nvpipe_decode

CFLAGS := -g -O2 -Wall -std=c99 -pthread
CPPFLAGS := -I/usr/local/nvpipe/include
LDFLAGS := -pthread -lrt -L/usr/local/nvpipe/lib -lNvPipe -Wl,--allow-shlib-undefined

all: $(BINS)

$(BINS): common.h

.PHONY: clean

clean:
	$(RM) $(BINS)
