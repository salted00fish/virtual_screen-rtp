QT       += core gui
QT       += network
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

# MinGW-w64 的 _mingw.h 默认把 _WIN32_WINNT 定成 0x502（Server 2003），
# 导致 winuser.h 里 #if _WIN32_WINNT >= 0x0600 包着的 SetProcessDPIAware /
# IsProcessDPIAware 等声明全被裁掉。提到 Windows 7（0x0601）才能用。
DEFINES += _WIN32_WINNT=0x0601 WINVER=0x0601

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    audio.cpp \
    avclock.cpp \
    capture_worker.cpp \
    ikcp.c \
    main.cpp \
    rtp.cpp \
    touch.cpp \
    video.cpp \
    widget.cpp

HEADERS += \
    audio.h \
    avclock.h \
    capture_worker.h \
    ikcp.h \
    rtp.h \
    touch.h \
    video.h \
    widget.h

FORMS += \
    widget.ui

# ============================================================================
#  Opus 1.5.2（源码内置，浮点构建，x86-64 走纯 C 路径）
#
#  源文件清单来自上游 celt_sources.mk / silk_sources.mk / opus_sources.mk，
#  只取编译 libopus 必须的部分：
#    - 不含任何带 main() 的文件（opus_demo.c / opus_compare.c /
#      repacketizer_demo.c / opus_custom_demo.c / test_*）；
#    - 不含 ARM / MIPS / x86 汇编与 intrinsics 优化（celt|silk/{arm,x86,mips}），
#      这些引用全部被 OPUS_X86_MAY_HAVE_* / OPUS_ARM_* / OPUS_HAVE_RTCD 宏包住，
#      这里都不定义，所以不会去找那些文件；
#    - 不含 dnn/（DRED / OSCE / DEEP_PLC 未启用）。
#
#  OPUS_BUILD : 表示正在编译库本体（公开头文件靠它切换导出宏）
#  VAR_ARRAYS : 用 C99 变长数组做栈分配；MinGW gcc 7.3 默认 -std=gnu11，支持
#  注意：不要定义 HAVE_CONFIG_H —— 本目录没有 config.h。
# ============================================================================
OPUS_DIR = $$PWD/3rdparty/opus
INCLUDEPATH += $$OPUS_DIR/include $$OPUS_DIR/celt $$OPUS_DIR/silk $$OPUS_DIR/silk/float $$OPUS_DIR/src
DEFINES += OPUS_BUILD VAR_ARRAYS

SOURCES += \
    $$OPUS_DIR/celt/bands.c \
    $$OPUS_DIR/celt/celt.c \
    $$OPUS_DIR/celt/celt_decoder.c \
    $$OPUS_DIR/celt/celt_encoder.c \
    $$OPUS_DIR/celt/celt_lpc.c \
    $$OPUS_DIR/celt/cwrs.c \
    $$OPUS_DIR/celt/entcode.c \
    $$OPUS_DIR/celt/entdec.c \
    $$OPUS_DIR/celt/entenc.c \
    $$OPUS_DIR/celt/kiss_fft.c \
    $$OPUS_DIR/celt/laplace.c \
    $$OPUS_DIR/celt/mathops.c \
    $$OPUS_DIR/celt/mdct.c \
    $$OPUS_DIR/celt/modes.c \
    $$OPUS_DIR/celt/pitch.c \
    $$OPUS_DIR/celt/quant_bands.c \
    $$OPUS_DIR/celt/rate.c \
    $$OPUS_DIR/celt/vq.c \
    $$OPUS_DIR/silk/A2NLSF.c \
    $$OPUS_DIR/silk/CNG.c \
    $$OPUS_DIR/silk/HP_variable_cutoff.c \
    $$OPUS_DIR/silk/LPC_analysis_filter.c \
    $$OPUS_DIR/silk/LPC_fit.c \
    $$OPUS_DIR/silk/LPC_inv_pred_gain.c \
    $$OPUS_DIR/silk/LP_variable_cutoff.c \
    $$OPUS_DIR/silk/NLSF2A.c \
    $$OPUS_DIR/silk/NLSF_VQ.c \
    $$OPUS_DIR/silk/NLSF_VQ_weights_laroia.c \
    $$OPUS_DIR/silk/NLSF_decode.c \
    $$OPUS_DIR/silk/NLSF_del_dec_quant.c \
    $$OPUS_DIR/silk/NLSF_encode.c \
    $$OPUS_DIR/silk/NLSF_stabilize.c \
    $$OPUS_DIR/silk/NLSF_unpack.c \
    $$OPUS_DIR/silk/NSQ.c \
    $$OPUS_DIR/silk/NSQ_del_dec.c \
    $$OPUS_DIR/silk/PLC.c \
    $$OPUS_DIR/silk/VAD.c \
    $$OPUS_DIR/silk/VQ_WMat_EC.c \
    $$OPUS_DIR/silk/ana_filt_bank_1.c \
    $$OPUS_DIR/silk/biquad_alt.c \
    $$OPUS_DIR/silk/bwexpander.c \
    $$OPUS_DIR/silk/bwexpander_32.c \
    $$OPUS_DIR/silk/check_control_input.c \
    $$OPUS_DIR/silk/code_signs.c \
    $$OPUS_DIR/silk/control_SNR.c \
    $$OPUS_DIR/silk/control_audio_bandwidth.c \
    $$OPUS_DIR/silk/control_codec.c \
    $$OPUS_DIR/silk/debug.c \
    $$OPUS_DIR/silk/dec_API.c \
    $$OPUS_DIR/silk/decode_core.c \
    $$OPUS_DIR/silk/decode_frame.c \
    $$OPUS_DIR/silk/decode_indices.c \
    $$OPUS_DIR/silk/decode_parameters.c \
    $$OPUS_DIR/silk/decode_pitch.c \
    $$OPUS_DIR/silk/decode_pulses.c \
    $$OPUS_DIR/silk/decoder_set_fs.c \
    $$OPUS_DIR/silk/enc_API.c \
    $$OPUS_DIR/silk/encode_indices.c \
    $$OPUS_DIR/silk/encode_pulses.c \
    $$OPUS_DIR/silk/gain_quant.c \
    $$OPUS_DIR/silk/init_decoder.c \
    $$OPUS_DIR/silk/init_encoder.c \
    $$OPUS_DIR/silk/inner_prod_aligned.c \
    $$OPUS_DIR/silk/interpolate.c \
    $$OPUS_DIR/silk/lin2log.c \
    $$OPUS_DIR/silk/log2lin.c \
    $$OPUS_DIR/silk/pitch_est_tables.c \
    $$OPUS_DIR/silk/process_NLSFs.c \
    $$OPUS_DIR/silk/quant_LTP_gains.c \
    $$OPUS_DIR/silk/resampler.c \
    $$OPUS_DIR/silk/resampler_down2.c \
    $$OPUS_DIR/silk/resampler_down2_3.c \
    $$OPUS_DIR/silk/resampler_private_AR2.c \
    $$OPUS_DIR/silk/resampler_private_IIR_FIR.c \
    $$OPUS_DIR/silk/resampler_private_down_FIR.c \
    $$OPUS_DIR/silk/resampler_private_up2_HQ.c \
    $$OPUS_DIR/silk/resampler_rom.c \
    $$OPUS_DIR/silk/shell_coder.c \
    $$OPUS_DIR/silk/sigm_Q15.c \
    $$OPUS_DIR/silk/sort.c \
    $$OPUS_DIR/silk/stereo_LR_to_MS.c \
    $$OPUS_DIR/silk/stereo_MS_to_LR.c \
    $$OPUS_DIR/silk/stereo_decode_pred.c \
    $$OPUS_DIR/silk/stereo_encode_pred.c \
    $$OPUS_DIR/silk/stereo_find_predictor.c \
    $$OPUS_DIR/silk/stereo_quant_pred.c \
    $$OPUS_DIR/silk/sum_sqr_shift.c \
    $$OPUS_DIR/silk/table_LSF_cos.c \
    $$OPUS_DIR/silk/tables_LTP.c \
    $$OPUS_DIR/silk/tables_NLSF_CB_NB_MB.c \
    $$OPUS_DIR/silk/tables_NLSF_CB_WB.c \
    $$OPUS_DIR/silk/tables_gain.c \
    $$OPUS_DIR/silk/tables_other.c \
    $$OPUS_DIR/silk/tables_pitch_lag.c \
    $$OPUS_DIR/silk/tables_pulses_per_block.c \
    $$OPUS_DIR/silk/float/LPC_analysis_filter_FLP.c \
    $$OPUS_DIR/silk/float/LPC_inv_pred_gain_FLP.c \
    $$OPUS_DIR/silk/float/LTP_analysis_filter_FLP.c \
    $$OPUS_DIR/silk/float/LTP_scale_ctrl_FLP.c \
    $$OPUS_DIR/silk/float/apply_sine_window_FLP.c \
    $$OPUS_DIR/silk/float/autocorrelation_FLP.c \
    $$OPUS_DIR/silk/float/burg_modified_FLP.c \
    $$OPUS_DIR/silk/float/bwexpander_FLP.c \
    $$OPUS_DIR/silk/float/corrMatrix_FLP.c \
    $$OPUS_DIR/silk/float/encode_frame_FLP.c \
    $$OPUS_DIR/silk/float/energy_FLP.c \
    $$OPUS_DIR/silk/float/find_LPC_FLP.c \
    $$OPUS_DIR/silk/float/find_LTP_FLP.c \
    $$OPUS_DIR/silk/float/find_pitch_lags_FLP.c \
    $$OPUS_DIR/silk/float/find_pred_coefs_FLP.c \
    $$OPUS_DIR/silk/float/inner_product_FLP.c \
    $$OPUS_DIR/silk/float/k2a_FLP.c \
    $$OPUS_DIR/silk/float/noise_shape_analysis_FLP.c \
    $$OPUS_DIR/silk/float/pitch_analysis_core_FLP.c \
    $$OPUS_DIR/silk/float/process_gains_FLP.c \
    $$OPUS_DIR/silk/float/regularize_correlations_FLP.c \
    $$OPUS_DIR/silk/float/residual_energy_FLP.c \
    $$OPUS_DIR/silk/float/scale_copy_vector_FLP.c \
    $$OPUS_DIR/silk/float/scale_vector_FLP.c \
    $$OPUS_DIR/silk/float/schur_FLP.c \
    $$OPUS_DIR/silk/float/sort_FLP.c \
    $$OPUS_DIR/silk/float/warped_autocorrelation_FLP.c \
    $$OPUS_DIR/silk/float/wrappers_FLP.c \
    $$OPUS_DIR/src/opus.c \
    $$OPUS_DIR/src/opus_encoder.c \
    $$OPUS_DIR/src/opus_decoder.c \
    $$OPUS_DIR/src/extensions.c \
    $$OPUS_DIR/src/repacketizer.c \
    $$OPUS_DIR/src/opus_multistream.c \
    $$OPUS_DIR/src/opus_multistream_encoder.c \
    $$OPUS_DIR/src/opus_multistream_decoder.c \
    $$OPUS_DIR/src/opus_projection_encoder.c \
    $$OPUS_DIR/src/opus_projection_decoder.c \
    $$OPUS_DIR/src/mapping_matrix.c \
    $$OPUS_DIR/src/analysis.c \
    $$OPUS_DIR/src/mlp.c \
    $$OPUS_DIR/src/mlp_data.c

# 屏幕采集（screencap.cpp / capture_worker.cpp）直接调 Win32 + DXGI/D3D11：
#   user32  -> GetCursorInfo / DrawIconEx / GetSystemMetrics / OpenInputDesktop...
#   gdi32   -> CreateDIBSection / DrawIconEx 用到的 DC / 位图 / 光标合成
#   dxgi    -> CreateDXGIFactory1 / IDXGIOutput1::DuplicateOutput（GPU 桌面复制）
#   d3d11   -> D3D11CreateDevice / CopyResource / Map
#   ole32   -> CoInitializeEx / CoUninitialize（采集线程里的 COM）
# 注意：gcc 只在 -mwindows 时才自动带 gdi32，本工程链接用的是
# -Wl,-subsystem,windows，所以这些库都要显式写出来。
win32: LIBS += -lgdi32 -luser32 -ldxgi -ld3d11 -lole32

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
