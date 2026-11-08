#!/usr/bin/env python3
"""fake-send.py —— 假 LocalSend v2 发送端，用来验证 PSVSend 接收侧的「单文件失败」。

为什么要它：官方 LocalSend 发送端的失败都是网络层偶发，没法稳定造出"某一个
文件校验失败、其余正常"的局面，而接收侧的部分失败（RECV_ST_PARTIAL）正是要
验证这条路径。

流程与官方一致（明文 HTTP，PSVSend 接收侧默认双栈）：
  1. POST /api/localsend/v2/prepare-upload 报清单（等 PSV 上点 Accept，最长 60s）
  2. 拿回 {sessionId, files{fileId: token}}
  3. 按文件 POST /api/localsend/v2/upload?sessionId&fileId&token 发裸字节

发现设备：不给 --ip 时自动扫本机所在 /24 的候选端口，再对开放端口发
GET /api/localsend/v2/info，能回 member info 的才算 LocalSend 设备。

用法：
  python3 scripts/fake-send.py
      自动找设备（本机 /24、端口 53317），找到 1 台就直接发

  python3 scripts/fake-send.py --ip 192.168.1.50
      指定地址（网络不允许广播/扫描时用）

  python3 scripts/fake-send.py --bad-sha 2
      第 2 个文件在清单里给一个错误的 sha256（内容本身是对的）→ PSV 收完后
      校验失败，应只把该文件标红，其余照收，终态「Complete, 1 failed」

  python3 scripts/fake-send.py --short 2
      第 2 个文件只发一半就断连 → 断流是会话级错误，应整批「传输中断」

  python3 scripts/fake-send.py --no-sha
      清单里 sha256 给 null（关掉接收侧校验的对照组）

  python3 scripts/fake-send.py --port 53317,4567
      扫描/连接多个端口（接收端口被改过时用）

M3/M6 边界用例（prepare 读取 / 拒绝 / 路径形态）：
  --count 2048 --size 0 --prepare-only   2048 个文件的清单应被接受（确认页）
  --count 2049 --size 0                  应回 413（文件数超过 2048，整份拒绝）
  --manifest-pad 4300000                 把清单撑到 4.1MB → 应回 413（清单过大）
  --prepare-short                        只发一半清单就 FIN → 应回 400（读短）
  --prepare-nocl                         清单不带 Content-Length → 走旧兼容路径
  --prepare-chunked                      清单用 chunked → 应回 411
  --prepare-concurrent 3                 同时发 3 份 → 1 份进入等待，其余 409

  --layout tree                          嵌套路径（FakeDir/2026/07/...）→ 应还原目录
  --layout case                          仅大小写不同的两个顶层 → 应合成一个根
  --layout bad                           非法 / 越界路径（../、绝对、盘符、空组件等）
                                         → 应跳过并计数，不落盘
  --layout long                          单级 250 字节 / 总路径超 512 → 应跳过或截断计数
  --layout reserved                      顶层带 .psvsend-part / .psvsend.tmp
                                         → 顶层冲突，只能改名/跳过

前提：PC 与 PSV 同一局域网；PSV 上的 PSVSend 已启动。
"""

import argparse
import concurrent.futures
import hashlib
import http.client
import json
import socket
import subprocess
import sys
import threading
import time

API = "/api/localsend/v2"


# ---------------- 设备发现 ----------------

def local_ips():
    """本机所有全局 IPv4（返回 [(ip, 前缀长度)]）。"""
    out = []
    try:
        txt = subprocess.check_output(
            ["ip", "-4", "-o", "addr", "show", "scope", "global"],
            text=True, stderr=subprocess.DEVNULL)
        for line in txt.splitlines():
            parts = line.split()
            if "inet" in parts:
                cidr = parts[parts.index("inet") + 1]
                ip, plen = cidr.split("/")
                out.append((ip, int(plen)))
    except Exception:
        pass
    if not out:                          # 没有 ip 命令时的兜底：UDP connect 拿主地址
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            out.append((s.getsockname()[0], 24))
            s.close()
        except Exception:
            pass
    return out


def probe_info(ip, port, timeout=2.0):
    """端口开着且 GET /info 回 200 → 视为 LocalSend 设备。"""
    s = socket.socket()
    s.settimeout(0.35)
    try:
        s.connect((ip, port))
    except Exception:
        return None
    finally:
        s.close()
    try:
        c = http.client.HTTPConnection(ip, port, timeout=timeout)
        c.request("GET", API + "/info")
        r = c.getresponse()
        body = r.read()
        c.close()
        if r.status != 200:
            return None
        j = json.loads(body)
        return {
            "ip": ip, "port": port,
            "alias": j.get("alias") or "?",
            "protocol": j.get("protocol") or "http",
        }
    except Exception:
        return None


def discover(ports):
    """扫本机所在 /24（每个端口）+ 验证 /info，返回设备列表。"""
    cand = []
    for ip, _plen in local_ips():
        base = ip.rsplit(".", 1)[0]
        cand += ["%s.%d" % (base, i) for i in range(1, 255)]
    cand = sorted(set(cand))
    print("[fake-send] 扫描 %d 个地址（端口 %s）..."
          % (len(cand), ",".join(str(p) for p in ports)))
    sys.stdout.flush()

    def one(ip):
        # 按候选顺序逐个试，命中即停（PSV 绑不了 53317，实际从 4567 起）
        for p in ports:
            d = probe_info(ip, p)
            if d:
                return [d]
        return []

    found = []
    with concurrent.futures.ThreadPoolExecutor(64) as ex:
        for hit in ex.map(one, cand):
            found += hit
    return found


# ---------------- HTTP 收发 ----------------

def post(ip, port, path, body=b"", ctype=None, timeout=90):
    """一次性 POST（短连接），返回 (status, body)。"""
    conn = http.client.HTTPConnection(ip, port, timeout=timeout)
    headers = {}
    if ctype:
        headers["Content-Type"] = ctype
    conn.request("POST", path, body=body, headers=headers)
    resp = conn.getresponse()
    data = resp.read()
    conn.close()
    return resp.status, data


def post_half(ip, port, path, body):
    """发一半 body 就硬断（模拟传输中断流），返回 None。"""
    conn = http.client.HTTPConnection(ip, port, timeout=30)
    conn.putrequest("POST", path)
    conn.putheader("Content-Length", str(len(body)))
    conn.endheaders()
    try:
        conn.send(body[: len(body) // 2])
        time.sleep(0.2)                 # 让对端先收下一部分
    except Exception:
        pass
    try:
        conn.sock.shutdown(socket.SHUT_RDWR)
    except Exception:
        pass
    try:
        conn.sock.close()
    except Exception:
        pass
    return None


def raw_exchange(ip, port, head, body=b"", half=False, timeout=15):
    """裸 socket 发请求（可造无 Content-Length / 短 body / chunked），
    发完 shutdown(WR) 让对端按 EOF 判完，返回 (status, raw)。"""
    s = socket.create_connection((ip, port), timeout=timeout)
    try:
        s.sendall(head)
        if body:
            s.sendall(body[: len(body) // 2] if half else body)
        try:
            s.shutdown(socket.SHUT_WR)
        except Exception:
            pass
        data = b""
        while True:
            try:
                b = s.recv(8192)
            except socket.timeout:
                break
            except Exception:
                break          # 对端回完响应就 close，RST 时忽略已收到的内容
            if not b:
                break
            data += b
        line = data.split(b"\r\n", 1)[0].decode(errors="replace")
        try:
            status = int(line.split()[1])
        except Exception:
            status = None
        return status, data
    finally:
        s.close()


def head_of(ip, port, extra=b""):
    return ("POST %s/prepare-upload HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Content-Type: application/json\r\n"
            "Connection: close\r\n" % (API, ip, port)).encode() + extra + b"\r\n"


# ---------------- 工具 ----------------

def id_list(s):
    return set(int(x) for x in s.replace(" ", "").split(",") if x)


def port_list(s):
    return [int(x) for x in s.replace(" ", "").split(",") if x]


def make_payload(seed, size):
    """可复现的伪随机内容（不必真随机，只要每个文件不同）。"""
    buf = bytearray(size)
    x = (seed * 2654435761) & 0xFFFFFFFF
    for i in range(size):
        x = (1103515245 * x + 12345) & 0xFFFFFFFF
        buf[i] = (x >> 16) & 0xFF
    return bytes(buf)


def layout_name(layout, i, stamp):
    """按 --layout 生成清单里的 fileName（接收侧据此还原相对路径）。"""
    n = i + 1
    if layout == "tree":
        return "FakeDir/2026/07/fake-%d-%d.bin" % (n, stamp)
    if layout == "case":
        top = "CaseOnly" if i % 2 == 0 else "caseonly"
        return "%s/fake-%d-%d.bin" % (top, n, stamp)
    if layout == "bad":
        bad = [
            "../evil-%d.txt" % n,          # 越界
            "/abs-%d.txt" % n,             # 绝对路径
            "a/../../b-%d.txt" % n,        # 中间越界
            "C:\\win-%d.txt" % n,          # 盘符 + 反斜杠
            "dir/./dot-%d.txt" % n,        # 当前目录组件
            "",                             # 空名字
            "a//b-%d.txt" % n,             # 空组件
        ]
        return bad[i % len(bad)]
    if layout == "long":
        if i % 2 == 0:
            return ("L" * 250) + "-%d.bin" % n        # 单级 >191 字节
        return "/".join(["D" * 120] * 5) + "/f-%d.bin" % n   # 相对路径 >512
    if layout == "reserved":
        top = "Reserved.psvsend-part" if i % 2 == 0 else "Reserved.psvsend.tmp"
        return "%s/fake-%d-%d.bin" % (top, n, stamp)
    return "fake-%d-%d.bin" % (n, stamp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", help="PSV 的 IP；省略则自动扫描本机所在 /24")
    ap.add_argument("--port", type=port_list,
                    default=[4567, 53317, 53318, 53316, 53319, 55555],
                    help="候选端口，可逗号分隔。Vita 系统保留 53317（bind EACCES），"
                         "PSVSend 实际按 4567 → 53318 → 53316 → 53319 → 55555 顺序取"
                         "第一个可绑的，所以默认就先试 4567")
    ap.add_argument("--count", type=int, default=3, help="文件个数（默认 3）")
    ap.add_argument("--size", type=int, default=262144, help="每个文件字节数（默认 256KB）")
    ap.add_argument("--bad-sha", type=id_list, default=set(),
                    help="第 N 个文件在清单里给错误 sha256（从 1 数，可逗号分隔）")
    ap.add_argument("--short", type=id_list, default=set(),
                    help="第 N 个文件只发一半就断连（造断流），可逗号分隔")
    ap.add_argument("--no-sha", action="store_true", help="清单里 sha256 全部给 null")
    ap.add_argument("--parallel", type=int, default=1,
                    help="并发上传数（默认 1；官方客户端会并发，可试 3）")
    # ---- M3 prepare 边界用例 ----
    ap.add_argument("--prepare-only", action="store_true",
                    help="只发 prepare（收到 200 后不再上传），用于测文件数/清单大小")
    ap.add_argument("--manifest-pad", type=int, default=0,
                    help="在清单里加一个 pad 字符串撑大 body（测 4MB 上限）")
    ap.add_argument("--prepare-short", action="store_true",
                    help="prepare 只发一半 body 就 FIN（期望 400）")
    ap.add_argument("--prepare-nocl", action="store_true",
                    help="prepare 不带 Content-Length（期望走旧兼容路径）")
    ap.add_argument("--prepare-chunked", action="store_true",
                    help="prepare 用 Transfer-Encoding: chunked（期望 411）")
    ap.add_argument("--prepare-concurrent", type=int, default=0,
                    help="同时发 N 份 prepare（期望 1 份进入等待，其余 409）")
    ap.add_argument("--prepare-wait", type=int, default=5,
                    help="并发 prepare 模式的等待秒数（默认 5）")
    # ---- M6 清单路径形态 ----
    ap.add_argument("--layout", default="flat",
                    choices=["flat", "tree", "case", "bad", "long", "reserved"],
                    help="fileName 形态：flat 平铺（默认）/ tree 嵌套目录 / "
                         "case 仅大小写不同 / bad 非法路径 / long 超长 / reserved 保留后缀")
    args = ap.parse_args()

    # ---- 0. 确定目标 ----
    ip, port = args.ip, args.port[0]
    if ip:
        # 指定了地址：在候选端口里挑真正在服务的那一个（PSV 一般 4567）
        for p in args.port:
            if probe_info(ip, p):
                port = p
                break
    else:
        devs = discover(args.port)
        if not devs:
            print("[fake-send] 没找到设备。检查：PSV 客户端已启动、与 PC 同网段；"
                  "端口候选可用 --port 覆盖；也可以用 --ip 直接指定。")
            return 1
        for d in devs:
            print("[fake-send]   发现 %s:%d  alias=%s  protocol=%s"
                  % (d["ip"], d["port"], d["alias"], d["protocol"]))
        ps = [d for d in devs if d["protocol"] == "http"] or devs
        if len(ps) > 1:
            print("[fake-send] 找到多台，请用 --ip 指定其中一台")
            return 1
        ip, port = ps[0]["ip"], ps[0]["port"]
        print("[fake-send] 使用 %s:%d" % (ip, port))

    # ---- 1. 准备文件与清单 ----
    payloads, metas = [], {}
    stamp = int(time.time()) % 100000
    for i in range(args.count):
        data = make_payload(i + 1, args.size)
        payloads.append(data)
        if args.no_sha:
            sha = None
        elif (i + 1) in args.bad_sha:
            sha = "00" * 32            # 错的校验和：内容对不上 → PSV 应回 422
        else:
            sha = hashlib.sha256(data).hexdigest()
        metas["f%d" % i] = {
            "id": "f%d" % i,
            "fileName": layout_name(args.layout, i, stamp),
            "size": len(data),
            "fileType": "application/octet-stream",
            "sha256": sha,
        }
    for i in range(args.count):
        tag = " (BAD SHA)" if (i + 1) in args.bad_sha else \
              (" (SHORT)" if (i + 1) in args.short else "")
        print("[fake-send]   f%d: [%s] %s %d B%s"
              % (i, args.layout, metas["f%d" % i]["fileName"], args.size, tag))

    doc = {
        "info": {
            "alias": "FAKE-SEND",
            "version": "2.0",
            "deviceModel": "fake-send",
            "deviceType": "desktop",
            "fingerprint": "bb" * 32,
            "port": port,
            "protocol": "http",
            "download": False,
        },
        "files": metas,
    }
    if args.manifest_pad:
        doc["pad"] = "x" * args.manifest_pad   # 测 4MB 清单上限用，接收侧忽略未知字段
    body = json.dumps(doc).encode()

    # ---- 1.5 M3 prepare 边界用例（在正常流程之前短路） ----
    if args.prepare_concurrent:
        def one(_i):
            try:
                return post(ip, port, API + "/prepare-upload", body,
                            "application/json", timeout=args.prepare_wait)[0]
            except Exception as e:
                return "wait/%s" % type(e).__name__   # 超时=赢得等待（等 UI 决定）

        with concurrent.futures.ThreadPoolExecutor(args.prepare_concurrent) as ex:
            got = list(ex.map(one, range(args.prepare_concurrent)))
        print("[fake-send] 并发 prepare ×%d → %s" % (args.prepare_concurrent, got))
        print("[fake-send] 期望：1 份进入等待（wait/...），其余 409")
        return 0

    if args.prepare_short:
        st, data = raw_exchange(
            ip, port,
            head_of(ip, port, ("Content-Length: %d\r\n" % len(body)).encode()),
            body, half=True)
        print("[fake-send] 短 body prepare → HTTP %s %s"
              % (st, data[:80].decode(errors="replace")))
        return 0 if st == 400 else 1

    if args.prepare_nocl:
        st, data = raw_exchange(ip, port, head_of(ip, port), body)
        print("[fake-send] 无 Content-Length prepare → HTTP %s %s"
              % (st, data[:160].decode(errors="replace")))
        return 0 if st in (200, 403, 409) else 1

    if args.prepare_chunked:
        chunked = b"%x\r\n" % len(body) + body + b"\r\n0\r\n\r\n"
        st, data = raw_exchange(
            ip, port,
            head_of(ip, port, b"Transfer-Encoding: chunked\r\n"), chunked)
        print("[fake-send] chunked prepare → HTTP %s %s"
              % (st, data[:80].decode(errors="replace")))
        return 0 if st == 411 else 1

    print("[fake-send] prepare-upload → %s:%d（请在 PSV 上点 Accept）" % (ip, port))
    sys.stdout.flush()
    try:
        code, rsp = post(ip, port, API + "/prepare-upload",
                         body, "application/json", timeout=90)
    except Exception as e:
        print("[fake-send] prepare 失败：%s" % e)
        return 1
    print("[fake-send] prepare <- HTTP %d %s" % (code, rsp[:200].decode(errors="replace")))
    if code != 200:
        print("[fake-send] 对端未接受（403=拒绝/超时，409=正忙，413=超限/过大），结束")
        return 1
    if args.prepare_only:
        print("[fake-send] --prepare-only：已拿到回执，不再上传")
        return 0

    try:
        res = json.loads(rsp)
        sid = res["sessionId"]
        tokens = res.get("files", {})
    except Exception as e:
        print("[fake-send] 回执解析失败：%s" % e)
        return 1

    # ---- 2. 逐文件上传 ----
    results = {}
    lock = threading.Lock()

    def upload(i):
        fid = "f%d" % i
        tok = tokens.get(fid)
        if not tok:
            print("[fake-send] %s：回执里没有 token（被跳过）" % fid)
            return
        path = "%s/upload?sessionId=%s&fileId=%s&token=%s" % (API, sid, fid, tok)
        try:
            if (i + 1) in args.short:
                r = post_half(ip, port, path, payloads[i])
                code, info = (None, "中途断连")
            else:
                code, rsp2 = post(ip, port, path, payloads[i])
                info = rsp2[:120].decode(errors="replace")
        except Exception as e:
            code, info = (None, str(e))
        with lock:
            results[fid] = code
            print("[fake-send] upload %s ← %s %s"
                  % (fid, "断连" if code is None else "HTTP %d" % code, info))

    if args.parallel > 1:
        with concurrent.futures.ThreadPoolExecutor(args.parallel) as ex:
            list(ex.map(upload, range(args.count)))
    else:
        for i in range(args.count):
            upload(i)

    print("[fake-send] 完成：%s" % ", ".join(
        "%s=%s" % (k, "断连" if v is None else v) for k, v in sorted(results.items())))
    print("[fake-send] PSV 端期望：--bad-sha → 该文件标红 + 终态 Complete, N failed；"
          "--short → 整批传输中断")
    return 0


if __name__ == "__main__":
    sys.exit(main())
