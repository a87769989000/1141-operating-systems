
CC = riscv64-linux-gnu-gcc


CFLAGS = -Wall -pthread -static


STUDENT_ID = a141064

TARGET = sched_demo_$(STUDENT_ID)


SRC = $(TARGET).c


all: $(TARGET)


$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $< -o $@


clean:
	rm -f $(TARGET)


.PHONY: all clean