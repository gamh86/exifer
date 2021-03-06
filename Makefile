DEBUG:=0
BUILD:=0.3.0
CC=gcc
WFLAGS=-Wall -Werror
CFILES=main.c exif.c file.c logging.c
OFILES=main.o exif.o file.o logging.o
BINARY=exifer

.PHONY: clean

$(BINARY): $(OFILES)
	$(CC) -o $(BINARY) $(OFILES)

$(OFILES): $(CFILES)
ifeq ($(DEBUG),1)
	@echo Debug build $(BUILD)
	$(CC) $(WFLAGS) -DDEBUG -c $(CFILES)
else
	@echo Production build $(BUILD)
	$(CC) $(WFLAGS) -c $(CFILES)
endif

clean:
	rm *.o
