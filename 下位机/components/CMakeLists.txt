# components 整体作为一个 ESP-IDF 组件注册。（和 esp32s31_test 工程同一套写法）
#
# 【重要】该目录下不能再出现其它 CMakeLists.txt，否则 ESP-IDF 会把整个 components
# 目录当作普通目录而不再扫描它，里面的源码全部不会被编译。
#
# 子目录说明：
#   usb/          USB CDC 串口封装（基于 TinyUSB）
#   tinyusb/      TinyUSB 协议栈本体（vendor 源码，勿改，升级时整体替换）
#   esp_tinyusb/  Espressif 的 TinyUSB 封装层（tinyusb_cdc_acm 等 API）
#
# 以后加新模块（wifi/udp/rtp/lcd...）就在这个目录下建子目录，
# 把名字追加到下面的 src_dirs / include_dirs 即可，不要在子目录里放 CMakeLists.txt。

# 注意：idf_component_register 同时给出 SRCS 与 SRC_DIRS 时，SRC_DIRS 收集到的源文件
# 会被丢弃（实测：libcomponents.a 里只剩 SRCS 列出的文件）。所以下面只用 SRC_DIRS 注册，
# esp_tinyusb 里按 Kconfig 条件生效的文件在注册后用 target_sources 单独追加（绝对路径）。
set(comp_dir ${CMAKE_CURRENT_LIST_DIR})

# ---------------- 自有源码（无条件编译） ----------------
set(src_dirs      usb led wifi lcd udp video jpeg opus audio kcp touch)
set(include_dirs  usb led wifi lcd udp video jpeg opus audio kcp touch)

# ---------------- TinyUSB（仅在 CDC 启用时编译） ----------------
# 当前日志走 USB CDC，所以 CONFIG_TINYUSB_CDC_ENABLED=y，这块参与编译。
if(CONFIG_TINYUSB_CDC_ENABLED)

    # ---- 协议栈本体（components/tinyusb）：对应上游 CMakeLists 的 SRCS 列表 ----
    list(APPEND src_dirs
        tinyusb/src
        tinyusb/src/common
        tinyusb/src/device
        tinyusb/src/class/audio
        tinyusb/src/class/bth
        tinyusb/src/class/cdc
        tinyusb/src/class/dfu
        tinyusb/src/class/hid
        tinyusb/src/class/midi
        tinyusb/src/class/msc
        tinyusb/src/class/mtp
        tinyusb/src/class/net
        tinyusb/src/class/printer
        tinyusb/src/class/usbtmc
        tinyusb/src/class/vendor
        tinyusb/src/class/video
        tinyusb/src/portable/synopsys/dwc2
        )
    # 上游的公开/私有 include 目录
    list(APPEND include_dirs
        tinyusb/src
        tinyusb/src/device
        )

    # ---- Espressif 封装层（components/esp_tinyusb） ----
    # 这些文件在上游是"按 Kconfig 条件"才编译的，必须显式列出：
    # 若用 SRC_DIRS 整目录收集，tinyusb_msc.c / tinyusb_net.c / tinyusb_pm.c 等
    # 会在对应功能关闭时也参与编译而报错。
    set(esp_tusb_srcs
        ${comp_dir}/esp_tinyusb/descriptors_control.c
        ${comp_dir}/esp_tinyusb/tinyusb.c
        ${comp_dir}/esp_tinyusb/usb_descriptors.c
        ${comp_dir}/esp_tinyusb/tinyusb_task.c
        )

    # CDC（含 VFS/console 桥接）
    if(CONFIG_TINYUSB_CDC_ENABLED)
        list(APPEND esp_tusb_srcs
            ${comp_dir}/esp_tinyusb/cdc.c
            ${comp_dir}/esp_tinyusb/tinyusb_cdc_acm.c
            )
        if(CONFIG_VFS_SUPPORT_IO)
            list(APPEND esp_tusb_srcs
                ${comp_dir}/esp_tinyusb/tinyusb_console.c
                ${comp_dir}/esp_tinyusb/vfs_tinyusb.c
                )
        endif()
    endif()

    # MSC（U 盘）
    if(CONFIG_TINYUSB_MSC_ENABLED)
        list(APPEND esp_tusb_srcs
            ${comp_dir}/esp_tinyusb/tinyusb_msc.c
            ${comp_dir}/esp_tinyusb/storage_spiflash.c
            )
        if(CONFIG_SOC_SDMMC_HOST_SUPPORTED)
            list(APPEND esp_tusb_srcs ${comp_dir}/esp_tinyusb/storage_sdmmc.c)
        endif()
    endif()

    # MTP（媒体传输协议）
    if(CONFIG_TINYUSB_MTP_ENABLED)
        list(APPEND esp_tusb_srcs
            ${comp_dir}/esp_tinyusb/mtp/tinyusb_mtp.c
            ${comp_dir}/esp_tinyusb/mtp/tinyusb_mtp_codec.c
            ${comp_dir}/esp_tinyusb/mtp/tinyusb_mtp_object_store.c
            ${comp_dir}/esp_tinyusb/mtp/tinyusb_mtp_operations.c
            ${comp_dir}/esp_tinyusb/mtp/tinyusb_mtp_properties.c
            ${comp_dir}/esp_tinyusb/mtp/tinyusb_mtp_read.c
            ${comp_dir}/esp_tinyusb/mtp/tinyusb_mtp_transfer.c
            ${comp_dir}/esp_tinyusb/mtp/tinyusb_mtp_transport.c
            ${comp_dir}/esp_tinyusb/mtp/tinyusb_mtp_write.c
            )
    endif()

    # 网络（NCM）：需要 lib/networking，暂未启用
    if(CONFIG_TINYUSB_NET_MODE_NCM)
        list(APPEND esp_tusb_srcs ${comp_dir}/esp_tinyusb/tinyusb_net.c)
    endif()

    # esp32p4 / esp32s31 的高速 USB 口需要软件 VBUS 监测
    if(CONFIG_IDF_TARGET_ESP32P4 OR CONFIG_IDF_TARGET_ESP32S31)
        list(APPEND esp_tusb_srcs ${comp_dir}/esp_tinyusb/tinyusb_vbus_monitor.c)
    endif()

    # 电源管理（休眠唤醒）
    if(CONFIG_TINYUSB_PM)
        list(APPEND esp_tusb_srcs ${comp_dir}/esp_tinyusb/tinyusb_pm.c)
    endif()
    if(CONFIG_TINYUSB_USB_OTG_WAKEUP)
        list(APPEND esp_tusb_srcs ${comp_dir}/esp_tinyusb/tinyusb_usb_wakeup.c)
    endif()

    list(APPEND include_dirs
        esp_tinyusb/include
        esp_tinyusb/include_private
        )

endif() # CONFIG_TINYUSB_CDC_ENABLED

# ---------------- 依赖 ----------------
set(requires esp_driver_gpio esp_hw_support esp_timer heap vfs esp_pm esp_wifi esp_netif
             esp_event nvs_flash lwip esp_lcd esp_driver_jpeg esp_driver_i2s esp_audio_codec esp_driver_i2c
   )

idf_component_register(SRC_DIRS ${src_dirs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${requires}
                       KCONFIG "${comp_dir}/esp_tinyusb/Kconfig")

if(CONFIG_TINYUSB_CDC_ENABLED)
    # 追加 esp_tinyusb 中按 Kconfig 条件生效的源文件（绝对路径）
    target_sources(${COMPONENT_LIB} PRIVATE ${esp_tusb_srcs})

    # 上游用 target_compile_options(... PUBLIC) 传递，本地合并为一个组件后在这里补上
    target_compile_options(${COMPONENT_LIB} PUBLIC -DCFG_TUSB_MCU=OPT_MCU_ESP32S31)

    # 上游对 usbd.c 的局部警告抑制
    set_source_files_properties("tinyusb/src/device/usbd.c"
                                PROPERTIES COMPILE_FLAGS "-Wno-type-limits")
endif()
