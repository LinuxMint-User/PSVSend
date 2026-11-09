#!/usr/bin/env python3
"""gen-emoji-font.py —— 由 Noto Emoji 生成 PSVSend 用的单色 emoji 字体。

为什么要它：libvita2d 的 UTF-8 解码（utf8_to_ucs2）只认 1~3 字节，4 字节码位
（emoji 全在 U+FFFF 以上）会被拆成 4 个单字节，各画一个缺字方框。绕过办法是把
emoji 码位重映射到 BMP 私用区再交给它绘制：

    U+1F000..U+1FAFF  →  U+E000..U+EAFF
    PUA = 0xE000 + (cp - 0x1F000)

运行时对应 widgets.c 的 utf8_to_drawable() / cp_domain()。中文文本里真正用到
BMP 私用区的概率极低，撞了也只是路由到 emoji 字体（最多画成方框）。

输入：Noto Emoji（单色，OFL）TTF。默认从 Google Fonts 的静态地址下载，也可用
--src 指定本地文件。输出：fonts/NotoEmoji-PSV.ttf（只保留重映射后的 PUA 字形）。

用法：
  python3 scripts/gen-emoji-font.py
  python3 scripts/gen-emoji-font.py --src /path/to/NotoEmoji.ttf
"""

import argparse
import os
import sys
import urllib.request

from fontTools import subset
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable

CP_MIN = 0x1F000          # emoji 起始（Mahjong Tiles）
CP_MAX = 0x1FAFF          # emoji 末尾（Symbols and Pictographs Extended-A）
PUA_BASE = 0xE000         # 目标私用区起点（BMP，3 字节 UTF-8）
SRC_URL = ("https://fonts.gstatic.com/s/notoemoji/v47/"
           "bMrnmSyK7YY-MEu6aWjPDs-ar6uWaGWuob-r0jwvS-FGJCMY.ttf")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "fonts", "NotoEmoji-PSV.ttf")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", help="本地 NotoEmoji TTF；省略则从 Google Fonts 下载")
    ap.add_argument("--out", default=OUT)
    args = ap.parse_args()

    src = args.src
    if not src:
        src = os.path.join("/tmp", "NotoEmoji.ttf")
        if not os.path.exists(src):
            print("[emoji] 下载 %s" % SRC_URL)
            urllib.request.urlretrieve(SRC_URL, src)
        print("[emoji] 使用 %s" % src)

    font = TTFont(src)
    cmap = font.getBestCmap()
    glyphs = []
    pua_map = {}
    for cp in sorted(cmap):
        if CP_MIN <= cp <= CP_MAX:
            g = cmap[cp]
            pua = PUA_BASE + (cp - CP_MIN)
            pua_map[pua] = g
            glyphs.append(g)
    if not pua_map:
        sys.exit("[emoji] 源字体里没有 U+1F000..U+1FAFF 的字形")

    # 只留用得到的字形（--glyphs 按字形名裁），再整体换掉 cmap
    opts = subset.Options()
    opts.glyph_names = True
    opts.notdef_outline = True
    opts.recalc_bounds = False
    opts.drop_tables += ["GSUB", "GPOS", "GDEF", "vhea", "vmtx", "STAT"]
    sub = subset.Subsetter(options=opts)
    sub.populate(glyphs=glyphs)
    sub.subset(font)

    keep = set(font.getGlyphOrder())
    pua_map = {p: g for p, g in pua_map.items() if g in keep}

    cm = newTable("cmap")
    cm.tableVersion = 0
    st = CmapSubtable.newSubtable(4)      # BMP 格式 4，够装下整个私用区
    st.platformID = 3                     # Windows
    st.platEncID = 1                      # Unicode BMP
    st.language = 0
    st.cmap = pua_map
    cm.tables = [st]
    font["cmap"] = cm

    out = os.path.normpath(args.out)
    font.save(out)
    print("[emoji] %d 个 emoji 字形 → %s (%d 字节)"
          % (len(pua_map), out, os.path.getsize(out)))


if __name__ == "__main__":
    main()
