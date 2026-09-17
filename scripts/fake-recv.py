#!/usr/bin/env python3
"""fake-recv.py —— 假 LocalSend v2 接收端，用来验证 PSVSend 发送侧的「逐文件独立成败」。

为什么要它：官方 LocalSend 客户端（电脑/手机）的接收弹窗只能整体接受或拒绝，
没法"只收一部分文件"，所以「对端只接受子集」这条路径在单机条件下没法用官方端复现。

它在 53317 端口起一个明文 HTTP 服务。PSV 发现设备时是主动扫本网段每个 IP 的
53317（POST /api/localsend/v2/register），所以不用发组播，PSV 扫描后就会看到一台
叫 FAKE-RECV 的设备。

前提：
  1. PC 与 PSV 在同一个局域网 / 同一网段；
  2. 先关掉 PC 上的官方 LocalSend（它同样占 53317 端口，会冲突）；
  3. PC 防火墙别挡 53317。

用法（PSV 端一次选 3 个文件发送最直观）：
  python3 fake-recv.py            全部接受（对照组，PSV 应显示绿色「已完成」）
  python3 fake-recv.py --skip 2   第 2 个文件不回 token（对端只收子集）
  python3 fake-recv.py --fail 2   第 2 个文件的上传回 HTTP 500
  python3 fake-recv.py --cut 2    第 2 个文件传到一半直接断连（模拟对端消失）
  python3 fake-recv.py --chunked  prepare 回执用 chunked 编码回（不写 Content-Length）
  python3 fake-recv.py --cut-resp 收到 prepare 清单后立刻断连，一个字都不回

多个文件可写逗号：--skip 1,3
"""

import argparse
import http.server
import json
import socket
import socketserver
import sys
import time

PORT = 53317  # 协议默认端口；PSV 发送出去的目标 port 取自我们应答里的 port 字段


def info_json():
    """PSV 探测时读这个（parse_member 要 alias，protocol=http 表示明文）。"""
    return json.dumps({
        "alias": "FAKE-RECV",
        "version": "2.0",
        "deviceModel": "fake-recv",
        "deviceType": "desktop",
        "fingerprint": "aa" * 32,   # 任意 64 位 hex，只要不等于 PSV 自己的就行
        "port": PORT,
        "protocol": "http",
        "download": False,
    }).encode()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"      # 短连接：每个请求一条连接，正是 PSV 的用法

    def log_message(self, *a):
        pass                            # 关掉基类的 stderr 噪音

    def log_error(self, *a):
        pass

    def handle_one_request(self):
        """PSV 探测第一轮会先试 TLS（发二进制 ClientHello），基类解析它会报错。
        静默吞掉即可——PSV 第二轮会自动改用明文重连。"""
        try:
            http.server.BaseHTTPRequestHandler.handle_one_request(self)
        except Exception:
            pass

    def reply(self, code, body=b""):
        try:
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if body:
                self.wfile.write(body)
        except Exception:
            pass

    def reply_chunked(self, code, body):
        """用 chunked 编码回（不写 Content-Length），验证 PSV 能正确解出 body。"""
        try:
            out = ("HTTP/1.1 %d OK\r\nContent-Type: application/json\r\n"
                   "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                   % code).encode()
            if not body:
                body = b""
            for i in range(0, len(body), 20):      # 故意切小块，逼真机多次收
                c = body[i:i + 20]
                out += ("%x\r\n" % len(c)).encode() + c + b"\r\n"
            out += b"0\r\n\r\n"
            self.wfile.write(out)
        except Exception:
            pass

    def read_body(self):
        """把请求体读出来（prepare-upload 的 JSON 用，内容很小）。"""
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        data = b""
        try:
            while len(data) < n:
                chunk = self.rfile.read(min(1 << 16, n - len(data)))
                if not chunk:
                    break
                data += chunk
        except Exception:
            pass
        return data

    def drain_body(self):
        """读完并丢弃请求体，只返回收到多少字节（上传的文件可能很大，别存内存）。"""
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        got = 0
        try:
            while got < n:
                chunk = self.rfile.read(min(1 << 16, n - got))
                if not chunk:
                    break
                got += len(chunk)
        except Exception:
            pass
        return got

    @staticmethod
    def file_index(query):
        for part in query.split("&"):
            if part.startswith("fileId="):
                v = part[len("fileId="):]
                if v.startswith("f"):
                    v = v[1:]
                try:
                    return int(v)
                except ValueError:
                    return -1
        return -1

    def do_POST(self):
        path, _, query = self.path.partition("?")
        path = path.rstrip("/")

        if path.endswith("/register"):
            self.drain_body()
            print("[fake-recv] register ← PSV 正在探测（已作为设备 FAKE-RECV 应答）")
            self.reply(200, info_json())

        elif path.endswith("/prepare-upload"):
            keys = []
            try:
                doc = json.loads(self.read_body() or b"{}")
                fmap = doc.get("files", {})
                keys = sorted(fmap.keys(),
                              key=lambda k: int(k[1:]) if k[1:].isdigit() else 0)
                for k in keys:      # 打印对方报来的文件名：非法 UTF-8/JSON 被截，会当场暴露
                    v = fmap.get(k)
                    v = v if isinstance(v, dict) else {}
                    print("[fake-recv]   %s: %r size=%s"
                          % (k, v.get("fileName"), v.get("size")))
            except Exception as e:
                print("[fake-recv] prepare 请求体解析失败：%s"
                      "（JSON 被截断？或文件名含非法 UTF-8 序列）" % e)
            print("[fake-recv] prepare-upload：对端要发 %d 个文件（%s）"
                  % (len(keys), ", ".join(keys)))
            if ARGS.cut_resp:
                # 一个字都不回就断连：PSV 侧 read_resp 会读失败 → 整批中止在准备阶段
                print("[fake-recv] prepare → 直接断连（不回任何响应）")
                self.close_connection = True
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except Exception:
                    pass
                try:
                    self.connection.close()
                except Exception:
                    pass
                return
            files = {}
            for i, k in enumerate(keys):
                if (i + 1) in ARGS.skip:
                    print("[fake-recv]   %s → 不接收（不回 token）" % k)
                    continue
                files[k] = "tok-%s" % k
            payload = json.dumps({
                "sessionId": "fake-%d" % int(time.time()),
                "files": files,
            }).encode()
            if ARGS.chunked:
                print("[fake-recv] prepare 回执 → 用 chunked 编码回（故意不写 Content-Length）")
                self.reply_chunked(200, payload)
            else:
                self.reply(200, payload)

        elif path.endswith("/upload"):
            idx = self.file_index(query)
            if (idx + 1) in ARGS.cut:
                try:
                    self.rfile.read(1 << 16)   # 收一点就断，让发送方写的时候出错
                except Exception:
                    pass
                print("[fake-recv] upload f%d → 中途断连（模拟对端消失）" % idx)
                self.close_connection = True
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except Exception:
                    pass
                try:
                    self.connection.close()
                except Exception:
                    pass
                return
            got = self.drain_body()
            if (idx + 1) in ARGS.fail:
                print("[fake-recv] upload f%d ← %d 字节 → 回 HTTP 500" % (idx, got))
                self.reply(500)
            else:
                print("[fake-recv] upload f%d ← %d 字节 → 回 200" % (idx, got))
                self.reply(200)

        elif path.endswith("/cancel"):
            self.drain_body()
            print("[fake-recv] cancel ← 对端通知取消")
            self.reply(200)

        else:
            self.drain_body()
            self.reply(404)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def id_list(s):
    return set(int(x) for x in s.replace(" ", "").split(",") if x)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip", type=id_list, default=set(),
                    help="不接收第 N 个文件（不回 token），从 1 数，可逗号分隔")
    ap.add_argument("--fail", type=id_list, default=set(),
                    help="第 N 个文件的上传回 HTTP 500，可逗号分隔")
    ap.add_argument("--cut", type=id_list, default=set(),
                    help="第 N 个文件的上传中途断连，可逗号分隔")
    ap.add_argument("--chunked", action="store_true",
                    help="prepare 回执用 chunked 编码回（不写 Content-Length）")
    ap.add_argument("--cut-resp", action="store_true",
                    help="收到 prepare 清单后直接断连（不回响应），测连接级中止")
    ARGS = ap.parse_args()

    if ARGS.cut_resp:
        print("[fake-recv] 模式：prepare 阶段直接断连（不回响应）")
    if ARGS.chunked:
        print("[fake-recv] 模式：prepare 回执走 chunked 编码（无 Content-Length）")
    if ARGS.skip:
        print("[fake-recv] 模式：文件 %s 不接收（不回 token）" % sorted(ARGS.skip))
    if ARGS.fail:
        print("[fake-recv] 模式：文件 %s 上传回 HTTP 500" % sorted(ARGS.fail))
    if ARGS.cut:
        print("[fake-recv] 模式：文件 %s 上传中途断连" % sorted(ARGS.cut))
    print("[fake-recv] 监听 0.0.0.0:%d —— PSV 扫描后会看到设备 FAKE-RECV，Ctrl-C 退出" % PORT)
    sys.stdout.flush()
    try:
        Server(("0.0.0.0", PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        print("\n[fake-recv] 退出")
