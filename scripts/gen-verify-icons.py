#!/usr/bin/env python3
"""生成验证页图标图集 fonts/verify/icons-48.png。

口径（改动前先读 docs/design.md §8「验证码口径」）：
  - 图标表 = LocalSend 的 app/lib/util/fingerprint_alphabet.dart 里的 iconAlphabet：
    共 260 项、按图标名严格字母序；**下标就是 base-260 的 digit**，故本表顺序不能动。
    采集自 localsend/localsend（v1.18.2 与 main 是同一个 blob，6182 字节；
    该字母表 2026-08-04 随 v1.18.0 引入，此后未变）。
  - 码位 = Flutter 的 packages/flutter/lib/src/material/icons.dart。
  - 字形 = Flutter 打包的 MaterialIcons-Regular.otf（Material Icons，Apache-2.0）。
    最保险的取法是从 PC 上官方 LocalSend 安装目录里拷同名字体，版本与对端一致。

输出：16 列 × 17 行，每格 48px + 四周 1px 透明边（格距 50），灰度+alpha（LA）PNG。
     libvita2d 的 PNG 加载会把 LA 自动转成 RGB_ALPHA，运行时按 1:1 绘制。

官方改字母表/图标集 → 必须重跑本脚本、替换图集并递增固件构建号，否则两端图标对不上。

用法：
  scripts/gen-verify-icons.py [--font PATH] [-o PATH]
依赖：Pillow（仅生成期需要，构建过程不依赖本脚本）。
"""
import argparse
import glob
import hashlib
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont

CELL = 48          # 单格像素（真机 1:1 绘制，不能事后缩放）
PAD = 1            # 每格四周透明边，防邻格渗色
COLS = 16
ROWS = 17          # ceil(260 / 16)
OUT_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "fonts", "verify", "icons-48.png")

# iconAlphabet 的 260 项，顺序 = digit。别排序、别去重、别改名。
ICONS = [
    ("ac_unit", 0xE037), ("accessibility", 0xE03C), ("agriculture", 0xE063), ("alarm", 0xE072),
    ("album", 0xE076), ("anchor", 0xE084), ("android", 0xE085), ("apartment", 0xE089),
    ("apple", 0xF04BE), ("architecture", 0xE090), ("attach_file", 0xE0B1), ("attach_money", 0xE0B2),
    ("audiotrack", 0xE0B6), ("back_hand", 0xF04C5), ("backpack", 0xE0C4), ("badge", 0xE0C8),
    ("bakery_dining", 0xE0C9), ("balance", 0xF04C6), ("bathtub", 0xE0CF), ("battery_full", 0xE0D2),
    ("bento", 0xE0DD), ("biotech", 0xE0DF), ("blender", 0xE0E0), ("bluetooth", 0xE0E4),
    ("bolt", 0xE0EE), ("bookmark", 0xE0F1), ("brush", 0xE113), ("bug_report", 0xE115),
    ("build", 0xE116), ("cable", 0xE11E), ("cake", 0xE120), ("calculate", 0xE121),
    ("calendar_today", 0xE122), ("campaign", 0xE138), ("casino", 0xE143), ("cast", 0xE144),
    ("castle", 0xF04CB), ("celebration", 0xE149), ("cell_tower", 0xF04CC), ("chair", 0xE14D),
    ("change_history", 0xE151), ("chat", 0xE153), ("checkroom", 0xE15D), ("church", 0xF04CD),
    ("circle", 0xE163), ("cloud", 0xE16F), ("coffee_maker", 0xE179), ("colorize", 0xE17D),
    ("computer", 0xE185), ("confirmation_number", 0xE186), ("content_cut", 0xE191), ("conveyor_belt", 0xF0858),
    ("cookie", 0xF04D9), ("coronavirus", 0xE199), ("cottage", 0xE19B), ("credit_card", 0xE19F),
    ("cruelty_free", 0xF04DA), ("cyclone", 0xF0797), ("delete", 0xE1B9), ("diamond", 0xF04ED),
    ("directions_bike", 0xE1D2), ("directions_boat", 0xE1D3), ("directions_bus", 0xE1D5), ("directions_car", 0xE1D7),
    ("door_front_door", 0xE1FC), ("earbuds", 0xE214), ("eco", 0xE217), ("edit", 0xE21A),
    ("egg", 0xF04F8), ("elevator", 0xE229), ("email", 0xE22A), ("emoji_events", 0xE22C),
    ("emoji_nature", 0xE22F), ("engineering", 0xE234), ("explore", 0xE248), ("extension", 0xE250),
    ("face", 0xE252), ("factory", 0xF04FD), ("fastfood", 0xE25A), ("favorite", 0xE25B),
    ("fence", 0xE262), ("festival", 0xE263), ("fingerprint", 0xE287), ("fire_extinguisher", 0xE288),
    ("fireplace", 0xE28A), ("fitness_center", 0xE28D), ("flag", 0xE28E), ("flashlight_on", 0xE295),
    ("flight", 0xE297), ("flutter_dash", 0xE2A0), ("forklift", 0xF0866), ("format_paint", 0xE2BB),
    ("gavel", 0xE2D3), ("grass", 0xE2E4), ("groups", 0xE2EE), ("handshake", 0xF06BE),
    ("hardware", 0xE2F3), ("headphones", 0xE2FF), ("healing", 0xE304), ("hearing", 0xE306),
    ("hexagon", 0xF0517), ("history_edu", 0xE315), ("hive", 0xF0518), ("home", 0xE318),
    ("hourglass_empty", 0xE325), ("houseboat", 0xE32A), ("hub", 0xF051D), ("ice_skating", 0xE330),
    ("icecream", 0xE331), ("inventory_2", 0xE34A), ("iron", 0xE34E), ("kebab_dining", 0xF052A),
    ("key", 0xF052B), ("keyboard", 0xE351), ("king_bed", 0xE35D), ("kitchen", 0xE35E),
    ("landscape", 0xE365), ("layers", 0xE36D), ("light", 0xE379), ("lightbulb", 0xE37B),
    ("link", 0xE380), ("liquor", 0xE383), ("local_bar", 0xE38C), ("local_cafe", 0xE38D),
    ("local_drink", 0xE391), ("local_florist", 0xE393), ("local_gas_station", 0xE394), ("local_laundry_service", 0xE398),
    ("local_mall", 0xE39A), ("local_pizza", 0xE3A0), ("local_shipping", 0xE3A6), ("local_taxi", 0xE3A7),
    ("location_city", 0xE3A8), ("lock", 0xE3AE), ("luggage", 0xE3C1), ("map", 0xE3C8),
    ("markunread_mailbox", 0xE3D2), ("masks", 0xE3D3), ("medication", 0xE3D9), ("memory", 0xE3DB),
    ("menu", 0xE3DC), ("menu_book", 0xE3DD), ("mic", 0xE3E1), ("microwave", 0xE3E6),
    ("military_tech", 0xE3E7), ("monitor_heart", 0xF053D), ("mood", 0xE3FE), ("mosque", 0xF053E),
    ("motorcycle", 0xE40A), ("mouse", 0xE40B), ("movie", 0xE40D), ("museum", 0xE414),
    ("newspaper", 0xF0541), ("nightlight", 0xE42E), ("notifications", 0xE44F), ("oil_barrel", 0xF07B7),
    ("outdoor_grill", 0xE463), ("outlet", 0xE465), ("palette", 0xE46B), ("park", 0xE478),
    ("pentagon", 0xF0546), ("person", 0xE491), ("pets", 0xE4A1), ("phishing", 0xF0548),
    ("phone", 0xE4A2), ("photo_camera", 0xE4B6), ("piano", 0xE4BE), ("pie_chart", 0xE4C3),
    ("place", 0xE4C9), ("pool", 0xE4DC), ("power", 0xE4E0), ("precision_manufacturing", 0xE4E4),
    ("print", 0xE4EA), ("propane_tank", 0xF07BA), ("psychology", 0xE4EF), ("public", 0xE4F0),
    ("push_pin", 0xE4F4), ("qr_code", 0xE4F5), ("radar", 0xE501), ("radio", 0xE502),
    ("ramen_dining", 0xE506), ("receipt", 0xE50C), ("recycling", 0xF055A), ("redeem", 0xE511),
    ("refresh", 0xE514), ("restaurant", 0xE532), ("rocket", 0xF055C), ("room_service", 0xE53D),
    ("route", 0xF0561), ("router", 0xE542), ("sailing", 0xE54D), ("satellite_alt", 0xF0562),
    ("savings", 0xE553), ("school", 0xE559), ("science", 0xE55A), ("search", 0xE567),
    ("settings", 0xE57F), ("shelves", 0xF0877), ("shield", 0xE596), ("shopping_basket", 0xE59B),
    ("shopping_cart", 0xE59C), ("shower", 0xE5A0), ("signpost", 0xF0569), ("sim_card", 0xE5B7),
    ("smart_toy", 0xE5C5), ("smartphone", 0xE5C6), ("smoking_rooms", 0xE5C8), ("soap", 0xE5D0),
    ("solar_power", 0xF07C6), ("speaker", 0xE5DB), ("sports_esports", 0xE5E8), ("sports_football", 0xE5E9),
    ("sports_golf", 0xE5EA), ("sports_hockey", 0xE5EC), ("sports_motorsports", 0xE5EF), ("sports_soccer", 0xE5F2),
    ("sports_tennis", 0xE5F3), ("square", 0xF0570), ("stadium", 0xF0572), ("stairs", 0xE5F8),
    ("star", 0xE5F9), ("store", 0xE60A), ("straighten", 0xE60E), ("stroller", 0xE612),
    ("table_restaurant", 0xF0585), ("tag", 0xE640), ("temple_buddhist", 0xF0587), ("theater_comedy", 0xE654),
    ("theaters", 0xE655), ("thermostat", 0xE656), ("thumb_up", 0xE65B), ("toggle_on", 0xE66C),
    ("toll", 0xE66D), ("tornado", 0xF07CC), ("toys", 0xE672), ("traffic", 0xE674),
    ("train", 0xE675), ("tune", 0xE683), ("umbrella", 0xE68A), ("usb", 0xE697),
    ("vaccines", 0xF0597), ("videocam", 0xE6A8), ("view_in_ar", 0xE6B4), ("visibility", 0xE6BD),
    ("voicemail", 0xE6C1), ("volcano", 0xF07D3), ("wallet", 0xF07D4), ("warehouse", 0xF05A0),
    ("warning", 0xE6CB), ("watch", 0xE6CE), ("water_drop", 0xF05A2), ("waves", 0xE6D3),
    ("wb_sunny", 0xE6D9), ("weekend", 0xE6E1), ("whatshot", 0xE6E3), ("wifi", 0xE6E7),
    ("wind_power", 0xF07DA), ("window", 0xE6F0), ("wine_bar", 0xE6F1), ("work", 0xE6F2),
]

FONT_CANDIDATES = [
    "/opt/localsend/data/flutter_assets/fonts/MaterialIcons-Regular.otf",
    "/opt/*/data/flutter_assets/fonts/MaterialIcons-Regular.otf",
    "/usr/lib/*/flutter_assets/fonts/MaterialIcons-Regular.otf",
    "~/.local/share/**/flutter_assets/fonts/MaterialIcons-Regular.otf",
    "~/Applications/**/flutter_assets/fonts/MaterialIcons-Regular.otf",
]


def find_font(explicit):
    if explicit:
        return explicit
    for pat in FONT_CANDIDATES:
        hits = sorted(glob.glob(os.path.expanduser(pat), recursive=True))
        if hits:
            return hits[0]
    sys.exit("找不到 MaterialIcons-Regular.otf；用 --font 指定"
             "（PC 上官方 LocalSend 安装目录内的 flutter_assets/fonts/ 下就有）")


def check_alphabet():
    names = [n for n, _ in ICONS]
    assert len(ICONS) == 260, "字母表条目数必须是 260（官方 iconAlphabet）"
    assert len(set(names)) == 260, "字母表有重名项"
    assert names == sorted(names), "字母表必须按名字升序（官方就是字母序）"
    assert len(set(c for _, c in ICONS)) == 260, "码位有重复"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", help="MaterialIcons-Regular.otf 路径")
    ap.add_argument("-o", "--out", default=OUT_DEFAULT)
    a = ap.parse_args()

    check_alphabet()
    font_path = find_font(a.font)
    font = ImageFont.truetype(font_path, CELL)

    # 覆盖率自检：一个字都不能缺（缺了页面会静默少一块，等于验证结果不可信）
    missing = [n for n, c in ICONS if font.getmask(chr(c)).getbbox() is None]
    if missing:
        sys.exit("字体缺这些字形，换字体再试：%s" % ", ".join(missing))

    pitch = CELL + PAD * 2
    sheet = Image.new("LA", (COLS * pitch, ROWS * pitch), (255, 0))
    white = Image.new("L", (CELL, CELL), 255)
    for i, (_, cp) in enumerate(ICONS):
        glyph = Image.new("L", (CELL, CELL), 0)
        ImageDraw.Draw(glyph).text((0, 0), chr(cp), font=font, fill=255)
        if glyph.getbbox() is None:
            sys.exit("渲染为空：%s (U+%04X)" % (ICONS[i][0], cp))
        # 贴成 LA：亮度固定 255（运行时靠 tint 上色），只有 alpha 是字形。
        # 直接把 L 贴进 LA 会得到 alpha=255 的实心方块（PIL 会把 L 当不透明），
        # 故必须显式构造 LA 再贴。
        sheet.paste(Image.merge("LA", (white, glyph)),
                    ((i % COLS) * pitch + PAD, (i // COLS) * pitch + PAD))

    # 自检：底色必须真的是透明的（否则整张图会画成实心方块，且 tint 后全黑）
    if sheet.getchannel("A").getextrema()[0] != 0:
        sys.exit("图集没有透明像素：像素粘贴方式写错了")

    out = os.path.abspath(a.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    sheet.save(out, optimize=True)
    with open(font_path, "rb") as f:
        fh = hashlib.sha256(f.read()).hexdigest()
    with open(out, "rb") as f:
        oh = hashlib.sha256(f.read()).hexdigest()
    print(json.dumps({
        "out": out,
        "size": list(sheet.size),
        "icons": len(ICONS),
        "bytes": os.path.getsize(out),
        "font": font_path,
        "font_sha256": fh,
        "png_sha256": oh,
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
