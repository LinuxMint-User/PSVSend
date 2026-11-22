/* PSVSend 入口：初始化 vita2d 后进入 UI 主循环 */
#include <vita2d.h>
#include "ui/ui.h"

int main(void)
{
    vita2d_init();
    vita2d_set_vblank_wait(1);

    /* 应用不提供内部退出：ui_run 不会返回，只能由系统 HOME 关闭，
     * 故其后无需清理；return 0 仅为满足 main 的返回类型。 */
    ui_run();
    return 0;
}
