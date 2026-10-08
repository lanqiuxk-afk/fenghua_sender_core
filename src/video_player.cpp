#include "video_player.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <mutex>
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavcodec/parser.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
}

#pragma comment(lib, "ws2_32.lib")

static const int PORT = 56790;

static const char* kMagic = "FHSC";

namespace {

std::atomic<bool> g_run{false};
std::thread g_thread;
std::atomic<SOCKET> g_sock{INVALID_SOCKET};
char g_ip[64] = "";

// FFmpeg 解码器
AVCodecContext* g_ffctx = nullptr;
AVFrame* g_ffframe = nullptr;
int g_decW = 0, g_decH = 0;

// 最新解码帧 (RGB32, top-down)。只保留一份缓冲: 解码线程写, 输出侧读
std::vector<uint8_t> g_back;
int g_frameW = 0, g_frameH = 0;
std::mutex g_frameMtx;

// 流解析状态
std::vector<uint8_t> g_csd;  // SPS+PPS (含起始码)
std::vector<uint8_t> g_au;   // 当前访问单元
bool g_auHasVcl = false;
AVCodecParserContext* g_parser = nullptr;
bool g_framedStream = false;

// 状态跟踪
char g_status[128] = "未启动";
std::atomic<bool> g_connected{false};
bool g_everConnected = false;
long long g_lastFrameMs = 0;   // 最近一帧解码完成的时间
int g_fps = 0;
long long g_fpsStart = 0;
int g_fpsCount = 0;
long long g_recvBytes = 0;     // 已收到字节数
std::atomic<long long> g_decodedFrames{0};
std::atomic<long long> g_decodeErrors{0};
std::atomic<long long> g_videoPackets{0};
char g_err[160] = "";          // 读取线程错误/状态, 供 GetStatus 显示
long long g_droppedFrames = 0; // 因积压丢掉的帧数

}  // namespace

// ---------------- FFmpeg 解码 ----------------

static void FeedAccessUnit(const std::vector<uint8_t>& au);
static bool ResetRawH264Parser();

static bool InitDecoder(int w, int h) {
    if (g_ffctx) {
        if (g_decW == w && g_decH == h) return true;
        avcodec_free_context(&g_ffctx);
        g_ffctx = nullptr;
    }
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        snprintf(g_err, sizeof(g_err), "avcodec_find_decoder(H264) 返回空");
        return false;
    }
    g_ffctx = avcodec_alloc_context3(codec);
    if (!g_ffctx) {
        snprintf(g_err, sizeof(g_err), "avcodec_alloc_context3 失败");
        return false;
    }
    // 2 线程: 管线延迟 ~16ms, 兼顾吞吐
    g_ffctx->thread_count = 2;
    g_ffctx->width = w;
    g_ffctx->height = h;
    g_ffctx->pix_fmt = AV_PIX_FMT_YUV420P;
    int orc = avcodec_open2(g_ffctx, codec, nullptr);
    if (orc < 0) {
        snprintf(g_err, sizeof(g_err), "avcodec_open2 失败 (%d)", orc);
        avcodec_free_context(&g_ffctx);
        g_ffctx = nullptr;
        return false;
    }
    if (!g_ffframe) g_ffframe = av_frame_alloc();
    if (!g_ffframe) {
        snprintf(g_err, sizeof(g_err), "av_frame_alloc 失败");
        avcodec_free_context(&g_ffctx);
        g_ffctx = nullptr;
        return false;
    }
    // 说明: 这里不再要求 h264 parser。
    // scrcpy 的精简 FFmpeg (--disable-everything ... --enable-parser=png) 没编译
    // h264 parser, av_parser_init 会返回 NULL; 解码只依赖 avcodec_send_packet,
    // 所以 parser 缺失不影响解码。
    g_decW = w;
    g_decH = h;
    snprintf(g_status, sizeof(g_status), "已连接 %s  %dx%d (FFmpeg)", g_ip, w, h);
    return true;
}

static void FeedAccessUnit(const std::vector<uint8_t>& au);
static bool ResetRawH264Parser();
static void ParseRawH264(std::vector<uint8_t>& acc);
static void FlushRawH264Parser();
static void ParseFramedH264Packet(const std::vector<uint8_t>& au);

static bool HasAnnexBStartCode(const uint8_t* data, size_t len) {
    return len >= 3 && data[0] == 0 && data[1] == 0 &&
           (data[2] == 1 || (len >= 4 && data[2] == 0 && data[3] == 1));
}

static bool ConvertAvccToAnnexB(const uint8_t* data, size_t len,
                                std::vector<uint8_t>& out) {
    out.clear();
    // AVCDecoderConfigurationRecord (often used for codec-config output).
    if (len >= 7 && data[0] == 1) {
        size_t off = 5;
        int spsCount = data[5] & 0x1F;
        for (int i = 0; i < spsCount; ++i) {
            if (off + 2 > len) return false;
            uint16_t n = (uint16_t)data[off] << 8 | data[off + 1];
            off += 2;
            if (n == 0 || off + n > len) return false;
            out.insert(out.end(), {0, 0, 0, 1});
            out.insert(out.end(), data + off, data + off + n);
            off += n;
        }
        if (off >= len) return !out.empty();
        int ppsCount = data[off++];
        for (int i = 0; i < ppsCount; ++i) {
            if (off + 2 > len) return false;
            uint16_t n = (uint16_t)data[off] << 8 | data[off + 1];
            off += 2;
            if (n == 0 || off + n > len) return false;
            out.insert(out.end(), {0, 0, 0, 1});
            out.insert(out.end(), data + off, data + off + n);
            off += n;
        }
        return off == len && !out.empty();
    }
    for (int lengthBytes : {4, 3, 2, 1}) {
        std::vector<uint8_t> candidate;
        size_t off = 0;
        while (off + (size_t)lengthBytes <= len) {
            uint32_t nalLen = 0;
            for (int j = 0; j < lengthBytes; ++j)
                nalLen = (nalLen << 8) | data[off + j];
            off += lengthBytes;
            if (nalLen == 0 || off + nalLen > len) break;
            candidate.insert(candidate.end(), {0, 0, 0, 1});
            candidate.insert(candidate.end(), data + off, data + off + nalLen);
            off += nalLen;
        }
        if (off == len && !candidate.empty()) {
            out = std::move(candidate);
            return true;
        }
    }
    return false;
}

static const std::vector<uint8_t>* NormalizeH264(const std::vector<uint8_t>& in,
                                                  std::vector<uint8_t>& converted) {
    if (HasAnnexBStartCode(in.data(), in.size())) return &in;
    if (ConvertAvccToAnnexB(in.data(), in.size(), converted)) return &converted;
    return &in;
}

// 转换 [row0,row1) 行 YUV -> RGB32 (NV12/NV21/平面YUV420)
static void ConvertRows(const AVFrame* f, uint8_t* rgb, int w, int row0, int row1) {
    bool nv = (f->format == AV_PIX_FMT_NV12 || f->format == AV_PIX_FMT_NV21);
    bool nv21 = (f->format == AV_PIX_FMT_NV21);
    const uint8_t* y = f->data[0];
    const uint8_t* u = f->data[1];
    const uint8_t* v = f->data[2];
    for (int j = row0; j < row1; j++) {
        for (int i = 0; i < w; i++) {
            int yy = y[(size_t)j * f->linesize[0] + i];
            int uu, vv;
            if (nv) {
                uu = u[(size_t)(j / 2) * f->linesize[1] + (i / 2) * 2 + (nv21 ? 1 : 0)] - 128;
                vv = u[(size_t)(j / 2) * f->linesize[1] + (i / 2) * 2 + (nv21 ? 0 : 1)] - 128;
            } else {
                uu = u[(size_t)(j / 2) * f->linesize[1] + i / 2] - 128;
                vv = v[(size_t)(j / 2) * f->linesize[2] + i / 2] - 128;
            }
            int c = (yy - 16) * 298;
            int r = (c + 409 * vv + 128) >> 8;
            int g = (c - 100 * uu - 208 * vv + 128) >> 8;
            int b = (c + 516 * uu + 128) >> 8;
            uint8_t* p = rgb + ((size_t)j * w + i) * 4;
            p[0] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
            p[1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            p[2] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            p[3] = 0xFF;
        }
    }
}

// AVFrame -> RGB32, 按行分 4 段并行转换 (转换是主要 CPU 开销, 串行会超过 60fps 预算)
static void FrameToRgb32(const AVFrame* f, uint8_t* rgb, int w, int h) {
    const int nThreads = 4;
    if (h < 64) {  // 小图单线程即可
        ConvertRows(f, rgb, w, 0, h);
        return;
    }
    int band = h / nThreads;
    std::vector<std::thread> ts;
    ts.reserve(nThreads);
    for (int i = 0; i < nThreads; i++) {
        int r0 = i * band;
        int r1 = (i == nThreads - 1) ? h : r0 + band;
        if (r0 >= h) break;
        ts.emplace_back(ConvertRows, f, rgb, w, r0, r1);
    }
    for (auto& t : ts) t.join();
}

// 帧输出: 解码后的 RGB32 帧按需落盘 (用于取帧/截图/二次分发)
namespace {
std::atomic<bool> g_dumpOn{false};
std::string g_dumpDir;
std::atomic<long long> g_dumpSeq{0};
}  // namespace

void VideoPlayerSetFrameDump(bool on, const char* dir) {
    g_dumpOn = on;
    if (dir && *dir) g_dumpDir = dir;
}

// 发布帧: 计数 + 可选的 PPM 落盘 (无窗口)
static void PublishFrame(int w, int h) {
    long long now = (long long)GetTickCount64();
    g_lastFrameMs = now;
    g_decodedFrames.fetch_add(1);
    g_fpsCount++;
    if (now - g_fpsStart >= 1000) {
        g_fps = g_fpsCount;
        g_fpsCount = 0;
        g_fpsStart = now;
    }
    if (g_dumpOn.load()) {
        // PPM (P6): 无需任何图像库, 可直接用 ffmpeg/看图工具打开
        static bool dirReady = false;
        if (!dirReady) {
            CreateDirectoryA(g_dumpDir.c_str(), nullptr);   // 目录不存在时创建
            dirReady = true;
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%06lld.ppm", g_dumpDir.c_str(),
                 g_dumpSeq.fetch_add(1));
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            const uint8_t* p = g_back.data();
            for (int i = 0; i < w * h; i++) {
                // RGB32 (B,G,R,X) -> PPM (R,G,B)
                uint8_t rgb[3] = {p[i * 4 + 2], p[i * 4 + 1], p[i * 4 + 0]};
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
    }
}

// 从解码器拉取所有可用输出帧
static void PullFrames() {
    if (!g_ffctx || !g_ffframe) return;
    while (true) {
        int r = avcodec_receive_frame(g_ffctx, g_ffframe);
        if (r < 0) break;  // EAGAIN(还需输入) / EOF
        int fw = g_ffframe->width, fh = g_ffframe->height;
        if (fw > 0 && fh > 0 && g_ffframe->data[0]) {
            size_t need = (size_t)fw * fh * 4;
            if (g_back.size() < need) g_back.resize(need);
            FrameToRgb32(g_ffframe, g_back.data(), fw, fh);
            PublishFrame(fw, fh);
            g_decW = fw;
            g_decH = fh;
        }
        av_frame_unref(g_ffframe);
    }
}

// ---------------- OBS 输出: H.264 -> MPEG-TS/UDP (127.0.0.1:9998) ----------------
// 手工打包 PAT/PMT/PES, 无外部依赖; OBS 媒体源用 udp://127.0.0.1:9998 采集

static SOCKET g_tsSock = INVALID_SOCKET;
static std::atomic<bool> g_obsOn{false};
static std::atomic<unsigned short> g_obsPort{9998};
static sockaddr_in g_tsDest = {};
static std::mutex g_tsMtx;
static const uint16_t kPidPat = 0x0000;
static const uint16_t kPidPmt = 0x1000;
static const uint16_t kPidVideo = 0x0100;
static uint8_t g_ccPat = 0, g_ccPmt = 0, g_ccVideo = 0;
static long long g_tsStartMs = 0;
static long long g_tsLastTableMs = 0;
static long long g_obsNextPtsMs = 0;      // 下一帧应发的时间戳(步进)
static long long g_obsLastArrivalMs = 0;  // 上一帧到达时刻
static float g_obsAvgDelta = 16.0f;       // 平均帧间隔 EMA
static std::vector<uint8_t> g_obsLastKeyAu;   // 最近关键帧(静默期重发用)
static long long g_tsLastFrameSentMs = 0;     // 最后发送帧时刻

// MPEG-2 CRC32 (poly 0x04C11DB7, init 0xFFFFFFFF, MSB-first, no reflect)
static uint32_t MpegCrc32(const uint8_t* d, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint32_t)d[i] << 24;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
    }
    return crc;
}

// 发送单个 TS 包 (188B): 4B 头 + 184B 负载(不足 0xFF 填充)
static void TsSendPacket(uint16_t pid, bool pusi, uint8_t& cc, const uint8_t* payload, size_t n) {
    std::lock_guard<std::mutex> lk(g_tsMtx);
    if (g_tsSock == INVALID_SOCKET) return;
    uint8_t pkt[188];
    pkt[0] = 0x47;
    pkt[1] = (pusi ? 0x40 : 0x00) | ((pid >> 8) & 0x1F);
    pkt[2] = pid & 0xFF;
    pkt[3] = 0x10 | (cc & 0x0F);  // AFC=01 仅负载, 无 PCR
    cc = (cc + 1) & 0x0F;
    size_t body = n < 184 ? n : 184;
    memcpy(pkt + 4, payload, body);
    if (body < 184) memset(pkt + 4 + body, 0xFF, 184 - body);
    sendto(g_tsSock, (const char*)pkt, 188, 0, (sockaddr*)&g_tsDest, sizeof(g_tsDest));
}

// 视频 PID 上发送 PCR (27MHz, 带适配域), 供解码器建立时钟
static void TsWritePcr(long long ptsMs) {
    std::lock_guard<std::mutex> lk(g_tsMtx);
    if (g_tsSock == INVALID_SOCKET) return;
    long long pcr = ptsMs * 27000LL;       // ms -> 27MHz
    long long pcrBase = pcr / 300;
    long long pcrExt = pcr % 300;
    uint8_t pkt[188];
    pkt[0] = 0x47;
    pkt[1] = (kPidVideo >> 8) & 0x1F;
    pkt[2] = kPidVideo & 0xFF;
    pkt[3] = 0x20 | (g_ccVideo & 0x0F);    // AFC=10 仅适配域
    // Adaptation-only packets do not advance the continuity counter.
    pkt[4] = 183;                          // adaptation_field_length
    pkt[5] = 0x10;                         // PCR_flag
    pkt[6] = (uint8_t)(pcrBase >> 25);
    pkt[7] = (uint8_t)(pcrBase >> 17);
    pkt[8] = (uint8_t)(pcrBase >> 9);
    pkt[9] = (uint8_t)(pcrBase >> 1);
    pkt[10] = (uint8_t)(((pcrBase & 1) << 7) | 0x7E | ((pcrExt >> 8) & 1));
    pkt[11] = (uint8_t)pcrExt;
    memset(pkt + 12, 0xFF, 176);
    sendto(g_tsSock, (const char*)pkt, 188, 0, (sockaddr*)&g_tsDest, sizeof(g_tsDest));
}

// 发送一个 section (PAT/PMT 用): 打包进 TS 包 (PSI 负载)
static void TsSendSection(uint16_t pid, uint8_t& cc, const uint8_t* sec, size_t len) {
    // 首包: pointer_field=0 + section; 其余包续传
    size_t off = 0;
    bool first = true;
    while (off < len) {
        size_t cap = first ? 183 : 184;
        std::vector<uint8_t> body;
        if (first) body.push_back(0x00);
        size_t take = (len - off) < cap ? (len - off) : cap;
        body.insert(body.end(), sec + off, sec + off + take);
        off += take;
        TsSendPacket(pid, first, cc, body.data(), body.size());
        first = false;
    }
}

// 周期发送 PAT + PMT
static void TsWriteTables() {
    // PAT: program 1 -> PMT PID
    uint8_t pat[16] = {};
    pat[0] = 0x00;                     // table_id
    pat[1] = 0xB0;                     // section_syntax + '0'
    pat[2] = 0x0D;                     // section_length = 13
    pat[3] = 0x00; pat[4] = 0x01;      // ts_id
    pat[5] = 0xC1;                     // version + current_next
    pat[6] = 0x00; pat[7] = 0x00;      // section_number/last
    pat[8] = 0x00; pat[9] = 0x01;      // program_number
    pat[10] = 0xE0 | ((kPidPmt >> 8) & 0x1F);
    pat[11] = (uint8_t)kPidPmt;
    uint32_t crc = MpegCrc32(pat, 12);
    pat[12] = crc >> 24; pat[13] = crc >> 16; pat[14] = crc >> 8; pat[15] = crc;
    TsSendSection(kPidPat, g_ccPat, pat, sizeof(pat));

    // PMT: H.264 stream -> video PID
    uint8_t pmt[21] = {};
    pmt[0] = 0x02;                     // table_id
    pmt[1] = 0xB0;
    pmt[2] = 0x12;                     // section_length = 18
    pmt[3] = 0x00; pmt[4] = 0x01;      // program_number
    pmt[5] = 0xC1;                     // version
    pmt[6] = 0x00; pmt[7] = 0x00;
    pmt[8] = 0xE0 | ((kPidVideo >> 8) & 0x1F);  // PCR_PID = video PID
    pmt[9] = (uint8_t)kPidVideo;
    pmt[10] = 0xF0; pmt[11] = 0x00;    // program_info_length = 0
    pmt[12] = 0x1B;                    // stream_type = AVC
    pmt[13] = 0xE0 | ((kPidVideo >> 8) & 0x1F);
    pmt[14] = (uint8_t)kPidVideo;
    pmt[15] = 0xF0; pmt[16] = 0x00;    // ES_info_length = 0
    crc = MpegCrc32(pmt, 17);
    pmt[17] = crc >> 24; pmt[18] = crc >> 16; pmt[19] = crc >> 8; pmt[20] = crc;
    TsSendSection(kPidPmt, g_ccPmt, pmt, sizeof(pmt));
}

// Annex-B (00 00 00 01 / 00 00 01) 转 3 字节起始码并打包 PES
static void TsWritePes(const std::vector<uint8_t>& au, long long ptsMs) {
    if (g_tsSock == INVALID_SOCKET || au.empty()) return;
    // 构建 PES 负载: 起始码统一为 00 00 01
    std::vector<uint8_t> es;
    es.reserve(au.size());
    for (size_t i = 0; i < au.size();) {
        if (i + 4 <= au.size() && au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1) {
            es.push_back(0); es.push_back(0); es.push_back(1);
            i += 4;
        } else if (i + 3 <= au.size() && au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            es.push_back(0); es.push_back(0); es.push_back(1);
            i += 3;
        } else {
            es.push_back(au[i]);
            i++;
        }
    }
    // PES 头: 00 00 01 E0 + length + 0x80 0x80 0x05 + PTS(5B)
    long long pts = ptsMs * 90;  // 90kHz
    std::vector<uint8_t> pes;
    pes.reserve(es.size() + 14);
    pes.push_back(0); pes.push_back(0); pes.push_back(1); pes.push_back(0xE0);
    size_t pesLen = 3 + 5 + es.size();
    if (pesLen > 0xFFFF) pesLen = 0;
    pes.push_back((uint8_t)(pesLen >> 8));
    pes.push_back((uint8_t)pesLen);
    pes.push_back(0x80);  // '10'
    pes.push_back(0x80);  // PTS only
    pes.push_back(0x05);  // PES header data length
    uint8_t p[5] = {
        (uint8_t)(0x20 | (((pts >> 30) & 0x07) << 1) | 1),
        (uint8_t)((pts >> 22) & 0xFF),
        (uint8_t)((((pts >> 15) & 0x7F) << 1) | 1),
        (uint8_t)((pts >> 7) & 0xFF),
        (uint8_t)(((pts & 0x7F) << 1) | 1),
    };
    pes.insert(pes.end(), p, p + 5);
    pes.insert(pes.end(), es.begin(), es.end());

    // 切分到 TS 包
    size_t off = 0;
    bool first = true;
    while (off < pes.size()) {
        size_t take = pes.size() - off;
        if (take > 184) take = 184;
        TsSendPacket(kPidVideo, first, g_ccVideo, pes.data() + off, take);
        off += take;
        first = false;
    }
}

static void ObsInit() {
    std::lock_guard<std::mutex> lk(g_tsMtx);
    if (g_tsSock != INVALID_SOCKET) return;
    g_tsSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_tsSock == INVALID_SOCKET) return;
    g_tsDest = {};
    g_tsDest.sin_family = AF_INET;
    g_tsDest.sin_port = htons(g_obsPort.load());
    g_tsDest.sin_addr.s_addr = inet_addr("127.0.0.1");
    g_tsStartMs = (long long)GetTickCount64();
    g_tsLastTableMs = 0;
    g_ccPat = g_ccPmt = g_ccVideo = 0;
    g_obsNextPtsMs = 0;
    g_obsLastArrivalMs = 0;
    g_obsAvgDelta = 16.0f;
    g_tsLastFrameSentMs = 0;
}

static void ObsClose() {
    std::lock_guard<std::mutex> lk(g_tsMtx);
    if (g_tsSock != INVALID_SOCKET) {
        closesocket(g_tsSock);
        g_tsSock = INVALID_SOCKET;
    }
}

// OBS 保活线程: 只要"OBS输出"为开, 就持续向 127.0.0.1:9998 发送节目表(250ms)
// + PCR/缓存关键帧(1s), 完全不依赖投屏读取线程状态。
// 这样 OBS 无论何时打开媒体源都能探测到流, 避免"打开输入流失败"。
static std::thread g_obsThread;
static std::atomic<bool> g_obsThreadOn{false};

static long long ObsNextPtsMs(long long now);

static void ObsKeepThread() {
    while (g_obsOn.load()) {
        ObsInit();
        {
            std::lock_guard<std::mutex> lk(g_tsMtx);
            if (g_tsSock == INVALID_SOCKET) { Sleep(50); continue; }
        }
        long long now = (long long)GetTickCount64();
        bool writeTables = false;
        std::vector<uint8_t> keyAu;
        {
            std::lock_guard<std::mutex> lk(g_tsMtx);
            if (now - g_tsLastTableMs > 250) {
                g_tsLastTableMs = now;
                writeTables = true;
            }
            if (!g_obsLastKeyAu.empty() && now - g_tsLastFrameSentMs > 1000)
                keyAu = g_obsLastKeyAu;
        }
        if (writeTables) TsWriteTables();
        // 静默超过 1s: 重发缓存关键帧, 让 OBS 探测时识别到视频流并显示首帧
        if (!keyAu.empty()) {
            long long pts = ObsNextPtsMs(now);
            TsWritePcr(pts);
            TsWritePes(keyAu, pts);
            std::lock_guard<std::mutex> lk(g_tsMtx);
            g_tsLastFrameSentMs = now;
        }
        Sleep(50);
    }
}

void VideoPlayerSetObs(bool on, unsigned short port) {
    g_obsPort = port;
    g_obsOn = on;
    if (on) {
        ObsInit();
        TsWriteTables();  // 立即发送节目表, OBS 打开源时不等待视频帧
        {
            std::lock_guard<std::mutex> lk(g_tsMtx);
            g_tsLastTableMs = (long long)GetTickCount64();
        }
        if (!g_obsThreadOn.exchange(true)) {
            g_obsThread = std::thread(ObsKeepThread);
        }
    } else {
        if (g_obsThreadOn.exchange(false)) {
            if (g_obsThread.joinable()) g_obsThread.join();
        }
        ObsClose();
    }
}

bool VideoPlayerGetObs() { return g_obsOn.load(); }

static bool AuHasIdr(const std::vector<uint8_t>& au) {
    for (size_t i = 0; i + 4 <= au.size();) {
        if (au[i] == 0 && au[i + 1] == 0 && (au[i + 2] == 1 || (au[i + 2] == 0 && au[i + 3] == 1))) {
            size_t hdr = (au[i + 2] == 1) ? 3 : 4;
            if (i + hdr < au.size() && (au[i + hdr] & 0x1F) == 5) return true;
            i += hdr;
        } else i++;
    }
    return false;
}

// 计算下一帧 PTS (步进铺开, 见 FeedObs 注释)
static long long ObsNextPtsMs(long long now) {
    std::lock_guard<std::mutex> lk(g_tsMtx);
    long long nowMs = now - g_tsStartMs;
    float d = (float)(nowMs - g_obsLastArrivalMs);
    if (d > 1.0f && d < 200.0f)
        g_obsAvgDelta = g_obsAvgDelta * 0.9f + d * 0.1f;
    g_obsLastArrivalMs = nowMs;
    long long next = g_obsNextPtsMs + (long long)g_obsAvgDelta;
    if (next < nowMs) next = nowMs;
    if (next > nowMs + 500) next = nowMs;
    g_obsNextPtsMs = next;
    return next;
}

// 将解码单元同时送往 OBS 转发 (MPEG-TS/UDP)
static void FeedObs(const std::vector<uint8_t>& au) {
    if (!g_obsOn.load()) return;
    ObsInit();
    {
        std::lock_guard<std::mutex> lk(g_tsMtx);
        if (g_tsSock == INVALID_SOCKET) return;
    }
    long long now = (long long)GetTickCount64();
    bool writeTables = false;
    {
        std::lock_guard<std::mutex> lk(g_tsMtx);
        if (now - g_tsLastTableMs > 500) {
            g_tsLastTableMs = now;
            writeTables = true;
        }
        if (AuHasIdr(au)) g_obsLastKeyAu = au;
    }
    if (writeTables) TsWriteTables();
    // PTS 步进铺开: 网络到达是突发式的, 直接按到达时刻打时间戳会让 OBS
    // 一会儿连播一串、一会儿空等(卡顿)。改为按平均帧间隔把突发帧的
    // 时间戳铺开到未来(OBS 缓冲后匀速播放); 画面静止后立即跳回当前时刻恢复
    long long pts = ObsNextPtsMs(now);
    TsWritePcr(pts);
    TsWritePes(au, pts);
    {
        std::lock_guard<std::mutex> lk(g_tsMtx);
        g_tsLastFrameSentMs = now;
    }
}

// 将一个访问单元送入解码器
static void FeedAccessUnit(const std::vector<uint8_t>& au) {
    std::vector<uint8_t> converted;
    const std::vector<uint8_t>* packet = NormalizeH264(au, converted);
    g_videoPackets.fetch_add(1);
    FeedObs(*packet);
    if (!g_ffctx || packet->empty()) return;
    AVPacket pkt;
    if (av_new_packet(&pkt, (int)packet->size()) < 0) {
        g_decodeErrors.fetch_add(1);
        return;
    }
    memcpy(pkt.data, packet->data(), packet->size());
    int r = 0;
    for (int guard = 0; guard < 64; guard++) {
        r = avcodec_send_packet(g_ffctx, &pkt);
        if (r == AVERROR(EAGAIN)) {
            // 输入队列满, 先取帧腾空间再重试
            PullFrames();
            continue;
        }
        break;
    }
    av_packet_unref(&pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) {
        g_decodeErrors.fetch_add(1);
        // 解码错误, 刷新重同步
        avcodec_flush_buffers(g_ffctx);
    }
    PullFrames();
}

// 一个访问单元直接送解码器。
//
// 为什么不用 av_parser_parse2:
//   scrcpy 精简版 FFmpeg (--disable-everything --enable-decoder=h264 ... --enable-parser=png)
//   里没有编译 h264 parser, av_parser_init(AV_CODEC_ID_H264) 永远返回 NULL,
//   走 parser 的路径会一帧都解不出来。而 framed 流里每个包本身就是完整访问单元,
//   直接 avcodec_send_packet 即可; 解码器会自行处理内部 slice 边界。
static void ParseFramedH264Packet(const std::vector<uint8_t>& au) {
    std::vector<uint8_t> converted;
    const std::vector<uint8_t>* packet = NormalizeH264(au, converted);
    if (!g_ffctx || packet->empty()) return;
    FeedAccessUnit(*packet);
}

static bool ResetRawH264Parser() {
    if (g_parser) av_parser_close(g_parser);
    g_parser = av_parser_init(AV_CODEC_ID_H264);
    return g_parser != nullptr;
}

static void ParseRawH264(std::vector<uint8_t>& acc) {
    if (!g_parser || !g_ffctx) return;
    while (!acc.empty()) {
        uint8_t* out = nullptr;
        int outSize = 0;
        int inputSize = (int)std::min<size_t>(acc.size(), 1 << 20);
        int consumed = av_parser_parse2(g_parser, g_ffctx, &out, &outSize,
                                        acc.data(), inputSize, AV_NOPTS_VALUE,
                                        AV_NOPTS_VALUE, 0);
        if (consumed < 0) {
            g_decodeErrors.fetch_add(1);
            acc.clear();
            avcodec_flush_buffers(g_ffctx);
            return;
        }
        if (outSize > 0 && out)
            FeedAccessUnit(std::vector<uint8_t>(out, out + outSize));
        if (consumed == 0) break;
        acc.erase(acc.begin(), acc.begin() + consumed);
    }
}

static void FlushRawH264Parser() {
    if (!g_parser || !g_ffctx) return;
    uint8_t* out = nullptr;
    int outSize = 0;
    av_parser_parse2(g_parser, g_ffctx, &out, &outSize, nullptr, 0,
                     AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (outSize > 0 && out)
        FeedAccessUnit(std::vector<uint8_t>(out, out + outSize));
}

// ---------------- H.264 NAL 解析 ----------------

static inline void AppendSc(std::vector<uint8_t>& v) {
    v.push_back(0);
    v.push_back(0);
    v.push_back(0);
    v.push_back(1);
}

// 处理一个 NAL 单元 (不含起始码)
static void OnNal(const uint8_t* nal, size_t len) {
    if (len < 1) return;
    uint8_t type = nal[0] & 0x1F;
    bool vcl = (type == 1 || type == 5);  // non-IDR slice / IDR slice

    if (type == 7 || type == 8) {  // SPS / PPS
        AppendSc(g_csd);
        g_csd.insert(g_csd.end(), nal, nal + len);
    }

    if (vcl && g_auHasVcl) {
        // 上一访问单元结束, 提交
        FeedAccessUnit(g_au);
        g_au.clear();
        g_auHasVcl = false;
    }
    if (g_au.empty() && type == 5) {
        // 只在 IDR 前置 SPS/PPS, 减少每帧冗余解码开销
        g_au.insert(g_au.end(), g_csd.begin(), g_csd.end());
    }
    AppendSc(g_au);
    g_au.insert(g_au.end(), nal, nal + len);
    if (vcl) g_auHasVcl = true;
}

// 从 [0,len) 提取完整 NAL 单元; len 是下一个(不完整)起始码的位置
static void ParseH264(const uint8_t* data, size_t len) {
    size_t i = 0;
    while (i < len) {
        // 找起始码
        size_t sc = i;
        while (sc + 4 <= len && !(data[sc] == 0 && data[sc + 1] == 0 &&
                                  (data[sc + 2] == 1 ||
                                   (data[sc + 2] == 0 && data[sc + 3] == 1))))
            sc++;
        if (sc + 4 > len) break;
        size_t hdr = (data[sc + 2] == 1) ? 3 : 4;
        size_t nalStart = sc + hdr;
        // 找下一个起始码
        size_t next = nalStart;
        bool found = false;
        while (next + 4 <= len) {
            if (data[next] == 0 && data[next + 1] == 0 &&
                (data[next + 2] == 1 || (data[next + 2] == 0 && data[next + 3] == 1))) {
                found = true;
                break;
            }
            next++;
        }
        if (!found) next = len;  // 最后一个 NAL 延伸到完整区域末尾
        OnNal(data + nalStart, next - nalStart);
        i = next;
    }
}

// ---------------- 网络读取 ----------------

static bool ConnectWithTimeout(int fd, const sockaddr_in& a, int timeoutSec) {
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);  // 非阻塞
    int rc = connect(fd, (const sockaddr*)&a, sizeof(a));
    if (rc != 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(fd, &wf);
            timeval tv = {timeoutSec, 0};
            rc = select(0, nullptr, &wf, nullptr, &tv);
            if (rc > 0) {
                int soerr = 0;
                int len = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
                rc = (soerr == 0) ? 0 : -1;
            } else {
                rc = -1;  // 超时或错误
            }
        } else {
            rc = -1;
        }
    }
    mode = 0;
    ioctlsocket(fd, FIONBIO, &mode);  // 恢复阻塞
    return rc == 0;
}

static void ReaderThread() {
    // 自动重连: 连接失败/断开后每 2 秒重试, 设备端投屏重启后无需手动重连
    while (g_run) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        g_sock.store(sock);
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_port = htons(PORT);
        inet_pton(AF_INET, g_ip, &a.sin_addr);
        if (sock == INVALID_SOCKET || !ConnectWithTimeout(sock, a, 5)) {
            snprintf(g_err, sizeof(g_err), "连接失败: %s:56790(2秒后自动重试)", g_ip);
            snprintf(g_status, sizeof(g_status), "连接失败: %s:56790, 2秒后重试", g_ip);
            if (g_sock.load() == sock) {
                g_sock.store(INVALID_SOCKET);
                if (sock != INVALID_SOCKET) closesocket(sock);
            }
            for (int i = 0; i < 200 && g_run; i++) Sleep(10);  // 2s 后重试
            continue;
        }
        BOOL noDelay = TRUE;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
        // 接收超时 400ms: 画面静止(无帧)时循环仍能tick, 维持 OBS 节目表心跳
        DWORD rcvto = 400;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvto, sizeof(rcvto));
        g_connected = true;
        g_everConnected = true;
        snprintf(g_status, sizeof(g_status), "已连接 %s:56790", g_ip);

        int w = 0, h = 0;
        bool gotHeader = false;
        uint8_t buf[32768];
        std::vector<uint8_t> acc;
        bool magicMatched = false;
        int magicIdx = 0;

        while (g_run && g_sock.load() == sock) {
            int n = recv(sock, (char*)buf, sizeof(buf), 0);
            if (n <= 0) {
                if (n == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT) {
                    continue;  // 无帧: OBS 保活由独立线程负责
                }
                break;
            }
            g_recvBytes += n;
            acc.insert(acc.end(), buf, buf + n);

            if (!gotHeader) {
                // 解析 16 字节流头: "FHSC" + w(u32BE) + h(u32BE) + flags
                if (acc.size() < 16) continue;
                if (memcmp(acc.data(), kMagic, 4) != 0) {
                    snprintf(g_err, sizeof(g_err), "流头错误(首4字节非FHSC) 收到%lldB", g_recvBytes);
                    snprintf(g_status, sizeof(g_status), "流头错误");
                    break;
                }
                w = ((uint32_t)acc[4] << 24) | ((uint32_t)acc[5] << 16) |
                    ((uint32_t)acc[6] << 8) | acc[7];
                h = ((uint32_t)acc[8] << 24) | ((uint32_t)acc[9] << 16) |
                    ((uint32_t)acc[10] << 8) | acc[11];
                if (w <= 0 || h <= 0) {
                    snprintf(g_err, sizeof(g_err), "非法分辨率 %dx%d", w, h);
                    snprintf(g_status, sizeof(g_status), "非法分辨率 %dx%d", w, h);
                    break;
                }
                gotHeader = true;
                g_framedStream = (acc[15] & 0x01) != 0;
                if (!InitDecoder(w, h)) {
                    snprintf(g_status, sizeof(g_status), "解码器初始化失败 %dx%d: %s", w, h, g_err);
                    printf("\n[stream] %s\n", g_status);   // 保留在日志里, 便于定位
                    fflush(stdout);
                    break;
                }
                if (g_ffctx) avcodec_flush_buffers(g_ffctx);  // 重连后清空解码器状态
                snprintf(g_status, sizeof(g_status), "已连接 %s  %dx%d", g_ip, w, h);
                acc.erase(acc.begin(), acc.begin() + 16);
            }

            if (g_framedStream) {
                while (acc.size() >= 4) {
                    uint32_t frameSize = ((uint32_t)acc[0] << 24) |
                                         ((uint32_t)acc[1] << 16) |
                                         ((uint32_t)acc[2] << 8) | acc[3];
                    if (frameSize == 0 || frameSize > (8u << 20)) {
                        snprintf(g_err, sizeof(g_err), "非法视频帧长度 %u", frameSize);
                        snprintf(g_status, sizeof(g_status), "非法视频帧长度");
                        acc.clear();
                        break;
                    }
                    if (acc.size() < (size_t)frameSize + 4) break;
                    std::vector<uint8_t> au(acc.begin() + 4,
                                            acc.begin() + 4 + frameSize);
                    ParseFramedH264Packet(au);
                    acc.erase(acc.begin(), acc.begin() + 4 + frameSize);
                }
            } else if (acc.size() >= 4) {
                int lastSc = -1;
                for (int k = 0; k + 4 <= (int)acc.size(); ++k) {
                    if (acc[k] == 0 && acc[k + 1] == 0 &&
                        (acc[k + 2] == 1 || (acc[k + 2] == 0 && acc[k + 3] == 1)))
                        lastSc = k;
                }
                if (lastSc > 0) {
                    ParseH264(acc.data(), (size_t)lastSc);
                    acc.erase(acc.begin(), acc.begin() + lastSc);
                }
            }

            // 积压过大(解码跟不上)时跳到最近的 SPS+IDR, 限制延迟。
            // 阈值 2MB: 1904²@32Mbps 的单个大关键帧可达数百KB, 阈值太低会误跳
            // 跳帧必须保留 SPS/PPS(跳到 IDR 前面的 SPS 位置), 否则花屏到下一个IDR
            if (g_framedStream && acc.size() > 2 * 1024 * 1024) {
                size_t jumpTo = SIZE_MAX;
                for (size_t k = 0; k + 4 <= acc.size();) {
                    uint32_t frameSize = ((uint32_t)acc[k] << 24) |
                                         ((uint32_t)acc[k + 1] << 16) |
                                         ((uint32_t)acc[k + 2] << 8) | acc[k + 3];
                    if (frameSize == 0 || frameSize > (8u << 20) ||
                        k + 4 + frameSize > acc.size()) break;
                    std::vector<uint8_t> au(acc.begin() + k + 4,
                                            acc.begin() + k + 4 + frameSize);
                    if (AuHasIdr(au)) jumpTo = k;
                    k += 4 + frameSize;
                }
                if (jumpTo != SIZE_MAX) {
                    acc.erase(acc.begin(), acc.begin() + jumpTo);
                    if (g_ffctx) avcodec_flush_buffers(g_ffctx);  // 跳帧后重同步
                    g_droppedFrames++;
                } else {
                    // 无 IDR 可跳: 丢掉最旧的一半, 防止内存膨胀
                    size_t cut = 0;
                    while (cut + 4 <= acc.size() && cut < acc.size() / 2) {
                        uint32_t frameSize = ((uint32_t)acc[cut] << 24) |
                                             ((uint32_t)acc[cut + 1] << 16) |
                                             ((uint32_t)acc[cut + 2] << 8) | acc[cut + 3];
                        if (frameSize == 0 || frameSize > (8u << 20) ||
                            cut + 4 + frameSize > acc.size()) break;
                        cut += 4 + frameSize;
                    }
                    if (cut > 0) acc.erase(acc.begin(), acc.begin() + cut);
                    else acc.clear();
                }
            }
        }

        if (!g_framedStream) {
            if (!acc.empty()) ParseH264(acc.data(), acc.size());
        } else {
            FlushRawH264Parser();
        }
        if (g_parser) { av_parser_close(g_parser); g_parser = nullptr; }
        g_connected = false;
        if (g_sock.load() == sock) {
            g_sock.store(INVALID_SOCKET);
            closesocket(sock);
        }
        if (!g_run) break;
        snprintf(g_err, sizeof(g_err), "连接断开(收到%lldB, 包%lld, 帧%lld, 错误%lld), 2秒后自动重连",
                 g_recvBytes, g_videoPackets.load(), g_decodedFrames.load(), g_decodeErrors.load());
        snprintf(g_status, sizeof(g_status), "连接断开, 2秒后重连");
        for (int i = 0; i < 200 && g_run; i++) Sleep(10);  // 2s 后重连
    }
}

// ---------------- 对外接口 ----------------

bool VideoPlayerStart(const char* ip) {
    if (g_run) return true;
    if (!ip || !ip[0]) return false;
    snprintf(g_ip, sizeof(g_ip), "%s", ip);

    // 粘性退出线程
    if (g_thread.joinable()) {
        g_run = false;
        SOCKET old = g_sock.exchange(INVALID_SOCKET);
        if (old != INVALID_SOCKET) closesocket(old);
        g_thread.join();
    }

    g_csd.clear();
    g_au.clear();
    g_auHasVcl = false;
    g_connected = false;
    g_err[0] = '\0';
    g_lastFrameMs = 0;
    g_decodedFrames = 0;
    g_decodeErrors = 0;
    g_videoPackets = 0;
    g_framedStream = false;
    snprintf(g_status, sizeof(g_status), "连接中 %s:56790", ip);
    g_run = true;
    g_thread = std::thread(ReaderThread);
    return true;
}

void VideoPlayerStop() {
    g_run = false;
    g_obsOn = false;
    if (g_obsThreadOn.exchange(false)) {
        if (g_obsThread.joinable()) g_obsThread.join();
    }
    {
        std::lock_guard<std::mutex> lk(g_tsMtx);
        g_obsLastKeyAu.clear();
        g_tsLastFrameSentMs = 0;
    }
    ObsClose();
    SOCKET old = g_sock.exchange(INVALID_SOCKET);
    if (old != INVALID_SOCKET) closesocket(old);
    if (g_thread.joinable()) g_thread.join();
    if (g_ffframe) {
        av_frame_free(&g_ffframe);
        g_ffframe = nullptr;
    }
    if (g_ffctx) {
        avcodec_free_context(&g_ffctx);
        g_ffctx = nullptr;
    }
    {
        std::lock_guard<std::mutex> l(g_frameMtx);
        g_back.clear();
        g_frameW = g_frameH = 0;
    }
    snprintf(g_status, sizeof(g_status), "已停止");
}

bool VideoPlayerRunning() { return g_run.load(); }

void VideoPlayerGetStatus(char* buf, size_t n) {
    if (g_err[0] != '\0') {
        snprintf(buf, n, "投屏: %s", g_err);
        return;
    }
    if (!g_run.load()) {
        snprintf(buf, n, "%s", "投屏: 未启动");
        return;
    }
    long long now = (long long)GetTickCount64();
    if (!g_connected) {
        if (g_everConnected)
            snprintf(buf, n, "投屏: 已断开");
        else
            snprintf(buf, n, "投屏: 连接中 %s:56790", g_ip);
        return;
    }
    if (g_lastFrameMs != 0 && now - g_lastFrameMs < 2000) {
        snprintf(buf, n, "投屏: %dx%d  画面正常 %dfps  收流 %dKB%s%s",
                 g_decW, g_decH, g_fps, (int)(g_recvBytes / 1024),
                 g_droppedFrames > 0 ? " 丢帧!" : "",
                  g_obsOn.load() ? "  [OBS输出: udp://127.0.0.1:9998]" : "");
    } else if (g_decW > 0) {
        snprintf(buf, n, "投屏: 已连接 %dx%d  等待画面...(收到 %dKB, 包%lld, 帧%lld, 错误%lld)",
                 g_decW, g_decH, (int)(g_recvBytes / 1024),
                 g_videoPackets.load(), g_decodedFrames.load(), g_decodeErrors.load());
    } else {
        snprintf(buf, n, "投屏: 已连接 %s  等待流头...(收到 %dKB)%s",
                 g_ip, (int)(g_recvBytes / 1024),
                 g_recvBytes == 0 ? " — 设备端投屏未开启? 请先到手机投屏页点\"开始投屏\"" : "");
    }
}
