

# 定义头文件目录
INCDIRS := ./lib/v4l2/include ./lib/log/include

# 生成 -I 前缀
INC_FLAGS := $(addprefix -I,$(INCDIRS))

# 生成 .i 文件（预处理文件）
main.i: main.c
	gcc -E $< $(INC_FLAGS) -o $@
# 编译规则
main_test: main.c
	gcc -E $< $(INC_FLAGS) -o $@

# 清理
clean:
	rm -f main.i main_test