// PORT-017 (KAP-558): cross-language contract anchor for the planner
// protocol. Decodes one payload with the C++ reference codec, validates it
// with the fail-closed validators and prints the reject name (e.g. "ok",
// "size-mismatch").
//
// Usage: planner_validator_cli <request|response> <nowMs> <hexPayload>
//
// In response mode the payload is validated against the embedded golden
// request envelope - the same vector the C++ value test and the Python fake
// (fake_planner.py) pin - which is what makes this executable the byte-exact
// cross-language reference. Exit status: 0 for any verdict (legal or
// rejected), 2 for usage errors.

#include "Companion/PlannerProtocol.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace CP = Companion::Planner;

static const char* kGoldenRequestHex =
    "434c5031010000008877665544332211d14e090003000000"
    "40420f000000000000000000540000000700000002000000"
    "02000000d24e090001000000d34e09000400000000000000"
    "000000000000000000000000"
;

static int HexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool FromHex(const char* hex, std::vector<uint8_t>& out)
{
    out.clear();
    if (strlen(hex) % 2 != 0)
        return false;
    for (size_t i = 0; hex[i]; i += 2)
    {
        int hi = HexVal(hex[i]);
        int lo = HexVal(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return true;
}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr,
                     "usage: planner_validator_cli <request|response> <nowMs> <hexPayload>\n");
        return 2;
    }
    const bool isRequest = strcmp(argv[1], "request") == 0;
    const bool isResponse = strcmp(argv[1], "response") == 0;
    if (!isRequest && !isResponse)
    {
        std::fprintf(stderr, "mode must be request or response\n");
        return 2;
    }
    char* end = nullptr;
    const unsigned long long now = strtoull(argv[2], &end, 10);
    if (!end || *end != '\0')
    {
        std::fprintf(stderr, "nowMs must be a decimal integer\n");
        return 2;
    }
    std::vector<uint8_t> payload;
    if (!FromHex(argv[3], payload))
    {
        std::fprintf(stderr, "hexPayload must be non-empty even-length hex\n");
        return 2;
    }

    CP::Verdict v;
    if (isRequest)
    {
        if (payload.size() < CP::kEnvelopeBytes)
        {
            std::printf("size-mismatch\n");
            return 0;
        }
        CP::Envelope env = CP::DecodeEnvelope(payload.data());
        if (payload.size() < CP::kRequestBytes)
        {
            // Shorter than the fixed request layout: the verdict is
            // SizeMismatch and the body is never touched.
            v = CP::ValidateRequest(env, CP::RequestBody{},
                                    (uint32_t)payload.size(), (uint64_t)now);
        }
        else
        {
            CP::RequestBody body =
                CP::DecodeRequestBody(payload.data() + CP::kEnvelopeBytes);
            v = CP::ValidateRequest(env, body, (uint32_t)payload.size(),
                                    (uint64_t)now);
        }
    }
    else
    {
        if (payload.size() < CP::kEnvelopeBytes)
        {
            std::printf("size-mismatch\n");
            return 0;
        }
        std::vector<uint8_t> reqBytes;
        if (!FromHex(kGoldenRequestHex, reqBytes))
            return 2;
        CP::Envelope req = CP::DecodeEnvelope(reqBytes.data());
        CP::Envelope res = CP::DecodeEnvelope(payload.data());
        uint32_t expected = (uint32_t)(CP::kEnvelopeBytes +
                               CP::kStepBytes * (uint64_t)res.stepCount);
        CP::Step steps[CP::kMaxSteps] = {};
        if (expected == (uint32_t)payload.size() && res.stepCount > 0 &&
            res.stepCount <= CP::kMaxSteps)
        {
            for (uint32_t i = 0; i < res.stepCount; ++i)
                steps[i] = CP::DecodeStep(payload.data() + CP::kEnvelopeBytes +
                                          CP::kStepBytes * i);
        }
        v = CP::ValidateResponse(req, res, steps, (uint32_t)payload.size(),
                                 (uint64_t)now);
    }
    std::printf("%s\n", CP::RejectName(v.reject));
    return 0;
}
