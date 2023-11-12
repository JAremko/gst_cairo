CC=gcc
CFLAGS=$(shell pkg-config --cflags gstreamer-1.0 gstreamer-video-1.0 cairo)
LIBS=$(shell pkg-config --libs gstreamer-1.0 gstreamer-video-1.0 cairo)

all: app buff

app: app_main.c
	$(CC) $(CFLAGS) -o app app_main.c $(LIBS)


buff: buff_main.c
	$(CC) $(CFLAGS) -o buff buff_main.c $(LIBS)

clean:
	rm -f app buff
