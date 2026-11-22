#include <WinSock2.h>
#include <d3d11.h>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "implot.h"
#include "MonitorLink.h"
#include "ParserLib.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

// 모니터링 서버 GUI(설계 portfolio/design/모니터링서버_설계.docx 4-5) - Dear ImGui + ImPlot, DirectX 11
// 사용: MonitorClient.exe [모니터 IP] [포트] - 키는 작업 폴더 db.cfg의 monitorKey(운영 툴이 adminKey를 읽는 것과 같다). DB에는 붙지 않는다
// 이 프로젝트는 /utf-8로 컴파일한다 - 한글 문자열 상수가 UTF-8이라 ImGui가 그대로 그린다

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* sDevice = nullptr;
static ID3D11DeviceContext* sContext = nullptr;
static IDXGISwapChain* sSwapChain = nullptr;
static ID3D11RenderTargetView* sTarget = nullptr;
static UINT sResizeWidth = 0;
static UINT sResizeHeight = 0;
static float sUiScale = 1.0f; // 화면 배율(DPI) - 픽셀로 정한 크기에 곱한다

static void createRenderTarget()
{
	ID3D11Texture2D* backBuffer = nullptr;
	sSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
	sDevice->CreateRenderTargetView(backBuffer, nullptr, &sTarget);
	backBuffer->Release();
}

static void releaseRenderTarget()
{
	if (sTarget != nullptr)
	{
		sTarget->Release();
		sTarget = nullptr;
	}
}

static bool createDevice(HWND hWnd)
{
	DXGI_SWAP_CHAIN_DESC desc{};
	desc.BufferCount = 2;
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BufferDesc.RefreshRate.Numerator = 60;
	desc.BufferDesc.RefreshRate.Denominator = 1;
	desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.OutputWindow = hWnd;
	desc.SampleDesc.Count = 1;
	desc.Windowed = TRUE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL level;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc, &sSwapChain, &sDevice, &level, &sContext);
	if (hr == DXGI_ERROR_UNSUPPORTED)
	{
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &desc, &sSwapChain, &sDevice, &level, &sContext); // 그래픽 장치가 없으면 소프트웨어
	}
	if (hr != S_OK)
	{
		return false;
	}
	createRenderTarget();
	return true;
}

static void releaseDevice()
{
	releaseRenderTarget();
	if (sSwapChain != nullptr)
	{
		sSwapChain->Release();
	}
	if (sContext != nullptr)
	{
		sContext->Release();
	}
	if (sDevice != nullptr)
	{
		sDevice->Release();
	}
}

static LRESULT WINAPI onWindowMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
	{
		return 1;
	}
	switch (msg)
	{
	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED)
		{
			sResizeWidth = LOWORD(lParam);
			sResizeHeight = HIWORD(lParam);
		}
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU)
		{
			return 0;
		}
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------- 화면

struct Series
{
	const char* Label;
	int32_t Value; // eMonValue
};

static const ImVec4 COLOR_OK(0.30f, 0.80f, 0.35f, 1.0f);
static const ImVec4 COLOR_BAD(0.95f, 0.30f, 0.30f, 1.0f);
static const ImVec4 COLOR_WARN(0.95f, 0.75f, 0.25f, 1.0f);
static const ImVec4 COLOR_GRAY(0.60f, 0.60f, 0.60f, 1.0f);

static const char* toString(const eServerState state)
{
	switch (state)
	{
	case eServerState::Online:
		return "정상";
	case eServerState::NoResponse:
		return "응답 없음";
	case eServerState::Stopped:
		return "종료";
	default:
		return "?";
	}
}

static ImVec4 stateColor(const eServerState state)
{
	return (state == eServerState::Online) ? COLOR_OK : (state == eServerState::NoResponse) ? COLOR_BAD : COLOR_GRAY;
}

static std::string formatTime(const int64_t unixSec, const char* format)
{
	const time_t t = static_cast<time_t>(unixSec);
	tm local{};
	localtime_s(&local, &t);
	char buf[32];
	strftime(buf, sizeof(buf), format, &local);
	return buf;
}

// 그래프 하나 - 행들의 같은 칸을 선으로
// 실시간(bLive)은 x축을 "몇 초 전"(-600–0)으로 고정 - 시각 축은 날짜 줄이 자리를 차지해 좁은 그래프에서 읽기 어렵다. 지난 기록은 시각 축
static void plotRows(const char* title, const std::vector<MonRateRow>& rows, const Series* series, const int32_t seriesCnt, const bool bLive, const double nowSec)
{
	const ImPlotFlags flags = (seriesCnt <= 1) ? ImPlotFlags_NoLegend : ImPlotFlags_None; // 선이 하나면 제목으로 충분
	if (!ImPlot::BeginPlot(title, ImVec2(-1, 170 * sUiScale), flags))
	{
		return;
	}
	ImPlot::SetupAxes(bLive ? "초 전" : nullptr, nullptr, ImPlotAxisFlags_None, ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit);
	if (bLive)
	{
		ImPlot::SetupAxisLimits(ImAxis_X1, -static_cast<double>(MonitorLink::LIVE_KEEP_SEC), 0.0, ImGuiCond_Always);
	}
	else
	{
		ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
		if (!rows.empty())
		{
			ImPlot::SetupAxisLimits(ImAxis_X1, static_cast<double>(rows.front().UnixSec), static_cast<double>(rows.back().UnixSec), ImGuiCond_Always);
		}
	}
	ImPlot::SetupLegend(ImPlotLocation_NorthWest, ImPlotLegendFlags_Horizontal);
	std::vector<double> xs(rows.size());
	std::vector<double> ys(rows.size());
	for (size_t i = 0; i < rows.size(); ++i)
	{
		xs[i] = bLive ? static_cast<double>(rows[i].UnixSec) - nowSec : static_cast<double>(rows[i].UnixSec);
	}
	for (int32_t s = 0; s < seriesCnt; ++s)
	{
		for (size_t i = 0; i < rows.size(); ++i)
		{
			ys[i] = rows[i].Values[series[s].Value];
		}
		ImPlot::PlotLine(series[s].Label, xs.data(), ys.data(), static_cast<int>(rows.size()));
	}
	ImPlot::EndPlot();
}

// 서버 하나의 그래프 묶음(2열)
static void drawCharts(const eServerKind kind, const std::vector<MonRateRow>& rows, const bool bLive, const double nowSec)
{
	static const Series CPU[] = { { "프로세스", MON_PROC_CPU } };
	static const Series SYS_CPU[] = { { "PC 전체", MON_SYS_CPU_PCT } };
	static const Series MEM[] = { { "사설 메모리", MON_MEM_MB } };
	static const Series SESSION[] = { { "세션", MON_SESSION } };
	static const Series ACCEPT[] = { { "수락", MON_ACCEPT_PS } };
	static const Series BYTES[] = { { "받음", MON_RECV_KB_PS }, { "보냄", MON_SEND_KB_PS } };
	static const Series PACKETS[] = { { "받음", MON_RECV_PACKET_PS }, { "보냄", MON_SEND_PACKET_PS } };
	static const Series CUTS[] = { { "넘침", MON_OVERFLOW_PS }, { "송신정체", MON_SEND_STALL_PS }, { "슬롯부족", MON_SESSION_FULL_PS },
		{ "비정상헤더", MON_BAD_HEADER_PS }, { "잘못된패킷", MON_BAD_PACKET_PS } };
	Series gauges[MON_CONTENT_CNT];
	Series rates[MON_CONTENT_CNT];
	int32_t gaugeCnt = 0;
	int32_t rateCnt = 0;
	for (int32_t i = 0; i < MON_CONTENT_CNT; ++i)
	{
		const Series item{ GetMonContentLabel(kind, i), MON_CONTENT + i };
		if (IsMonContentGauge(kind, i))
		{
			gauges[gaugeCnt++] = item;
		}
		else
		{
			rates[rateCnt++] = item;
		}
	}
	if (!ImGui::BeginTable("charts", 2, ImGuiTableFlags_SizingStretchSame))
	{
		return;
	}
	ImGui::TableNextColumn();
	plotRows("CPU (코어)", rows, CPU, 1, bLive, nowSec);
	ImGui::TableNextColumn();
	plotRows("PC CPU (%)", rows, SYS_CPU, 1, bLive, nowSec);
	ImGui::TableNextColumn();
	plotRows("메모리 (MB)", rows, MEM, 1, bLive, nowSec);
	ImGui::TableNextColumn();
	plotRows("세션", rows, SESSION, 1, bLive, nowSec);
	ImGui::TableNextColumn();
	plotRows("수락 (/s)", rows, ACCEPT, 1, bLive, nowSec);
	ImGui::TableNextColumn();
	plotRows("송수신 (KB/s)", rows, BYTES, 2, bLive, nowSec);
	ImGui::TableNextColumn();
	plotRows("패킷 (/s)", rows, PACKETS, 2, bLive, nowSec);
	ImGui::TableNextColumn();
	plotRows("끊은 이유 (/s)", rows, CUTS, 5, bLive, nowSec);
	ImGui::TableNextColumn();
	plotRows("컨텐츠 - 지금 값", rows, gauges, gaugeCnt, bLive, nowSec);
	ImGui::TableNextColumn();
	plotRows("컨텐츠 - 초당", rows, rates, rateCnt, bLive, nowSec);
	ImGui::EndTable();
}

// 마지막 1초 값과 경고(설계 4-8 B - 화면 표시만). 경고가 있으면 true
static bool drawCurrentValues(const ServerView& server)
{
	bool bAlert = (server.State != eServerState::Online);
	if (server.Live.empty())
	{
		ImGui::TextColored(COLOR_GRAY, "아직 1초 값이 없음");
		return bAlert;
	}
	const MonRateRow& last = server.Live.back();
	const float* const v = last.Values;
	ImGui::Text("%s 기준", formatTime(last.UnixSec, "%H:%M:%S").c_str());
	const int32_t columns = (std::max)(3, (std::min)(9, static_cast<int32_t>(ImGui::GetContentRegionAvail().x / (130.0f * sUiScale)))); // 창 너비에 맞춰 칸 수
	if (ImGui::BeginTable("now", columns, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame))
	{
		struct Cell
		{
			const char* Label;
			float Value;
			bool bBad;
		};
		const float cuts = v[MON_OVERFLOW_PS] + v[MON_SEND_STALL_PS] + v[MON_SESSION_FULL_PS] + v[MON_BAD_HEADER_PS] + v[MON_BAD_PACKET_PS];
		std::vector<Cell> cells = {
			{ "CPU(코어)", v[MON_PROC_CPU], v[MON_PROC_CPU] >= 1.8f }, // 워커 2개가 거의 다 일함
			{ "PC CPU(%)", v[MON_SYS_CPU_PCT], v[MON_SYS_CPU_PCT] >= 90.0f },
			{ "메모리(MB)", v[MON_MEM_MB], false },
			{ "핸들", v[MON_HANDLES], false },
			{ "세션", v[MON_SESSION], false },
			{ "수락/s", v[MON_ACCEPT_PS], false },
			{ "받은KB/s", v[MON_RECV_KB_PS], false },
			{ "보낸KB/s", v[MON_SEND_KB_PS], false },
			{ "끊은 수/s", cuts, cuts > 0.0f },
			{ "PC 남은 메모리(MB)", v[MON_SYS_AVAIL_MB], v[MON_SYS_AVAIL_MB] < 500.0f },
		};
		for (int32_t i = 0; i < MON_CONTENT_CNT; ++i)
		{
			const float value = v[MON_CONTENT + i];
			bool bBad = false;
			if (server.Kind == eServerKind::Game)
			{
				bBad = (i == 3 && value >= 100.0f) || (i >= 4 && value > 0.0f); // DB대기 100 이상, DB실패·입장 끊김
			}
			else
			{
				bBad = (i == 4 && value <= 10.0f) || (i == 5 && value > 0.0f); // 키쌍 10개 이하, 키쌍부족끊김
			}
			cells.push_back(Cell{ GetMonContentLabel(server.Kind, i), value, bBad });
		}
		for (const Cell& cell : cells)
		{
			ImGui::TableNextColumn();
			ImGui::TextColored(cell.bBad ? COLOR_BAD : COLOR_GRAY, "%s", cell.Label);
			ImGui::TextColored(cell.bBad ? COLOR_BAD : ImVec4(1, 1, 1, 1), "%.2f", cell.Value);
			bAlert = bAlert || cell.bBad;
		}
		ImGui::EndTable();
	}
	return bAlert;
}

static void drawEvents(const std::vector<EventView>& events)
{
	if (!ImGui::BeginTable("events", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(-1, -1)))
	{
		return;
	}
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("시각", ImGuiTableColumnFlags_WidthFixed, 150 * sUiScale);
	ImGui::TableSetupColumn("서버", ImGuiTableColumnFlags_WidthFixed, 80 * sUiScale);
	ImGui::TableSetupColumn("내용");
	ImGui::TableHeadersRow();
	for (size_t i = events.size(); i > 0; --i) // 최근 것이 위
	{
		const EventView& event = events[i - 1];
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(formatTime(event.UnixMs / 1000, "%m-%d %H:%M:%S").c_str());
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(event.Name.c_str());
		ImGui::TableNextColumn();
		const bool bBad = (event.Text.find("응답 없음") != std::string::npos || event.Text.find("끊김") != std::string::npos || event.Text.find("실패") != std::string::npos);
		ImGui::TextColored(bBad ? COLOR_BAD : ImVec4(1, 1, 1, 1), "%s", event.Text.c_str());
	}
	ImGui::EndTable();
}

// "YYYY-MM-DD HH:MM" → 유닉스 초(지역 시각). 형식이 틀리면 -1
static int64_t parseLocalTime(const char* text)
{
	tm local{};
	if (sscanf_s(text, "%d-%d-%d %d:%d", &local.tm_year, &local.tm_mon, &local.tm_mday, &local.tm_hour, &local.tm_min) != 5)
	{
		return -1;
	}
	local.tm_year -= 1900;
	local.tm_mon -= 1;
	local.tm_isdst = -1;
	return static_cast<int64_t>(mktime(&local));
}

struct UiState
{
	int32_t Selected = 0;
	char HistoryFrom[32] = {};
	int32_t HistoryMinutes = 60;
	bool bHistoryMinute = false;
};

static void drawFrame(MonitorLink* link, UiState* ui)
{
	MonitorLink::Snapshot snap;
	link->TakeSnapshot(&snap);
	const double nowSec = static_cast<double>(time(nullptr));

	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGui::Begin("모니터", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	ImGui::TextColored(snap.bConnected ? COLOR_OK : COLOR_BAD, "%s", snap.Status.c_str());
	ImGui::SameLine();
	ImGui::TextColored(COLOR_GRAY, "  서버 %zu개 · 이벤트 %zu개", snap.Servers.size(), snap.Events.size());
	ImGui::Separator();

	// 왼쪽: 서버 목록
	ImGui::BeginChild("servers", ImVec2(200 * sUiScale, 0), ImGuiChildFlags_Border);
	ImGui::TextUnformatted("서버");
	for (size_t i = 0; i < snap.Servers.size(); ++i)
	{
		const ServerView& server = snap.Servers[i];
		ImGui::PushID(static_cast<int>(i));
		if (ImGui::Selectable(server.Name.c_str(), ui->Selected == static_cast<int32_t>(i)))
		{
			ui->Selected = static_cast<int32_t>(i);
		}
		ImGui::SameLine(100 * sUiScale);
		ImGui::TextColored(stateColor(server.State), "%s", toString(server.State));
		ImGui::PopID();
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// 오른쪽: 선택한 서버
	ImGui::BeginChild("main", ImVec2(0, 0));
	if (snap.Servers.empty())
	{
		ImGui::TextColored(COLOR_GRAY, "붙은 서버가 없음 - 로그인·게임 서버의 server.cfg monitorHost·monitorPort와 db.cfg monitorKey를 확인");
	}
	else
	{
		if (ui->Selected >= static_cast<int32_t>(snap.Servers.size()))
		{
			ui->Selected = 0;
		}
		const ServerView& server = snap.Servers[ui->Selected];
		ImGui::Text("%s (%s)", server.Name.c_str(), (server.Kind == eServerKind::Login) ? "로그인 서버" : "게임 서버");
		ImGui::SameLine();
		ImGui::TextColored(stateColor(server.State), "%s", toString(server.State));
		if (ImGui::BeginTabBar("tabs"))
		{
			if (ImGui::BeginTabItem("실시간"))
			{
				const float eventHeight = 150.0f * sUiScale;
				ImGui::BeginChild("live", ImVec2(0, -eventHeight), ImGuiChildFlags_None);
				const bool bAlert = drawCurrentValues(server);
				if (bAlert)
				{
					ImGui::TextColored(COLOR_BAD, "경고: 빨간 항목을 확인");
				}
				const std::vector<MonRateRow> rows(server.Live.begin(), server.Live.end());
				drawCharts(server.Kind, rows, true, nowSec);
				ImGui::EndChild();
				ImGui::TextUnformatted("이벤트");
				drawEvents(snap.Events);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("지난 기록"))
			{
				if (ui->HistoryFrom[0] == '\0')
				{
					const std::string defaultFrom = formatTime(static_cast<int64_t>(nowSec) - 3600, "%Y-%m-%d %H:%M");
					strcpy_s(ui->HistoryFrom, defaultFrom.c_str());
				}
				ImGui::SetNextItemWidth(160 * sUiScale);
				ImGui::InputText("시작(YYYY-MM-DD HH:MM)", ui->HistoryFrom, sizeof(ui->HistoryFrom));
				ImGui::SameLine();
				ImGui::SetNextItemWidth(100 * sUiScale);
				ImGui::InputInt("분", &ui->HistoryMinutes);
				ImGui::SameLine();
				ImGui::Checkbox("1분 요약", &ui->bHistoryMinute);
				ImGui::SameLine();
				const int64_t fromSec = parseLocalTime(ui->HistoryFrom);
				ImGui::BeginDisabled(snap.History.bLoading || !snap.bConnected || fromSec < 0 || ui->HistoryMinutes <= 0);
				if (ImGui::Button("불러오기"))
				{
					link->RequestHistory(server.Name, fromSec, fromSec + static_cast<int64_t>(ui->HistoryMinutes) * 60, ui->bHistoryMinute);
				}
				ImGui::EndDisabled();
				if (fromSec < 0)
				{
					ImGui::TextColored(COLOR_BAD, "시작 시각 형식: 2026-11-22 09:00");
				}
				const HistoryView& history = snap.History;
				if (!history.Name.empty())
				{
					ImGui::Text("%s %s ~ %s, %s, %zu행%s", history.Name.c_str(), formatTime(history.FromSec, "%m-%d %H:%M").c_str(),
						formatTime(history.ToSec, "%m-%d %H:%M").c_str(), history.bMinute ? "1분 요약 평균" : "1초", history.Rows.size(),
						history.bLoading ? " (불러오는 중)" : "");
					eServerKind kind = server.Kind; // 기록을 부른 서버의 종류 - 그사이 다른 서버를 골랐을 수 있다
					for (const ServerView& other : snap.Servers)
					{
						if (other.Name == history.Name)
						{
							kind = other.Kind;
						}
					}
					drawCharts(kind, history.Rows, false, nowSec);
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::EndChild();
	ImGui::End();
}

// db.cfg에서 monitorKey만 읽는다
static bool loadKey(std::string* outKey)
{
	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser("db.cfg", debug);
	return parser != nullptr && parser->Get("monitorKey", *outKey) && !outKey->empty();
}

int main(int argc, char* argv[])
{
	const std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
	const int32_t port = (argc > 2) ? atoi(argv[2]) : 33010;
	std::string key;
	if (!loadKey(&key))
	{
		MessageBoxW(nullptr, L"작업 폴더의 db.cfg에 monitorKey가 있어야 합니다", L"MonitorClient", MB_OK | MB_ICONERROR);
		return 1;
	}
	ImGui_ImplWin32_EnableDpiAwareness(); // 배율 125%·150% 화면에서 흐리게 늘이지 않고 글자 크기를 배율만큼
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	MonitorLink link;
	link.Start(host, port, key);

	WNDCLASSEXW windowClass = { sizeof(windowClass), CS_CLASSDC, onWindowMessage, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ArkchimyMonitor", nullptr };
	RegisterClassExW(&windowClass);
	RECT workArea{};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0); // 작업 표시줄을 뺀 화면의 90%
	const int width = (workArea.right - workArea.left) * 9 / 10;
	const int height = (workArea.bottom - workArea.top) * 9 / 10;
	HWND hWnd = CreateWindowW(windowClass.lpszClassName, L"Arkchimy 모니터", WS_OVERLAPPEDWINDOW, workArea.left + width / 18, workArea.top + height / 18, width, height,
		nullptr, nullptr, windowClass.hInstance, nullptr);
	if (!createDevice(hWnd))
	{
		releaseDevice();
		UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
		return 1;
	}
	ShowWindow(hWnd, SW_SHOWDEFAULT);
	UpdateWindow(hWnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImPlot::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr; // 창 배치 파일을 만들지 않는다
	const float dpiScale = ImGui_ImplWin32_GetDpiScaleForHwnd(hWnd);
	sUiScale = dpiScale;
	io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 17.0f * dpiScale, nullptr, io.Fonts->GetGlyphRangesKorean());
	ImGui::StyleColorsDark();
	ImGui::GetStyle().ScaleAllSizes(dpiScale);
	ImPlot::GetStyle().UseLocalTime = true;
	ImPlot::GetStyle().Use24HourClock = true;
	ImGui_ImplWin32_Init(hWnd);
	ImGui_ImplDX11_Init(sDevice, sContext);

	UiState ui;
	bool bDone = false;
	while (!bDone)
	{
		MSG msg;
		while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
			{
				bDone = true;
			}
		}
		if (bDone)
		{
			break;
		}
		if (sResizeWidth != 0 && sResizeHeight != 0)
		{
			releaseRenderTarget();
			sSwapChain->ResizeBuffers(0, sResizeWidth, sResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
			sResizeWidth = 0;
			sResizeHeight = 0;
			createRenderTarget();
		}
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		drawFrame(&link, &ui);
		ImGui::Render();
		const float clear[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
		sContext->OMSetRenderTargets(1, &sTarget, nullptr);
		sContext->ClearRenderTargetView(sTarget, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		sSwapChain->Present(1, 0); // 수직 동기 - 1초 값이라 60fps면 충분
	}

	link.Stop();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImPlot::DestroyContext();
	ImGui::DestroyContext();
	releaseDevice();
	DestroyWindow(hWnd);
	UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
	WSACleanup();
	return 0;
}
