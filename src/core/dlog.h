/* 极简落盘日志（ux0:data/psvsend/log.txt）。
 * 目的：真机没有 stdout，网络/发现的初始化错误码写进文件，
 * 通过 FTP 拉出来排查。仅初始化阶段与看门狗线程使用。 */
#ifndef PSVSEND_DLOG_H
#define PSVSEND_DLOG_H

/* 清空旧日志并打开（可重复调用） */
void dlog_init(void);
/* 追加一行（内部带时间戳毫秒）；fmt 同 printf */
void dlog(const char *fmt, ...);

#endif
