TARGETS=lab4

CFLAGS=-Wall -g -O0

all: $(TARGETS)

lab4: lab4.c
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

clean:
	$(RM) $(TARGETS)

