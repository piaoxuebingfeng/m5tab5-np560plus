# np560_soc 组件：NP560+ 外设模型（LCD/键盘/ECC/NAND/定时器/音频/组合层）
idf_component_register(SRCS "np560_lcd.c" "np560_keypad.c" "np560_ecc.c" "np560_nand.c" "np560_nand_file.c" "np560_timers.c" "np560_audio.c" "np560_periph.c" "np560_soc.c"
                    INCLUDE_DIRS "include"
                    REQUIRES dmcu_core)
