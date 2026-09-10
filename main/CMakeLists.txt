idf_component_register(
    SRCS "main.c"
         "dex_ui.c"
         "dex_battery.c"
         "dex_core.c"
         "dex_layout.c"
         "dex_sprite.c"
         "dex_static.c"
         "dex_audio.c"
         "dex_adpcm.c"
         "dex_bgm_data.c"
         "vendor/miniz/miniz.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash
)

# 只用 miniz 的 tinfl(inflate)路径,关闭压缩/归档 API 以减体积。
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MINIZ_NO_ARCHIVE_APIS
    MINIZ_NO_STDIO
    MINIZ_NO_TIME
)

# 离线资源 blob:嵌入只读段,符号 _binary_dex_sprites_bin_start/_end、
# _binary_dex_attrs_bin_start/_end(见 dex_static.h)。
target_add_binary_data(${COMPONENT_LIB} "dex_sprites.bin" TEXT)
target_add_binary_data(${COMPONENT_LIB} "dex_attrs.bin" TEXT)
