/*
 * NxFrame
 * Copyright (c) 2026 Michalis Michael. All rights reserved.
 *
 * This file is part of the NxFrame source distribution. Use, copying,
 * modification and redistribution are governed by the project license / EULA
 * supplied with the repository. Do not remove this notice from source copies.
 *
 * File: tests/test_bounded_queue_policy.cpp
 * Description: NxFrame regression and validation tests.
 */

#include "core/bounded_queue.h"

#include <cstdlib>
#include <iostream>

static void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "bounded queue policy test failed: " << message << "\n";
        std::exit(1);
    }
}

int main()
{
    BoundedQueue<int> q(2);
    require(q.push_with_policy(1, QueueOverflowPolicy::DropOldest) == QueuePushResult::Pushed,
            "first DropOldest push should be accepted");
    require(q.push_with_policy(2, QueueOverflowPolicy::DropOldest) == QueuePushResult::Pushed,
            "second DropOldest push should be accepted");
    require(q.push_with_policy(3, QueueOverflowPolicy::DropOldest) == QueuePushResult::DroppedOldestAndPushed,
            "full DropOldest queue should discard oldest item and accept new item");

    int v = 0;
    require(q.pop(v) && v == 2, "DropOldest queue should retain second item");
    require(q.pop(v) && v == 3, "DropOldest queue should retain newest item");

    require(q.push_with_policy(4, QueueOverflowPolicy::DropNewest) == QueuePushResult::Pushed,
            "first DropNewest push should be accepted");
    require(q.push_with_policy(5, QueueOverflowPolicy::DropNewest) == QueuePushResult::Pushed,
            "second DropNewest push should be accepted");
    require(q.push_with_policy(6, QueueOverflowPolicy::DropNewest) == QueuePushResult::DroppedNewest,
            "full DropNewest queue should reject newest item");
    require(q.pop(v) && v == 4, "DropNewest queue should retain oldest item");
    require(q.pop(v) && v == 5, "DropNewest queue should retain second item");

    q.stop();
    require(q.push_with_policy(7, QueueOverflowPolicy::DropOldest) == QueuePushResult::Stopped,
            "stopped queue should reject new pushes");

    std::cout << "bounded queue live policy test passed\n";
    return 0;
}
