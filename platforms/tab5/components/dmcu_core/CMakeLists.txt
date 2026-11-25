# dmcu_core 组件：DMCU CPU 解释器核心
# 纯 C99，无 ESP-IDF 头依赖；host/Makefile 用同一批源文件构建主机测试。
idf_component_register(SRCS
    "dmcu_bus.c"
    "dmcu_regs.c"
    "dmcu_data.c"
    "dmcu_alu.c"
    "dmcu_irq.c"
    "dmcu_exec.c"
    "dmcu_clock.c"
    "np560_mp3_codec.c"
    "np560_mp3_hle_tab5.c"
    INCLUDE_DIRS "include" ".")
