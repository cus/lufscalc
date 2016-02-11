# use pkg-config for getting CFLAGS abd LDFLAGS
FFMPEG_LIBS=libavdevice libavformat libavfilter libavcodec libswscale libavutil libswresample
CFLAGS+=-Wall $(shell pkg-config  --cflags $(FFMPEG_LIBS)) -O3 -I bs1770 -DPLANAR -Df64
LDFLAGS+=$(shell pkg-config --libs $(FFMPEG_LIBS)) -lm
BS1770OBJS=bs1770/biquad.o bs1770/bs1770_a85.o bs1770/bs1770_add_samples.o bs1770/bs1770_aggr.o bs1770/bs1770.o bs1770/bs1770_ctx_add_samples.o bs1770/bs1770_ctx.o bs1770/bs1770_default.o bs1770/bs1770_hist.o bs1770/bs1770_nd_add_samples.o bs1770/bs1770_nd.o bs1770/bs1770_r128.o bs1770/bs1770_stats.o bs1770/bs1770_add_sample.o

EXAMPLES=lufscalc

OBJS=$(addsuffix .o,$(EXAMPLES))

%: %.o bs1770
	$(CC) $< $(LDFLAGS) -o $@ $(BS1770OBJS)

bs1770/bs1770_add_sample.o: CFLAGS+=-UPLANAR

%.o: %.c
	$(CC) $< $(CFLAGS) -c -o $@

.phony: all clean

all: $(OBJS) $(EXAMPLES)

bs1770: $(BS1770OBJS)

clean:
	rm -rf $(EXAMPLES) $(OBJS)
	rm -rf $(BS1770OBJS)
