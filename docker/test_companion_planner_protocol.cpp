// PORT-017 (KAP-558): value-level coverage for the versioned planner
// protocol (Companion/PlannerProtocol.h). Standalone: no engine, no
// Docker. Compiled and run by docker/test_companion_planner_protocol.py.
//
// The golden vectors below are byte-identical with the Python reference
// encoder (docker/personality-service/fake_planner.py) and with the
// cross-language anchor (docker/personality-service/planner_validator_cli.cpp).
// Both sides must stay in lockstep; the contract test enforces it.

#include "Companion/PlannerProtocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace CP = Companion::Planner;

static int g_failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            std::printf("FAIL %d: %s\n", __LINE__, #cond);         \
            ++g_failures;                                          \
        }                                                          \
    } while (0)

#define CHECK_REJECT(expr, expected)                               \
    do {                                                           \
        CP::Verdict v_ = (expr);                                   \
        if (v_.reject != (expected)) {                             \
            std::printf("FAIL %d: %s -> %s (want %s)\n", __LINE__, \
                        #expr, CP::RejectName(v_.reject),          \
                        CP::RejectName(expected));                 \
            ++g_failures;                                          \
        }                                                          \
    } while (0)

static const char* kGoldenRequestHex =
    "434c5031010000008877665544332211d14e090002000000"
    "40420f000000000000000000540000000700000002000000"
    "02000000d24e090001000000d34e09000400000000000000"
    "000000000000000000000000"
;

static const char* kGoldenResponseHex =
    "434c5031010000008877665544332211d14e090002000000"
    "104a0f00000000000200000068000000d24e090001000000"
    "070000000100000000000000f84d0f000000000000000000"
    "d34e0900010000000700000003000000c8252600f84d0f00"
    "0000000000000000"
;

static const uint64_t kGoldenNowMs = 1002000; // 2000 ms after the request capture

// ---------------------------------------------------------------------------

static int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::vector<uint8_t> FromHex(const char* hex)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
    {
        int hi = HexVal(hex[i]);
        int lo = HexVal(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return std::vector<uint8_t>();
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return out;
}

static void TestConstantsAndLayout()
{
    CHECK(CP::kProtocolVersion == 1);
    CHECK(CP::kMagic == 0x31504C43u);
    CHECK(CP::kEnvelopeBytes == 40);
    CHECK(CP::kRequestBodyBytes == 44);
    CHECK(CP::kStepBytes == 32);
    CHECK(CP::kRequestBytes == 84);
    CHECK(CP::kMaxResponseBytes == 296);
    CHECK(CP::kMaxSteps == 8);
    CHECK(CP::kMaxPartyBots == 4);
    CHECK(CP::kMaxPayloadBytes == 4096);
    CHECK(CP::kMaxResponseAgeMs == 1000);
    CHECK(CP::kMaxStepLifetimeMs == 5000);
    CHECK(CP::kMaxPreference == 255);
    CHECK(CP::kActionCount == 8);
    CHECK(sizeof(CP::Envelope) == 40);
    CHECK(sizeof(CP::BotSlot) == 8);
    CHECK(sizeof(CP::RequestBody) == 44);
    CHECK(sizeof(CP::Step) == 32);
    CHECK(CP::RequiresTarget(CP::Action::Assist));
    CHECK(CP::RequiresTarget(CP::Action::Loot));
    CHECK(!CP::RequiresTarget(CP::Action::None));
    CHECK(!CP::RequiresTarget(CP::Action::Hold));
    CHECK(!CP::RequiresTarget(CP::Action::Follow));
    CHECK(!CP::RequiresTarget(CP::Action::Defend));
    CHECK(!CP::RequiresTarget(CP::Action::Regroup));
    CHECK(!CP::RequiresTarget(CP::Action::Preference));
}

static void TestRoundTrip()
{
    CP::Envelope e;
    e.magic = CP::kMagic;
    e.protocolVersion = 2; // the codec is value-blind; validation is separate
    e.requestId = 0x0123456789ABCDEFULL;
    e.ownerGuid = 424242;
    e.observationVersion = 9;
    e.captureTimeMs = 0x0000DEADBEEF0001ULL;
    e.stepCount = 7;
    e.totalSize = CP::kEnvelopeBytes + 7 * CP::kStepBytes;
    uint8_t buf[CP::kEnvelopeBytes] = {};
    CP::EncodeEnvelope(buf, e);
    CP::Envelope d = CP::DecodeEnvelope(buf);
    CHECK(d.magic == e.magic);
    CHECK(d.protocolVersion == e.protocolVersion);
    CHECK(d.requestId == e.requestId);
    CHECK(d.ownerGuid == e.ownerGuid);
    CHECK(d.observationVersion == e.observationVersion);
    CHECK(d.captureTimeMs == e.captureTimeMs);
    CHECK(d.stepCount == e.stepCount);
    CHECK(d.totalSize == e.totalSize);

    CP::RequestBody b;
    b.generation = 321;
    b.flags = 5;
    b.botCount = 3;
    b.bots[0].botGuid = 11;
    b.bots[0].cls = 1;
    b.bots[1].botGuid = 12;
    b.bots[1].cls = 2;
    b.bots[2].botGuid = 13;
    b.bots[2].cls = 3;
    uint8_t buf2[CP::kRequestBodyBytes] = {};
    CP::EncodeRequestBody(buf2, b);
    CP::RequestBody db = CP::DecodeRequestBody(buf2);
    CHECK(db.generation == b.generation);
    CHECK(db.flags == b.flags);
    CHECK(db.botCount == b.botCount);
    for (uint32_t i = 0; i < CP::kMaxPartyBots; ++i)
    {
        CHECK(db.bots[i].botGuid == b.bots[i].botGuid);
        CHECK(db.bots[i].cls == b.bots[i].cls);
    }

    CP::Step s;
    s.botGuid = 77;
    s.loginGeneration = 5;
    s.orderGeneration = 6;
    s.action = 3;
    s.targetGuid = 2500040;
    s.expiresAtMs = 1234567890123ULL;
    s.preference = 255;
    uint8_t buf3[CP::kStepBytes] = {};
    CP::EncodeStep(buf3, s);
    CP::Step ds = CP::DecodeStep(buf3);
    CHECK(ds.botGuid == s.botGuid);
    CHECK(ds.loginGeneration == s.loginGeneration);
    CHECK(ds.orderGeneration == s.orderGeneration);
    CHECK(ds.action == s.action);
    CHECK(ds.targetGuid == s.targetGuid);
    CHECK(ds.expiresAtMs == s.expiresAtMs);
    CHECK(ds.preference == s.preference);
}

static CP::Envelope ValidResponseEnv()
{
    CP::Envelope e;
    e.magic = CP::kMagic;
    e.protocolVersion = CP::kProtocolVersion;
    e.requestId = 9;
    e.ownerGuid = 610001;
    e.observationVersion = Companion::kObservationVersion;
    e.captureTimeMs = 1000;
    e.stepCount = 2;
    e.totalSize = CP::kEnvelopeBytes + 2 * CP::kStepBytes;
    return e;
}

static void TestEnvelopeRejects()
{
    const uint32_t full = CP::kEnvelopeBytes + 2 * CP::kStepBytes;
    CP::Envelope e = ValidResponseEnv();

    e.magic = 0xDEADBEEF;
    CHECK_REJECT(CP::ValidateEnvelope(e, full, full, false, 1000), CP::Reject::BadMagic);

    e = ValidResponseEnv();
    e.protocolVersion = 2;
    CHECK_REJECT(CP::ValidateEnvelope(e, full, full, false, 1000), CP::Reject::UnknownVersion);

    e = ValidResponseEnv();
    e.observationVersion = 99;
    CHECK_REJECT(CP::ValidateEnvelope(e, full, full, false, 1000), CP::Reject::UnknownVersion);

    // received length disagrees with the layout, both directions
    e = ValidResponseEnv();
    CHECK_REJECT(CP::ValidateEnvelope(e, full + 4, full, false, 1000), CP::Reject::SizeMismatch);
    CHECK_REJECT(CP::ValidateEnvelope(e, full - 4, full, false, 1000), CP::Reject::SizeMismatch);

    // the ceiling fires before any field parse
    e = ValidResponseEnv();
    CHECK_REJECT(CP::ValidateEnvelope(e, CP::kMaxPayloadBytes + 1, full, false, 1000), CP::Reject::Oversized);

    e = ValidResponseEnv();
    e.requestId = 0;
    CHECK_REJECT(CP::ValidateEnvelope(e, full, full, false, 1000), CP::Reject::BadId);
    e = ValidResponseEnv();
    e.ownerGuid = 0;
    CHECK_REJECT(CP::ValidateEnvelope(e, full, full, false, 1000), CP::Reject::BadId);
    e = ValidResponseEnv();
    e.captureTimeMs = 0;
    CHECK_REJECT(CP::ValidateEnvelope(e, full, full, false, 1000), CP::Reject::BadId);

    // step count range (the sizes line up so the count check is what fires)
    e = ValidResponseEnv();
    e.stepCount = 0;
    e.totalSize = CP::kEnvelopeBytes;
    CHECK_REJECT(CP::ValidateEnvelope(e, CP::kEnvelopeBytes, CP::kEnvelopeBytes, false, 1000),
                 CP::Reject::TooManySteps);
    e = ValidResponseEnv();
    e.stepCount = 9;
    e.totalSize = CP::kEnvelopeBytes + 9 * CP::kStepBytes;
    CHECK_REJECT(CP::ValidateEnvelope(e, e.totalSize, e.totalSize, false, 1000),
                 CP::Reject::TooManySteps);

    // requests carry no steps
    CP::Envelope r;
    r.magic = CP::kMagic;
    r.protocolVersion = CP::kProtocolVersion;
    r.requestId = 9;
    r.ownerGuid = 610001;
    r.observationVersion = Companion::kObservationVersion;
    r.captureTimeMs = 1000;
    r.stepCount = 1;
    r.totalSize = CP::kRequestBytes;
    CHECK_REJECT(CP::ValidateEnvelope(r, CP::kRequestBytes, CP::kRequestBytes, true, 1000),
                 CP::Reject::TooManySteps);

    // total_size disagrees with the wire length
    e = ValidResponseEnv();
    e.totalSize = full + 8;
    CHECK_REJECT(CP::ValidateEnvelope(e, full, full, false, 1000), CP::Reject::SizeMismatch);

    // age: exactly the budget is legal, one ms more is stale
    e = ValidResponseEnv();
    CHECK_REJECT(CP::ValidateEnvelope(e, full, full, false, 2000), CP::Reject::Ok);
    e = ValidResponseEnv();
    e.captureTimeMs = 999;
    CHECK_REJECT(CP::ValidateEnvelope(e, full, full, false, 2000), CP::Reject::Stale);
}

static CP::RequestBody ValidBody()
{
    CP::RequestBody b;
    b.generation = 7;
    b.flags = 2;
    b.botCount = 2;
    b.bots[0].botGuid = 610002;
    b.bots[0].cls = 1;
    b.bots[1].botGuid = 610003;
    b.bots[1].cls = 4;
    return b;
}

static void TestRequestBodyRejects()
{
    CP::RequestBody b = ValidBody();
    CHECK_REJECT(CP::ValidateRequestBody(b), CP::Reject::Ok);

    b = ValidBody();
    b.reserved[1] = 1;
    CHECK_REJECT(CP::ValidateRequestBody(b), CP::Reject::BadReserved);

    b = ValidBody();
    b.botCount = 0;
    CHECK_REJECT(CP::ValidateRequestBody(b), CP::Reject::TooManySteps);
    b = ValidBody();
    b.botCount = 5;
    CHECK_REJECT(CP::ValidateRequestBody(b), CP::Reject::TooManySteps);

    b = ValidBody();
    b.bots[0].botGuid = 0;
    CHECK_REJECT(CP::ValidateRequestBody(b), CP::Reject::BadId);

    b = ValidBody();
    b.bots[0].reserved[2] = 7;
    CHECK_REJECT(CP::ValidateRequestBody(b), CP::Reject::BadReserved);

    b = ValidBody();
    b.bots[3].botGuid = 99; // unused slots must stay zero
    CHECK_REJECT(CP::ValidateRequestBody(b), CP::Reject::BadReserved);
}

static CP::Envelope StepEnv()
{
    CP::Envelope e = ValidResponseEnv();
    e.captureTimeMs = 1000;
    e.stepCount = 1;
    e.totalSize = CP::kEnvelopeBytes + CP::kStepBytes;
    return e;
}

static CP::Step ValidStep(uint64_t nowMs)
{
    CP::Step s;
    s.botGuid = 610002;
    s.loginGeneration = 1;
    s.orderGeneration = 7;
    s.action = 1; // Hold: no target
    s.targetGuid = 0;
    s.expiresAtMs = nowMs + 500;
    s.preference = 0;
    return s;
}

static void TestStepRejects()
{
    const uint64_t nowMs = 1500;
    CP::Envelope env = StepEnv();

    CP::Step s = ValidStep(nowMs);
    CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::Ok);

    s = ValidStep(nowMs);
    s.botGuid = 0;
    CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::BadId);

    s = ValidStep(nowMs);
    s.reserved[0] = 1;
    CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::BadReserved);

    for (uint32_t a = 8; a < 256; ++a)
    {
        s = ValidStep(nowMs);
        s.action = (uint8_t)a;
        CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::UnknownAction);
    }

    s = ValidStep(nowMs);
    s.action = 3;
    s.targetGuid = 0;
    CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::MissingTarget);
    s = ValidStep(nowMs);
    s.action = 5;
    s.targetGuid = 0;
    CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::MissingTarget);

    for (uint32_t a = 0; a < 8; ++a)
    {
        if (a == 3 || a == 5)
            continue;
        s = ValidStep(nowMs);
        s.action = (uint8_t)a;
        s.targetGuid = 42;
        CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::ForbiddenTarget);
    }

    s = ValidStep(nowMs);
    s.preference = 255;
    CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::Ok);
    s = ValidStep(nowMs);
    s.preference = 256;
    CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::OutOfRange);

    s = ValidStep(nowMs);
    s.expiresAtMs = nowMs; // deadline inclusive: arrival at the deadline is late
    CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::Expired);
    s = ValidStep(nowMs);
    s.expiresAtMs = nowMs + 1;
    CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::Ok);
    s = ValidStep(nowMs);
    s.expiresAtMs = 100; // already past
    CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::Expired);

    // lifetime bound relative to the capture
    s = ValidStep(nowMs);
    s.expiresAtMs = env.captureTimeMs + CP::kMaxStepLifetimeMs; // boundary, legal
    CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::Ok);
    s = ValidStep(nowMs);
    s.expiresAtMs = env.captureTimeMs + CP::kMaxStepLifetimeMs + 1;
    CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::Expired);
}

static void TestResponseRejects()
{
    std::vector<uint8_t> reqB = FromHex(kGoldenRequestHex);
    std::vector<uint8_t> resB = FromHex(kGoldenResponseHex);
    CP::Envelope req = CP::DecodeEnvelope(reqB.data());
    CP::Envelope res = CP::DecodeEnvelope(resB.data());
    CP::Step steps[2] = {CP::DecodeStep(resB.data() + CP::kEnvelopeBytes),
                         CP::DecodeStep(resB.data() + CP::kEnvelopeBytes + CP::kStepBytes)};
    const uint32_t full = (uint32_t)resB.size();

    CHECK_REJECT(CP::ValidateResponse(req, res, steps, full, kGoldenNowMs), CP::Reject::Ok);

    res.requestId = req.requestId + 1;
    CHECK_REJECT(CP::ValidateResponse(req, res, steps, full, kGoldenNowMs), CP::Reject::Mismatched);

    res = CP::DecodeEnvelope(resB.data());
    res.ownerGuid = req.ownerGuid + 1;
    CHECK_REJECT(CP::ValidateResponse(req, res, steps, full, kGoldenNowMs), CP::Reject::Mismatched);

    // one bad step rejects the whole response (all-or-nothing)
    res = CP::DecodeEnvelope(resB.data());
    CP::Step bad = steps[1];
    bad.action = 200;
    CP::Step arr[2] = {steps[0], bad};
    CHECK_REJECT(CP::ValidateResponse(req, res, arr, full, kGoldenNowMs), CP::Reject::UnknownAction);
}

static void TestGoldenVectors()
{
    std::vector<uint8_t> reqB = FromHex(kGoldenRequestHex);
    std::vector<uint8_t> resB = FromHex(kGoldenResponseHex);
    CHECK(reqB.size() == 84);
    CHECK(resB.size() == 104);

    CP::Envelope req = CP::DecodeEnvelope(reqB.data());
    CHECK(req.magic == CP::kMagic);
    CHECK(req.protocolVersion == 1);
    CHECK(req.requestId == 0x1122334455667788ULL);
    CHECK(req.ownerGuid == 610001);
    CHECK(req.observationVersion == 2);
    CHECK(req.observationVersion == Companion::kObservationVersion);
    CHECK(req.captureTimeMs == 1000000);
    CHECK(req.stepCount == 0);
    CHECK(req.totalSize == 84);
    CP::RequestBody body = CP::DecodeRequestBody(reqB.data() + CP::kEnvelopeBytes);
    CHECK(body.generation == 7);
    CHECK(body.flags == 2);
    CHECK(body.botCount == 2);
    CHECK(body.bots[0].botGuid == 610002);
    CHECK(body.bots[0].cls == 1);
    CHECK(body.bots[1].botGuid == 610003);
    CHECK(body.bots[1].cls == 4);
    CHECK_REJECT(CP::ValidateRequest(req, body, 84, kGoldenNowMs), CP::Reject::Ok);

    CP::Envelope res = CP::DecodeEnvelope(resB.data());
    CHECK(res.stepCount == 2);
    CHECK(res.captureTimeMs == 1002000);
    CHECK(res.totalSize == 104);
    CP::Step s0 = CP::DecodeStep(resB.data() + CP::kEnvelopeBytes);
    CP::Step s1 = CP::DecodeStep(resB.data() + CP::kEnvelopeBytes + CP::kStepBytes);
    CHECK(s0.botGuid == 610002);
    CHECK(s0.loginGeneration == 1);
    CHECK(s0.orderGeneration == 7);
    CHECK(s0.action == 1);   // Hold
    CHECK(s0.targetGuid == 0);
    CHECK(s0.expiresAtMs == 1003000);
    CHECK(s1.botGuid == 610003);
    CHECK(s1.action == 3);   // Assist
    CHECK(s1.targetGuid == 2500040);
    CHECK(s1.expiresAtMs == 1003000);
    CP::Step arr[2] = {s0, s1};
    CHECK_REJECT(CP::ValidateResponse(req, res, arr, 104, kGoldenNowMs), CP::Reject::Ok);

    // encode-decode-encode idempotence: re-encoding the decoded values
    // reproduces the golden bytes exactly.
    uint8_t out[40 + 2 * 32] = {};
    CP::EncodeEnvelope(out, req);
    CP::EncodeRequestBody(out + 40, body);
    CHECK(memcmp(out, reqB.data(), 84) == 0);
    CP::EncodeEnvelope(out, res);
    CP::EncodeStep(out + 40, s0);
    CP::EncodeStep(out + 72, s1);
    CHECK(memcmp(out, resB.data(), 104) == 0);
}

static void TestRejectNames()
{
    const CP::Reject all[15] = {
        CP::Reject::Ok, CP::Reject::BadMagic, CP::Reject::UnknownVersion,
        CP::Reject::SizeMismatch, CP::Reject::Oversized, CP::Reject::TooManySteps,
        CP::Reject::BadId, CP::Reject::BadReserved, CP::Reject::UnknownAction,
        CP::Reject::MissingTarget, CP::Reject::ForbiddenTarget,
        CP::Reject::OutOfRange, CP::Reject::Expired, CP::Reject::Stale,
        CP::Reject::Mismatched};
    const char* names[15] = {
        "ok", "bad-magic", "unknown-version", "size-mismatch", "oversized",
        "too-many-steps", "bad-id", "bad-reserved", "unknown-action",
        "missing-target", "forbidden-target", "out-of-range", "expired",
        "stale", "mismatched"};
    for (int i = 0; i < 15; ++i)
        CHECK(strcmp(CP::RejectName(all[i]), names[i]) == 0);
}

static void TestVocabularyClosure()
{
    const uint64_t nowMs = 1500;
    CP::Envelope env = StepEnv();
    for (uint32_t a = 0; a < 8; ++a)
    {
        CP::Step s = ValidStep(nowMs);
        s.action = (uint8_t)a;
        s.targetGuid = CP::RequiresTarget(CP::Action(a)) ? 2500040 : 0;
        CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::Ok);
    }
    for (uint32_t a = 8; a < 256; ++a)
    {
        CP::Step s = ValidStep(nowMs);
        s.action = (uint8_t)a;
        CHECK_REJECT(CP::ValidateStep(s, env, nowMs), CP::Reject::UnknownAction);
    }
}

static void TestBoundedFuzz()
{
    std::vector<uint8_t> resB = FromHex(kGoldenResponseHex);
    std::vector<uint8_t> reqB = FromHex(kGoldenRequestHex);
    CP::Envelope req = CP::DecodeEnvelope(reqB.data());
    // Deterministic LCG (seed 12345): 400 single-byte mutations of the
    // golden response. The validators must never crash on any mutation and
    // the mutations must reach a spread of distinct reject classes.
    uint64_t state = 12345;
    bool seen[15] = {false};
    int distinct = 0;
    for (int i = 0; i < 400; ++i)
    {
        state = state * 1664525ULL + 1013904223ULL;
        size_t idx = (size_t)((state >> 8) % resB.size());
        uint8_t val = (uint8_t)((state >> 24) & 0xFF);
        std::vector<uint8_t> mut = resB;
        mut[idx] = val;
        CP::Envelope res = CP::DecodeEnvelope(mut.data());
        uint32_t expected = (uint32_t)(CP::kEnvelopeBytes +
                               CP::kStepBytes * (uint64_t)res.stepCount);
        CP::Step steps[CP::kMaxSteps] = {};
        CP::Verdict v;
        if (expected == (uint32_t)mut.size() && res.stepCount > 0 &&
            res.stepCount <= CP::kMaxSteps)
        {
            for (uint32_t j = 0; j < res.stepCount; ++j)
                steps[j] = CP::DecodeStep(mut.data() + CP::kEnvelopeBytes +
                                          CP::kStepBytes * j);
            v = CP::ValidateResponse(req, res, steps, (uint32_t)mut.size(), kGoldenNowMs);
        }
        else
        {
            v = CP::ValidateResponse(req, res, nullptr, (uint32_t)mut.size(), kGoldenNowMs);
        }
        int r = (int)v.reject;
        if (r >= 0 && r < 15 && !seen[r])
        {
            seen[r] = true;
            ++distinct;
        }
    }
    std::printf("fuzz distinct rejects: %d\n", distinct);
    CHECK(distinct >= 6);
}

int main()
{
    TestConstantsAndLayout();
    TestRoundTrip();
    TestEnvelopeRejects();
    TestRequestBodyRejects();
    TestStepRejects();
    TestResponseRejects();
    TestGoldenVectors();
    TestRejectNames();
    TestVocabularyClosure();
    TestBoundedFuzz();
    if (g_failures == 0)
        std::printf("value tests: ALL OK\n");
    else
        std::printf("value tests: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
