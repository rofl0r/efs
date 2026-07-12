-include config.mak

all: efsbuilder efsreader

efsbuilder.o: efsbuilder.c efs.h
efsreader.o: efsreader.c efs.h

efsbuilder: efsbuilder.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

efsreader: efsreader.o
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@
