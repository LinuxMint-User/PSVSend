/* LocalSend for PS Vita
 * 入口: 初始化 vita2d, 显示启动画面文字
 * 后续实现 LocalSend v2 协议 */
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>

int main(void)
{
    vita2d_init();
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));

    vita2d_pvf *font = vita2d_load_default_pvf();

    while (1) {
        vita2d_start_drawing();
        vita2d_clear_screen();

        if (font) {
            const char *text = "PSVSend v0.1.0";
            int w, h;
            vita2d_pvf_text_dimensions(font, 2.0f, text, &w, &h);
            vita2d_pvf_draw_text(font, (960 - w) / 2, (544 - h) / 2,
                                 RGBA8(255, 255, 255, 255), 2.0f, text);
        }

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
