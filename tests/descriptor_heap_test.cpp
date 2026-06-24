#include "engine/rhi/d3d12/FixedDescriptorHeap.h"

#include "test_expect.h"

#include <iostream>

int main()
{
    FixedDescriptorHeap heap(8);

    test::ExpectTrue(heap.Capacity() == 8, "Capacity should match constructor argument");
    test::ExpectTrue(heap.UsedCount() == 0, "New heap should be empty");

    const std::uint32_t first = heap.AllocateOne();
    const std::uint32_t second = heap.AllocateOne();
    test::ExpectTrue(first == 0, "First allocation should use index 0");
    test::ExpectTrue(second == 1, "Second allocation should use index 1");
    test::ExpectTrue(heap.UsedCount() == 2, "Used count should track single allocations");

    const std::uint32_t block = heap.AllocateBlock(3);
    test::ExpectTrue(block == 2, "Block allocation should pack after existing slots");
    test::ExpectTrue(heap.IsAllocated(2) && heap.IsAllocated(3) && heap.IsAllocated(4), "Block slots should be marked used");

    heap.FreeOne(first);
    test::ExpectTrue(!heap.IsAllocated(first), "Freed slot should be available again");
    test::ExpectTrue(heap.UsedCount() == 4, "Used count should decrease after free");

    const std::uint32_t reused = heap.AllocateOne();
    test::ExpectTrue(reused == 0, "Freed slot should be reused before expanding");

    heap.FreeBlock(block, 3);
    test::ExpectTrue(heap.UsedCount() == 2, "Block free should release all slots in the range");

    const std::uint32_t largeBlock = heap.AllocateBlock(4);
    test::ExpectTrue(largeBlock != FixedDescriptorHeap::kInvalid, "Large block should fit after freeing indices 2-4");
    test::ExpectTrue(heap.UsedCount() == 6, "Used count should reflect large block allocation");

    const std::uint32_t remaining = heap.AllocateOne();
    test::ExpectTrue(remaining != FixedDescriptorHeap::kInvalid, "One slot should remain after prior allocations");
    const std::uint32_t finalSlot = heap.AllocateOne();
    test::ExpectTrue(finalSlot != FixedDescriptorHeap::kInvalid, "Final free slot should allocate");
    test::ExpectTrue(heap.AllocateOne() == FixedDescriptorHeap::kInvalid, "Heap should return invalid when full");
    test::ExpectTrue(!heap.IsValidIndex(FixedDescriptorHeap::kInvalid), "Invalid index must not be passed to GPU handle math");

    const std::uint32_t usedBeforeInvalidFree = heap.UsedCount();
    heap.FreeBlock(FixedDescriptorHeap::kInvalid, 1);
    heap.FreeOne(FixedDescriptorHeap::kInvalid);
    test::ExpectTrue(heap.UsedCount() == usedBeforeInvalidFree, "Freeing invalid indices should be a no-op");

    FixedDescriptorHeap tiny(2);
    test::ExpectTrue(tiny.AllocateBlock(3) == FixedDescriptorHeap::kInvalid, "Oversized block request should fail");
    test::ExpectTrue(tiny.AllocateOne() == 0 && tiny.AllocateOne() == 1, "Tiny heap should allocate two slots");
    test::ExpectTrue(tiny.AllocateOne() == FixedDescriptorHeap::kInvalid, "Tiny heap should exhaust after two slots");

    if (test::FailureCount() == 0)
    {
        std::cout << "All descriptor heap tests passed.\n";
    }

    return test::ExitCode();
}
