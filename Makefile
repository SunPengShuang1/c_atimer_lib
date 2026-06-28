
# 编译器设置
CC = gcc
# 编译标志：
# -Wall -Wextra: 开启大部分警告
# -O2: 优化等级2
# -g: 生成调试信息
# -std=c99: 使用 C99 标准
# 必须定义 _GNU_SOURCE 以启用 timerfd 和 CLOCK_MONOTONIC	
#		-D_GNU_SOURCE:在 Makefile 中定义更稳妥，确保所有 POSIX 扩展可用
 
CFLAGS = -Wall -Wextra -O2 -g -std=c99 -D_GNU_SOURCE
# 链接标志：需要 pthread 库和 rt 库（实时扩展，虽然现代 Linux 通常合并在 libc 中，但显式链接更兼容）
LDFLAGS = -lpthread -lrt

# 源文件列表
SRCS = atimer.c tpool.c main.c
# 对象文件列表（将 .c 替换为 .o）
OBJS = $(SRCS:.c=.o)

# 最终可执行文件名
TARGET = demo_timer

# 默认目标：构建可执行文件
all: $(TARGET)

# 链接规则：将所有对象文件链接成可执行文件
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# 编译规则：将每个 .c 文件编译为对应的 .o 文件
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 清理规则：删除生成的对象文件和可执行文件
clean:
	rm -f $(OBJS) $(TARGET)

# 声明伪目标，防止与同名文件冲突
.PHONY: all clean
