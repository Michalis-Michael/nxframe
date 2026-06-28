#include "core/pipeline_telemetry.h"

#include <atomic>
#include <iostream>
#include <sstream>
#include <string>

namespace {

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    PipelineTelemetry telemetry;

    std::ostringstream captured;
    std::streambuf* old = std::cout.rdbuf(captured.rdbuf());

    telemetry.missVideo.fetch_add(49, std::memory_order_relaxed);
    telemetry.report(0, 0, 0, 0);
    const std::string first = captured.str();

    captured.str(std::string());
    captured.clear();

    telemetry.report(0, 0, 0, 0);
    const std::string second = captured.str();

    std::cout.rdbuf(old);

    if (!contains(first, "enc_empty[v/a]=49/0")) {
        std::cerr << "first report did not include interval encoder-empty count: " << first << "\n";
        return 1;
    }
    if (!contains(first, "enc_empty_total[v/a]=49/0")) {
        std::cerr << "first report did not include cumulative encoder-empty count: " << first << "\n";
        return 1;
    }
    if (!contains(second, "enc_empty[v/a]=0/0")) {
        std::cerr << "second report should show no new encoder-empty events: " << second << "\n";
        return 1;
    }
    if (!contains(second, "enc_empty_total[v/a]=49/0")) {
        std::cerr << "second report should retain cumulative encoder-empty total: " << second << "\n";
        return 1;
    }

    return 0;
}
