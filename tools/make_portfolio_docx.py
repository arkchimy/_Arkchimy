# 포트폴리오 Word 문서 생성기 - README 초안(2026-11-10)과 같은 내용을 Word로 만든다
# 필요: python-docx, pillow (pip install --user python-docx pillow)
# 실행: python tools/make_portfolio_docx.py  → portfolio/IOCP_네트워크_라이브러리_포트폴리오.docx
import os
import re

from docx import Document
from docx.enum.section import WD_ORIENT
from docx.enum.table import WD_TABLE_ALIGNMENT
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.opc.constants import RELATIONSHIP_TYPE as RT
from docx.oxml import OxmlElement
from docx.oxml.ns import qn
from docx.shared import Cm, Inches, Pt, RGBColor
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "portfolio")
IMG_DIR = os.path.join(OUT_DIR, "images")
OUT_DOCX = os.path.join(OUT_DIR, "IOCP_네트워크_라이브러리_포트폴리오.docx")
REPO = "https://github.com/arkchimy/_Arkchimy/blob/main/"

FONT = "맑은 고딕"
FONT_LATIN = "Malgun Gothic"
CODE_FONT = "Consolas"
ACCENT = RGBColor(0x1F, 0x38, 0x64)
HEADER_FILL = "DCE6F2"


# ---------------------------------------------------------------- 구조 그림
def draw_architecture(path):
    scale = 2
    w, h = 900 * scale, 420 * scale
    img = Image.new("RGB", (w, h), "white")
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype("C:/Windows/Fonts/malgun.ttf", 15 * scale)
    fb = ImageFont.truetype("C:/Windows/Fonts/malgunbd.ttf", 15 * scale)
    fs = ImageFont.truetype("C:/Windows/Fonts/malgun.ttf", 12 * scale)

    def s(v):
        return int(v * scale)

    def box(x, y, bw, bh, lines, fill, bold=False):
        d.rounded_rectangle([s(x), s(y), s(x + bw), s(y + bh)], radius=s(8), fill=fill, outline=(70, 90, 120), width=s(1.5))
        font = fb if bold else f
        total = len(lines) * s(20)
        ty = s(y + bh / 2) - total // 2
        for line in lines:
            tw = d.textlength(line, font=font)
            d.text((s(x + bw / 2) - tw / 2, ty), line, fill=(20, 30, 45), font=font)
            ty += s(20)

    def arrow(x1, y1, x2, y2, label=None, dashed=False, lx=None, ly=None):
        color = (60, 75, 100)
        if dashed:
            n = 12
            for i in range(n):
                if i % 2 == 0:
                    ax = x1 + (x2 - x1) * i / n
                    ay = y1 + (y2 - y1) * i / n
                    bx = x1 + (x2 - x1) * (i + 1) / n
                    by = y1 + (y2 - y1) * (i + 1) / n
                    d.line([s(ax), s(ay), s(bx), s(by)], fill=color, width=s(2))
        else:
            d.line([s(x1), s(y1), s(x2), s(y2)], fill=color, width=s(2))
        # 화살촉
        import math
        ang = math.atan2(y2 - y1, x2 - x1)
        a1 = (x2 - 10 * math.cos(ang - 0.4), y2 - 10 * math.sin(ang - 0.4))
        a2 = (x2 - 10 * math.cos(ang + 0.4), y2 - 10 * math.sin(ang + 0.4))
        d.polygon([(s(x2), s(y2)), (s(a1[0]), s(a1[1])), (s(a2[0]), s(a2[1]))], fill=color)
        if label:
            tx = lx if lx is not None else (x1 + x2) / 2
            ty = ly if ly is not None else (y1 + y2) / 2 - 18
            tw = d.textlength(label, font=fs)
            d.text((s(tx) - tw / 2, s(ty)), label, fill=(80, 90, 110), font=fs)

    lib = (232, 239, 250)
    content = (255, 244, 225)
    other = (236, 244, 236)
    # 라이브러리 영역 표시
    d.rounded_rectangle([s(160), s(118), s(890), s(410)], radius=s(12), outline=(150, 165, 190), width=s(1))
    d.text((s(790), s(390)), "NetworkLib.dll", fill=(110, 125, 150), font=fs)

    box(10, 170, 120, 200, ["클라이언트"], (245, 245, 245), bold=True)
    box(175, 160, 140, 64, ["IOCP 완료 큐"], lib)
    box(355, 160, 140, 64, ["워커 스레드"], lib, bold=True)
    box(535, 160, 160, 64, ["수신 링", "패킷 자르기"], lib)
    box(735, 160, 145, 64, ["컨텐츠", "OnRecv"], content)
    box(705, 310, 175, 72, ["세션별 송신 링", "+ 보낼 목록"], lib)
    box(425, 300, 210, 92, ["WSASend", "꺼낸 완료를 다 처리한 뒤", "세션마다 1번"], lib)
    box(160, 18, 170, 64, ["워커 밖 스레드", "DB 작업 등"], other)
    box(380, 18, 150, 64, ["송신 정체 감시"], other)

    arrow(130, 192, 175, 192, "TCP", ly=170)
    arrow(315, 192, 355, 192)
    arrow(495, 192, 535, 192)
    arrow(695, 192, 735, 192)
    arrow(807, 224, 807, 310, "SendPacket", lx=850, ly=255)
    arrow(705, 346, 635, 346)
    arrow(425, 346, 130, 346, "TCP", ly=325)
    arrow(245, 82, 245, 160, "송신 요청, 작업 완료", lx=315, ly=94)
    arrow(455, 82, 435, 160, "끊기 요청", dashed=True, lx=495, ly=94)
    img.save(path)


# ---------------------------------------------------------------- 문서 도우미
def set_run_font(run, name=FONT, latin=FONT_LATIN, size=None, bold=None, color=None):
    run.font.name = latin
    rpr = run._element.get_or_add_rPr()
    rfonts = rpr.find(qn("w:rFonts"))
    if rfonts is None:
        rfonts = OxmlElement("w:rFonts")
        rpr.insert(0, rfonts)
    rfonts.set(qn("w:eastAsia"), name)
    rfonts.set(qn("w:ascii"), latin)
    rfonts.set(qn("w:hAnsi"), latin)
    if size is not None:
        run.font.size = Pt(size)
    if bold is not None:
        run.font.bold = bold
    if color is not None:
        run.font.color.rgb = color


def style_font(style, size, bold=False, color=None):
    style.font.name = FONT_LATIN
    style.font.size = Pt(size)
    style.font.bold = bold
    if color is not None:
        style.font.color.rgb = color
    rpr = style.element.get_or_add_rPr()
    rfonts = rpr.find(qn("w:rFonts"))
    if rfonts is None:
        rfonts = OxmlElement("w:rFonts")
        rpr.insert(0, rfonts)
    rfonts.set(qn("w:eastAsia"), FONT)
    rfonts.set(qn("w:ascii"), FONT_LATIN)
    rfonts.set(qn("w:hAnsi"), FONT_LATIN)


TOKEN = re.compile(r"(\*\*.+?\*\*|`.+?`)")


def add_rich(paragraph, text, size=None, color=None):
    # **굵게**, `코드`만 해석한다
    for part in TOKEN.split(text):
        if not part:
            continue
        if part.startswith("**") and part.endswith("**"):
            # 굵은 글씨 안의 `코드`도 코드 글꼴로
            for sub in re.split(r"(`.+?`)", part[2:-2]):
                if not sub:
                    continue
                if sub.startswith("`") and sub.endswith("`"):
                    r = paragraph.add_run(sub[1:-1])
                    set_run_font(r, name=CODE_FONT, latin=CODE_FONT, size=(size or 10) - 0.5, bold=True, color=color)
                else:
                    r = paragraph.add_run(sub)
                    set_run_font(r, size=size, bold=True, color=color)
        elif part.startswith("`") and part.endswith("`"):
            r = paragraph.add_run(part[1:-1])
            set_run_font(r, name=CODE_FONT, latin=CODE_FONT, size=(size or 10) - 0.5, color=color)
        else:
            r = paragraph.add_run(part)
            set_run_font(r, size=size, color=color)
    return paragraph


def add_link(paragraph, text, path):
    url = REPO + path
    rid = paragraph.part.relate_to(url, RT.HYPERLINK, is_external=True)
    link = OxmlElement("w:hyperlink")
    link.set(qn("r:id"), rid)
    r = OxmlElement("w:r")
    rpr = OxmlElement("w:rPr")
    rfonts = OxmlElement("w:rFonts")
    rfonts.set(qn("w:eastAsia"), FONT)
    rfonts.set(qn("w:ascii"), FONT_LATIN)
    rfonts.set(qn("w:hAnsi"), FONT_LATIN)
    rpr.append(rfonts)
    color = OxmlElement("w:color")
    color.set(qn("w:val"), "0563C1")
    rpr.append(color)
    u = OxmlElement("w:u")
    u.set(qn("w:val"), "single")
    rpr.append(u)
    r.append(rpr)
    t = OxmlElement("w:t")
    t.text = text
    t.set(qn("xml:space"), "preserve")
    r.append(t)
    link.append(r)
    paragraph._p.append(link)


def para(doc, text="", style=None, after=4, size=None):
    p = doc.add_paragraph(style=style)
    p.paragraph_format.space_after = Pt(after)
    if text:
        add_rich(p, text, size=size)
    return p


def bullet(doc, text):
    p = doc.add_paragraph(style="List Bullet")
    p.paragraph_format.space_after = Pt(2)
    add_rich(p, text)
    return p


def links(doc, label, items):
    # "자세히: 문서1 · 문서2" 한 줄
    p = doc.add_paragraph()
    p.paragraph_format.space_after = Pt(6)
    r = p.add_run(label)
    set_run_font(r, size=9, color=RGBColor(0x59, 0x59, 0x59))
    for i, (text, path) in enumerate(items):
        if i > 0:
            r = p.add_run(" · ")
            set_run_font(r, size=9)
        add_link(p, text, path)
    return p


def shade(cell, fill):
    tcpr = cell._element.get_or_add_tcPr()
    shd = OxmlElement("w:shd")
    shd.set(qn("w:val"), "clear")
    shd.set(qn("w:color"), "auto")
    shd.set(qn("w:fill"), fill)
    tcpr.append(shd)


def table(doc, header, rows, widths, size=9):
    t = doc.add_table(rows=1 + len(rows), cols=len(header))
    t.style = "Table Grid"
    t.alignment = WD_TABLE_ALIGNMENT.CENTER
    t.autofit = False
    for row_idx, values in enumerate([header] + rows):
        for col_idx, value in enumerate(values):
            cell = t.rows[row_idx].cells[col_idx]
            cell.width = widths[col_idx]
            p = cell.paragraphs[0]
            p.paragraph_format.space_after = Pt(0)
            if row_idx == 0:
                r = p.add_run(value)
                set_run_font(r, size=size, bold=True)
                shade(cell, HEADER_FILL)
            else:
                add_rich(p, value, size=size)
    # 표 아래 간격
    doc.add_paragraph().paragraph_format.space_after = Pt(2)
    return t


def add_page_number(section):
    p = section.footer.paragraphs[0]
    p.alignment = WD_ALIGN_PARAGRAPH.CENTER
    fld = OxmlElement("w:fldSimple")
    fld.set(qn("w:instr"), "PAGE")
    r = OxmlElement("w:r")
    t = OxmlElement("w:t")
    t.text = "1"
    r.append(t)
    fld.append(r)
    p._p.append(fld)


# ---------------------------------------------------------------- 본문
def build():
    os.makedirs(IMG_DIR, exist_ok=True)
    arch_png = os.path.join(IMG_DIR, "architecture.png")
    draw_architecture(arch_png)

    doc = Document()
    sec = doc.sections[0]
    sec.orientation = WD_ORIENT.PORTRAIT
    sec.page_width, sec.page_height = Cm(21.0), Cm(29.7)
    for side in ("left_margin", "right_margin"):
        setattr(sec, side, Cm(2.0))
    sec.top_margin, sec.bottom_margin = Cm(1.8), Cm(1.8)
    add_page_number(sec)

    styles = doc.styles
    style_font(styles["Normal"], 10)
    styles["Normal"].paragraph_format.line_spacing = 1.2
    style_font(styles["Heading 1"], 15, bold=True, color=ACCENT)
    style_font(styles["Heading 2"], 12, bold=True, color=ACCENT)
    for name, before, after in (("Heading 1", 16, 6), ("Heading 2", 10, 4)):
        pf = styles[name].paragraph_format
        pf.space_before, pf.space_after, pf.keep_with_next = Pt(before), Pt(after), True
    style_font(styles["List Bullet"], 10)

    content_w = Cm(17.0)

    # 제목
    p = doc.add_paragraph()
    p.paragraph_format.space_after = Pt(2)
    r = p.add_run("Windows IOCP 네트워크 라이브러리")
    set_run_font(r, size=22, bold=True, color=ACCENT)
    p = doc.add_paragraph()
    p.paragraph_format.space_after = Pt(2)
    r = p.add_run("게임 서버용 TCP 네트워크 라이브러리 설계 포트폴리오")
    set_run_font(r, size=12, color=RGBColor(0x40, 0x40, 0x40))
    p = doc.add_paragraph()
    p.paragraph_format.space_after = Pt(10)
    r = p.add_run("C++ · Windows · IOCP · Winsock2 · Visual Studio 2022    저장소: ")
    set_run_font(r, size=9, color=RGBColor(0x59, 0x59, 0x59))
    add_link(p, "github.com/arkchimy/_Arkchimy", "README.md")
    p._p.get_or_add_pPr()
    bdr = OxmlElement("w:pBdr")
    bottom = OxmlElement("w:bottom")
    for k, v in (("w:val", "single"), ("w:sz", "8"), ("w:space", "4"), ("w:color", "1F3864")):
        bottom.set(qn(k), v)
    bdr.append(bottom)
    p._p.pPr.append(bdr)

    # 1. 개요
    doc.add_heading("1. 개요", level=1)
    para(doc, "**게임 서버용 TCP 네트워크 라이브러리를 C++로 직접 설계했습니다. 주요 설계 결정마다 왜 그렇게 정했는지, 어떤 안을 버렸는지, 그 판단이 맞는지 어떻게 확인했는지를 함께 남겼습니다.**")
    para(doc, "IOCP와 `AcceptEx` 기반의 서버 라이브러리를 DLL로 만들고, 그 위에 에코·채팅 서버와 검증·부하 클라이언트를 구현했습니다. 방 브로드캐스트처럼 메시지가 몰리는 상황에서 \"빠르다\"보다 **숫자가 왜 그렇게 나왔는지 설명할 수 있는 설계**를 목표로 삼았습니다.")

    # 2. 구조
    doc.add_heading("2. 구조", level=1)
    doc.add_picture(arch_png, width=Cm(16.5))
    doc.paragraphs[-1].alignment = WD_ALIGN_PARAGRAPH.CENTER
    bullet(doc, "**스레드:** 워커가 완료와 컨텐츠 콜백을 직접 처리합니다. 컨텐츠가 만든 다른 스레드(예: DB 작업 스레드)도 `SendPacket`과 작업 완료를 부를 수 있지만, 실제 `WSASend`와 세션 해제는 IOCP를 거쳐 항상 워커가 합니다.")
    bullet(doc, "**세션:** 시작할 때 최대 수만큼 만들어 재사용하고, 컨텐츠는 포인터가 아니라 ID로만 다룹니다. 수명은 I/O 카운트 하나로 관리합니다.")
    bullet(doc, "**컨텐츠 경계:** 컨텐츠는 라이브러리를 상속해 `OnAccept`, `OnRecv`, `OnRelease`를 구현합니다. 워커 밖 스레드(DB 등)가 세션을 쓰는 동안에는 `HoldSession`/`UnholdSession`으로 해제를 막을 수 있고, DB 처리 자체는 컨텐츠가 맡습니다. 방 정책이나 게임 규칙은 라이브러리가 모릅니다.")

    # 3. 설계 결정
    doc.add_heading("3. 설계 결정", level=1)
    para(doc, "영역마다 고른 방식, 버린 안, 그렇게 정한 이유입니다.")
    rows = [
        ["연결 수락", "`AcceptEx`(미리 걸어 둔 소켓)", "`accept`", "워커 2개에서 거절 0.006%로 `accept`(0.038%)의 1/6, 처리량 1.24배. 워커 1개에서는 미리 걸어 둔 소켓이 모자라 오히려 거절이 9배 많았음"],
        ["완료 꺼내기", "GQCSEx, 최대 64개씩", "GQCS 1개씩", "한 번에 꺼낸 완료들이 모아 보내는 범위가 됨"],
        ["워커 수", "워커 수와 IOCP 동시 실행 수를 따로 설정 (기본 2)", "워커를 코어 수만큼 두고 모두 깨움", "동시에 깨어 있는 워커가 많으면 한 번에 꺼내는 완료가 줄어 덜 모아 보냄. 워커 4: 884만 → 동시 실행 수 2로 묶자 1048만/s"],
        ["컨텐츠 실행 스레드", "패킷을 받은 IOCP 워커가 `OnRecv`를 바로 호출", "패킷을 큐로 넘겨 로직 스레드 하나가 처리", "공유 메시지 큐의 락 경합·복사·스레드 깨우기가 없음"],
        ["세션 수명", "I/O 카운트 + 해제 표시 비트", "세션 락", "해제를 한 번만 하는 것은 락으로도 되지만 송신·세션 찾기마다 락을 잡아야 하고, 아무도 안 쓰는 순간은 결국 카운트로 알아야 함. 카운트 하나로 락 없이 처리하고 해제는 항상 워커에서"],
        ["송신 버퍼", "세션별 고정 송신 링 32KB", "직렬화 버퍼 공유", "암호화는 컨텐츠가 세션마다 다른 키로 하므로, 같은 메시지도 받는 세션마다 바이트가 달라짐"],
        ["송신 시점", "꺼낸 완료를 다 처리한 뒤 세션마다 1번", "즉시 송신", "방 16 전달 처리량 12.7–12.9배"],
        ["무응답 클라", "송신 정체·수신 타임아웃", "TCP에 맡김", "수신 윈도우가 0이면 TCP가 끊지 않음"],
        ["배포", "DLL + 공개·내부 헤더 분리", "정적 라이브러리", "함수 내부·세션 구조·링 크기만 바뀌면 DLL만 교체. `NetworkLib` 클래스의 멤버·가상 함수가 바뀌면 서버도 다시 빌드"],
    ]
    table(doc, ["영역", "선택", "버린 안", "이유"], rows, [Cm(2.6), Cm(4.4), Cm(2.8), Cm(7.2)])
    bullet(doc, "**컨텐츠 실행 스레드의 대가:** 서로 다른 세션의 콜백이 여러 워커에서 동시에 불리므로 공유 데이터는 컨텐츠가 락으로 보호합니다. 같은 세션의 `OnRecv`는 순서대로 불리고 동시에 불리지 않습니다.")
    bullet(doc, "**송신 버퍼:** 라이브러리는 받은 바이트를 그대로 전송만 하고, 암호화는 컨텐츠가 세션마다 다른 키로 맡습니다. 그러면 같은 메시지라도 받는 세션마다 보내는 바이트가 달라져, 한 번 직렬화한 버퍼를 여러 세션이 공유할 수 없습니다. 그래서 공유 버퍼 대신 세션마다 고정 크기 송신 링을 두고, 컨텐츠가 세션별로 만든 바이트를 각자의 링에 받는 구조로 설계했습니다.")

    doc.add_heading("3-1. 송신 시점: 언제 WSASend를 부르는가", level=2)
    para(doc, "**`SendPacket`은 소켓에 바로 보내지 않고 받는 세션의 송신 링에 쌓기만 하며, `WSASend`는 모아서 부릅니다.** `WSASend`는 시스템 콜이라 한 번에 약 7us가 들고, 이 비용은 한 번에 실린 메시지 수와 거의 상관없습니다. 그래서 메시지 1건의 송신 비용은 \"`WSASend` 1번 비용 ÷ 한 번에 실린 메시지 수\"가 되고, 언제 부르느냐가 처리량을 정합니다.")
    para(doc, "")
    para(doc, "**부하 패턴:** 부하 클라이언트는 256세션(방 16명씩 16개)이 각자 32바이트 메시지 16개를 `send` 한 번으로 보내고, 자기 16개가 방송으로 모두 돌아오면 곧바로 다음 16개를 보냅니다. 속도 제한 없이 서버가 처리하는 만큼 계속 보내는 포화 부하입니다.")
    para(doc, "")
    para(doc, "세 가지 시점을 차례로 비교했습니다.")
    table(doc, ["방식", "WSASend를 부르는 때"], [
        ["메시지마다", "`SendPacket`을 부를 때마다 그 자리에서"],
        ["수신 1건마다", "수신 버퍼에서 꺼낸 패킷을 모두 처리한 뒤, 보낼 데이터가 쌓인 세션마다 1번"],
        ["꺼낸 완료마다 (지금)", "GQCSEx로 한 번에 꺼낸 완료(최대 64개)를 모두 처리한 뒤, 세션마다 1번"],
    ], [Cm(4.0), Cm(13.0)])
    para(doc, "")

    para(doc, "**① 메시지마다 → 수신 1건마다:** 메시지를 하나 처리할 때마다 보내던 것을, 수신 버퍼에서 꺼낸 메시지를 다 처리한 뒤 한 번 보내도록 바꿨습니다.", after=2)
    table(doc, ["항목", "메시지마다", "수신 1건마다"], [
        ["`WSASend` 1번에 실린 메시지", "1.1개", "16.4개"],
        ["전달 처리량", "30.4만/s", "387만/s (12.7배)"],
        ["전달 1건당 서버 CPU", "6.41us", "0.52us"],
        ["방 락을 잡은 시간", "94us", "0.91us"],
    ], [Cm(6.0), Cm(5.5), Cm(5.5)])
    bullet(doc, "메시지마다 보내면 방송에서 받는 사람 수만큼 `WSASend`를 부릅니다. 그것도 방 락을 쥔 채 불러 락을 잡는 시간까지 길어졌습니다.")
    bullet(doc, "바꾼 뒤에는 방 락 안에서 송신 링에 넣기만 하고, `WSASend`는 락을 놓은 뒤 부릅니다.")

    para(doc, "**② 수신 1건마다 → 꺼낸 완료마다:** 모으는 범위를 수신 1건에서, 워커가 한 번에 꺼낸 완료 전체로 넓혔습니다.", after=2)
    table(doc, ["항목", "수신 1건마다", "꺼낸 완료마다"], [
        ["`WSASend` 1번에 실린 메시지", "16개", "59개"],
        ["전달 처리량", "411만/s", "993만/s (2.4배)"],
        ["메시지 왕복 지연 p99", "20.7ms", "7.6ms"],
    ], [Cm(6.0), Cm(5.5), Cm(5.5)])
    bullet(doc, "수신 1건에는 보낸 사람 한 명의 메시지만 들어 있어, 받는 사람마다 16개씩만 모였습니다. 범위를 꺼낸 완료 전체로 넓히면 여러 보낸 사람의 메시지가 받는 사람마다 한 번에 합쳐집니다.")
    bullet(doc, "타이머로 기다리지 않습니다. 한가하면 꺼낸 완료가 적어 곧바로 나가고, 바쁘면 그만큼 많이 모입니다. 그래서 처리량이 오르면서 지연도 줄었습니다.")
    para(doc, "측정 조건: PC B(Core i5-8600), 루프백, 채팅 256세션·방 16, 워커 2, 완료 통지 skip 켬, 조건별 3회 중앙값. ①과 ②는 서로 다른 커밋에서 잰 값이라 표 안에서만 비교합니다.", size=9)

    # 4. 고민
    doc.add_heading("4. 가장 오래 고민한 것", level=1)
    doc.add_heading("4-1. 송신 경로: 우연에 기대던 모아 보내기를 설계로", level=2)
    para(doc, "완료 통지를 줄이려고 skip(`FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`)을 켜자 처리량이 77.9만 → 30.2만 패킷/s로 61% 떨어졌습니다. 패킷마다 원래 `WSASend`를 1번 부르는 4KB 대조 조건은 차이가 없어, 원인이 완료 통지가 아니라 **송신 완료가 늦게 처리되는 동안 우연히 일어나던 모아 보내기가 사라진 것**임을 분리했습니다.")
    para(doc, "수신 처리 끝에서 한 번만 보내도록 바꿔 에코 처리량이 57–58% 올랐고, 받는 사람마다 모으도록 넓혀 방 16 전달 처리량이 12.7–12.9배가 됐습니다.")

    doc.add_heading("4-2. 세션 수명: 해제는 정확히 한 번, 그리고 항상 워커에서", level=2)
    para(doc, "해제는 IOCP에 작업으로 넣어 항상 워커에서 하는데, 넣기(`PostQueuedCompletionStatus`)가 실패한 분기만 그 자리에서 해제했습니다. 이 분기는 방 락을 쥔 채 `OnRelease`가 같은 락을 다시 잡는 경로였습니다. 실패를 강제해 재현했고(20회 중 4회 서버 종료), 복구할 방법이 없는 실패라 그 자리 해제를 없애고 로그를 남긴 뒤 멈추도록 바꿨습니다.")
    para(doc, "그 뒤 카운트의 63번 비트를 해제 표시로 써서 잠금 없이 해제를 한 번만 일어나게 했습니다. 리뷰에서 네 판이 실행 순서 시나리오로 깨진 끝에 **\"표시는 수락에서 새 ID를 쓴 뒤에만 지운다\"**는 결론을 얻었습니다.")

    doc.add_heading("4-3. 무응답 클라: 정상 클라를 잘못 끊지 않는 판정", level=2)
    para(doc, "상대의 수신 윈도우가 0이면 TCP는 연결을 끊지 않습니다. 그래서 라이브러리는 송신 요청 시각(`WSASend`를 건 시각)이 기준보다 오래된 연결을 송신 정체로 끊고, 컨텐츠는 하트비트로 수신 타임아웃을 판정하도록 나눴습니다.")
    para(doc, "어려웠던 것은 끊는 것보다 **잘못 끊지 않는 것**이었습니다. 부호 없는 시각 뺄셈이 활발한 세션을 끊었고, 서버가 밀린 동안 하트비트를 보내던 클라가 끊겼고, 워커에 맡긴 송신의 큐 대기를 송신 정체로 세기도 했습니다. 시각 뺄셈과 큐 대기 오판은 재현한 뒤 고쳤고, 하트비트 클라가 끊긴 문제는 송신을 모으는 범위의 기본값을 바꾼 뒤 사라졌습니다(원인은 확정하지 못함). 지금 구조에서는 서버 CPU가 포화된 상태에서도 정상 클라 끊김 0을 확인했습니다.")

    # 5. 검증
    doc.add_heading("5. 검증", level=1)
    para(doc, "에코 서버와 시험 클라이언트로 라이브러리가 깨질 수 있는 지점을 하나씩 시험했습니다. 아래 케이스는 모두 확인했습니다.")
    rows = [
        ["회신 메시지의 순서·내용 검증", "서버는 여러 응답을 모아 `WSASend` 한 번으로 보낸다. 모으는 과정에서 순서가 바뀌거나 내용이 섞이지 않는지 확인한다"],
        ["송신을 멈춘 뒤 오지 않는 회신 검사", "세션마다 송신 플래그로 `WSASend`를 하나씩만 건다. 플래그를 잘못 다루면 링에 보낼 데이터가 남아 있는데도 아무도 `WSASend`를 걸지 않는다. 요청을 멈추고 기다려 이 현상이 일어나는지 확인한다"],
        ["비정상 길이 헤더 차단", "헤더의 길이를 최대보다 크게 조작한 패킷이 오면, 라이브러리가 그 데이터를 기다리지 않고 즉시 끊는지 확인한다"],
        ["송신 정체 연결 끊기", "상대가 읽지 않아 수신 윈도우가 0이 되어도 TCP는 연결을 끊지 않는다. 그런 연결이 서버 자원을 계속 잡지 않도록 라이브러리가 송신 정체 시간 기준으로 끊는지 확인한다"],
        ["클라 종료 방식(FIN·RST)별 세션 해제", "끊김은 FIN이면 0바이트 수신 완료로, RST면 실패한 I/O 완료로 온다. 경로마다 I/O 카운트를 정확히 내려야 한다 - 놓치면 세션 슬롯이 돌아오지 않고, 두 번 내리면 사용 중인 세션이 해제된다"],
        ["서버 종료 시 정리", "종료할 때 세션이 0으로 돌아오고, 워커·감시 스레드가 남은 일 없이 멈추는지 확인한다"],
    ]
    table(doc, ["테스트 케이스", "실험한 이유"], rows, [Cm(5.0), Cm(12.0)])
    para(doc, "")
    para(doc, "**조건과 결과** (Core Ultra 5 125U, 서버·클라 같은 PC 루프백, Release x64, 2026-11-12). 장시간 시험은 정상 세션 576 + 비정상 헤더 세션 64로 4시간, 서버 기본 설정입니다.")
    rows = [
        ["회신 메시지의 순서·내용 검증", "장시간 시험", "묶음 820만 개(요청 8.2억 개) 응답 불일치 0"],
        ["송신을 멈춘 뒤 오지 않는 회신 검사", "장시간 시험 중 5분마다 일시정지", "47회 모두 2초 안에 미수신 0"],
        ["비정상 길이 헤더 차단", "장시간 시험", "보냄 = 끊김 확인 = 서버 비정상헤더 614,592"],
        ["송신 정체 연결 끊기", "정상 576 + 송신 정체 64, 커널 송신 버퍼 0, 기준 3초, 3회", "클라 확인 수 = 서버 송신정체끊김 수, 4.0–6.7초에 끊김, 정상 세션 끊김 0"],
        ["클라 종료 방식(FIN·RST)별 세션 해제", "장시간 시험", "FIN 89,785회, RST 89,483회, 수락 = 해제 794,436, 세션 0"],
        ["서버 종료 시 정리", "장시간 시험 끝에 클라 → 서버 종료", "세션 0, 정상 종료, 메모리 49MB 그대로"],
    ]
    table(doc, ["테스트 케이스", "조건", "결과"], rows, [Cm(5.0), Cm(5.5), Cm(6.5)])
    bullet(doc, "측정 자체도 의심했습니다. 클라이언트가 먼저 포화된 숫자, 프로파일러 비용이 한쪽에만 붙은 비교, 최적화가 꺼진 `/Od` 빌드를 찾아 결론까지 고쳤습니다.")

    # 6. 사용 예
    doc.add_heading("6. 사용 예: 오목 대전 서버", level=1)
    para(doc, "(작성 예정) 이 라이브러리로 로그인, 매칭, 대전, 결과 저장을 하는 1:1 오목 서버를 만듭니다. 턴 타이머 스레드의 송신, MySQL 조회를 워커 밖 스레드에서 처리하는 동안의 세션 붙잡기(`HoldSession`), 대전 중 끊김 처리를 실제로 씁니다.")

    # 7. 한계
    doc.add_heading("7. 한계", level=1)
    bullet(doc, "모두 루프백 측정이라 서버·클라이언트가 CPU와 커널 TCP 스택을 나눠 씁니다. 서버 단독 한계는 재지 못했습니다.")
    bullet(doc, "부하가 closed-loop라 포화 전 지연은 재지 못했습니다.")
    bullet(doc, "즉시 완료가 이어지는 동안 송신이 꺼낸 완료를 다 처리할 때까지 밀리는 문제를 알고 있으며, 지금은 고치지 않기로 했습니다.")

    # 8. 저장소
    doc.add_heading("8. 저장소 구성", level=1)
    rows = [
        ["`_lib/NetworkLib/`", "라이브러리 DLL"],
        ["`_lib/ProtocolLib/`", "서버·클라이언트 공용 패킷 헤더"],
        ["`Echo/Server/`", "에코 서버"],
        ["`Echo/Client/`", "에코 시험 클라이언트(select 스레드 1개)"],
        ["`tools/`", "에코 자동 시험, 문서·코딩 규칙 검사"],
        ["`portfolio/`", "구조, 사례, 검증, 설계 기록"],
    ]
    table(doc, ["경로", "내용"], rows, [Cm(5.0), Cm(12.0)])
    para(doc, "빌드는 Windows, Visual Studio 2022(MSVC), x64 기준이며, 빌드와 시험 명령은 저장소의 README에 있습니다.", size=9)

    doc.save(OUT_DOCX)
    return OUT_DOCX


if __name__ == "__main__":
    print(build())
