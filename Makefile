# use pkg-config for getting CFLAGS abd LDFLAGS
FFMPEG_LIBS=libavdevice libavformat libavfilter libavcodec libswscale libavutil
CFLAGS+=-Wall $(shell pkg-config  --cflags $(FFMPEG_LIBS)) -O3 -I bs1770
LDFLAGS+=$(shell pkg-config --libs $(FFMPEG_LIBS))
BS1770OBJS=bs1770/biquad.o bs1770/bs1770.o bs1770/bs1770_ctx.o bs1770/bs1770_stats.o bs1770/bs1770_stats_h.o bs1770/bs1770_stats_s.o

EXAMPLES=lufscalc

OBJS=$(addsuffix .o,$(EXAMPLES))

%: %.o bs1770
	$(CC) $< $(LDFLAGS) -o $@ $(BS1770OBJS)

%.o: %.c
	$(CC) $< $(CFLAGS) -c -o $@

.phony: all clean

all: $(OBJS) $(EXAMPLES)

bs1770: $(BS1770OBJS)

clean:
	rm -rf $(EXAMPLES) $(OBJS)
	rm -rf $(BS1770OBJS)
