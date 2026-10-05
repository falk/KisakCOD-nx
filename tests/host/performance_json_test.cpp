#include "../../src/platform/switch/switch_profile_format.h"
#include <cassert>
#include <cstring>
#include <cstdio>

int main()
{
    kisakperf::Json escaped;
    escaped.Raw("{\"name\":");
    escaped.String("quote\"\\\n\t");
    escaped.Raw("}");
    assert(!std::strcmp(escaped.Line(), "KPERF {\"name\":\"quote\\\"\\\\\\u000a\\u0009\"}"));
    kisakperf::Json overflow;
    for (int i = 0; i < 1100; ++i)
        overflow.Raw("x");
    assert(std::strstr(overflow.Line(), "\"name\":\"overflow\""));
    assert(std::strlen(overflow.Line()) < 1000);
    kisakperf::Json gpu;
    kisakperf::Samples(gpu, "gpu.busy", "busy", 1,
                      "\"frame\",\"time\",\"value\",\"width\",\"height\",\"list_seq\"");
    for (int i = 0; i < 8; ++i)
        gpu.Raw("%s[4294967295,1844674407370955,4294967295,65535,65535,18446744073709551615]", i ? "," : "");
    gpu.Raw("],\"tags\":{\"timing\":\"sum_of_list_busy\",\"alignment\":\"retirement_observation\",\"frame_domain\":\"gpu\"}}");
    assert(!std::strstr(gpu.Line(), "\"name\":\"overflow\""));
    puts(overflow.Line());
    puts(gpu.Line());
    puts("PASS:PERFORMANCE_JSON");
}
