#ifndef BOARD_LCD_CONFIG_H
#define BOARD_LCD_CONFIG_H

/*
 * ============================================================
 *  板级 LCD 配置 —— 硬件改动记录
 * ============================================================
 *
 * 【重要硬件改动】背光 EN（LCD_BL）已由用户短接到 SD_CLK。
 *
 * 依据数据手册引脚表（第 100 页），SDIO 的引脚号就是 GPIO 号：
 *
 *   芯片引脚 27  SDIO_DATA0  ->  GPIO20
 *   芯片引脚 28  SDIO_DATA1  ->  GPIO21
 *   芯片引脚 29  SDIO_DATA2  ->  GPIO22
 *   芯片引脚 31  SDIO_DATA3  ->  GPIO23
 *   芯片引脚 32  SDIO_CLK    ->  GPIO24   <== 背光 EN 短接在这里
 *   芯片引脚 33  SDIO_CMD    ->  GPIO25
 *
 * 后果与约束：
 *   1. 控制背光 = 驱动 GPIO24（BOARD_LCD_BL_GPIO）。
 *   2. SDMMC 外设一旦初始化，会通过 IOMUX 抢占 GPIO24 并输出时钟，
 *      背光就会跟着时钟高频闪，同时 SD 卡也无法正常工作
 *      → 本板禁止初始化 SDMMC / 把 GPIO24 交还给 SD 外设。
 *   3. GPIO20~GPIO25 都在 VDDPST_SD 域上。若该域没有电到 pad，
 *      则这些脚输出无效（实测 GPIO25 曾为 0V）——这时背光仍然不会亮。
 */

#define BOARD_LCD_BL_GPIO           GPIO_NUM_24
#define BOARD_LCD_RES_GPIO          GPIO_NUM_20

/* 是否由本固件驱动 GPIO24 来做背光开关。
 * 置 1：初始化时把 GPIO24 拉高 -> 背光常亮。
 * 置 0：完全不碰这个脚，交给其它逻辑（例如你自己想手动控制）。 */
#define BOARD_LCD_BL_FROM_SDCLK     1

/* 若背光极性相反（低电平点亮），改成 0 */
#define BOARD_LCD_BL_ACTIVE_HIGH    1

/*
 * 防护：本板严禁初始化 SDMMC。
 * GPIO24 已被背光占用，交给 SD 外设会导致背光闪烁并使 SD 功能异常。
 */
#define BOARD_SDMMC_MUST_STAY_OFF   1

#endif /* BOARD_LCD_CONFIG_H */
