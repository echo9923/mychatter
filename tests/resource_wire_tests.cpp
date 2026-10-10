// resource_wire_tests.cpp — P1c 线协议 v2 编解码往返单元测试。
// 只依赖 proto/resource_wire.h（纯 C++11 标准库），不建 socket/不依赖 Qt。
#include <cstdio>
#include <string>

#include "resource_wire.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
    else {
        std::printf("ok: %s\n", what);
    }
}

//往返：encode → decode 得到同一 meta/raw
void TestRoundTrip()
{
    const std::string meta = "{\"error\":0,\"message_id\":\"9100000001\","
        "\"offset\":\"0\",\"bytes\":\"262144\",\"chunk_sha256\":\"abc\","
        "\"total_size\":\"1048576\",\"is_last\":false}";
    const std::string raw(1024, '\x5A');
    std::string frame;
    Check(llfc_wire::EncodeFrame(meta, raw, &frame), "encode succeeds");
    Check(frame.size() == 2 + meta.size() + raw.size(), "frame size exact");
    //meta_len 大端：首字节为高 8 位
    const unsigned hi = static_cast<unsigned char>(frame[0]);
    const unsigned lo = static_cast<unsigned char>(frame[1]);
    Check(((hi << 8) | lo) == meta.size(), "meta_len big-endian");

    std::string meta2;
    std::string raw2;
    Check(llfc_wire::DecodeFrame(frame, &meta2, &raw2), "decode succeeds");
    Check(meta2 == meta && raw2 == raw, "round-trip identical");

    //空 raw（1512 错误分支）
    std::string err_frame;
    Check(llfc_wire::EncodeFrame(meta, std::string(), &err_frame),
        "encode empty raw succeeds");
    std::string em, er;
    Check(llfc_wire::DecodeFrame(err_frame, &em, &er) && em == meta && er.empty(),
        "empty raw round-trip");

    //预算双向校验：空 meta/超限 meta/超限 raw/总帧超限一律拒绝且不改 out
    const std::string big(llfc_wire::kMaxMetaLen + 1, 'x');
    std::string out;
    out = "untouched";
    Check(!llfc_wire::EncodeFrame(big, raw, &out) && out == "untouched",
        "oversized meta rejected, out untouched");
    Check(!llfc_wire::EncodeFrame(std::string(), raw, &out),
        "empty meta rejected");
    const std::string too_big_raw(llfc_wire::kResourceChunkSize + 1, 'y');
    Check(!llfc_wire::EncodeFrame(meta, too_big_raw, &out),
        "oversized raw rejected");
    Check(!llfc_wire::EncodeFrame(meta, raw, 0), "null out rejected");
}

//边界：截断帧/坏头/超长 meta_len
void TestBoundaries()
{
    std::uint16_t meta_len = 0;
    Check(!llfc_wire::PeekMetaLen("", 1, &meta_len), "short head rejected");
    Check(llfc_wire::PeekMetaLen("\x01\x02", 2, &meta_len) && meta_len == 0x0102,
        "peek big-endian");

    //meta_len 声明超过帧长 → 拒绝
    std::string frame;
    llfc_wire::EncodeFrame(std::string("{\"a\":1}"), std::string(4, 'a'), &frame);
    frame[0] = static_cast<char>(0xFF); //声明 65535 字节 meta
    frame[1] = static_cast<char>(0xFF);
    std::string m, r;
    Check(!llfc_wire::DecodeFrame(frame, &m, &r), "truncated frame rejected");

    //空帧/仅前缀拒绝；raw 超分片预算拒绝且不改输出
    Check(!llfc_wire::DecodeFrame(std::string(), &m, &r), "empty frame rejected");
    Check(!llfc_wire::DecodeFrame(std::string("\x00\x01", 2), &m, &r),
        "header-only frame rejected");
    //over-raw 用例需要一个小 meta（本函数局部；TestRoundTrip 的 meta 不在此作用域）
    const std::string meta("{\"a\":1}");
    std::string over_raw;
    llfc_wire::EncodeFrame(meta, std::string(llfc_wire::kResourceChunkSize, 'q'),
        &over_raw);
    over_raw += 'q'; //帧总长仍 < kMaxFrameBody（meta 很小），但 raw 超 256KiB
    m = "keep"; r = "keep";
    Check(!llfc_wire::DecodeFrame(over_raw, &m, &r) && m == "keep" && r == "keep",
        "raw over chunk budget rejected, outputs untouched");

    //最大分片帧：256KiB raw 可整帧往返（帧体预算内）
    const std::string max_raw(llfc_wire::kResourceChunkSize, 'Z');
    std::string max_frame;
    Check(llfc_wire::EncodeFrame(std::string("{\"a\":1}"), max_raw, &max_frame)
        && max_frame.size() <= llfc_wire::kMaxFrameBody,
        "256KiB chunk within frame budget");
    std::string m2, r2;
    Check(llfc_wire::DecodeFrame(max_frame, &m2, &r2) && r2 == max_raw,
        "256KiB round-trip");
}

} // namespace

int main()
{
    TestRoundTrip();
    TestBoundaries();
    std::printf("\n%s: %d failure(s)\n",
        g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
