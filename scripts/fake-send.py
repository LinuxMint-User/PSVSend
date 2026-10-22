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
            "fileName": "fake-%d-%d.bin" % (i, int(time.time()) % 100000),
            "size": len(data),
            "fileType": "application/octet-stream",
            "sha256": sha,
        }
    for i in range(args.count):
        tag = " (BAD SHA)" if (i + 1) in args.bad_sha else \
              (" (SHORT)" if (i + 1) in args.short else "")
        print("[fake-send]   f%d: %s %d B%s"
              % (i, metas["f%d" % i]["fileName"], args.size, tag))

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
    body = json.dumps(doc).encode()

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
        print("[fake-send] 对端未接受（403=拒绝/超时，409=正忙），结束")
        return 1

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
