/* PSVSend 入口：初始化 vita2d 后进入 UI 主循环 */
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
#include "ui/ui.h"

int main(void)
{
    vita2d_init();
    vita2d_set_vblank_wait(1);

    ui_run();

    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
