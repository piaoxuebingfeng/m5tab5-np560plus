# host 快速回归：直接用系统 gcc 编译 dmcu_core + np560_soc 源文件
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L \
	-Ishim \
	-I../platforms/tab5/components/dmcu_core/include \
	-I../platforms/tab5/components/np560_soc/include
CORE_SRCS = $(wildcard ../platforms/tab5/components/dmcu_core/*.c)
SOC_SRCS = $(wildcard ../platforms/tab5/components/np560_soc/*.c)

dmcu_runner: runner.c $(CORE_SRCS) $(SOC_SRCS)
	$(CC) $(CFLAGS) -o $@ runner.c $(CORE_SRCS) $(SOC_SRCS)

clean:
	rm -f dmcu_runner

.PHONY: clean
