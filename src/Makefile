CC = gcc
SRCS = iolib.c banner.c workload.c
CFLAGS = -Wall -Wextra
RESULT = run

all : $(RESULT)

$(RESULT): $(SRCS)
	$(CC) $(SRCS) -o $@

clean:
	@rm -f $(RESULT)



