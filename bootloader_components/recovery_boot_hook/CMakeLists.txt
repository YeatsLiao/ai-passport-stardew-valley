# IDF_PATH 可能带 Windows 反斜杠(D:\Software\...),在 CMake 二次解析中
# "\S" 等会被当作非法转义序列,先统一转换为正斜杠。
file(TO_CMAKE_PATH "$ENV{IDF_PATH}" _idf_path)

idf_component_register(
    SRCS "recovery_boot_hook.c"
    PRIV_REQUIRES bootloader_support
    PRIV_INCLUDE_DIRS "${_idf_path}/components/bootloader_support/private_include"
)
