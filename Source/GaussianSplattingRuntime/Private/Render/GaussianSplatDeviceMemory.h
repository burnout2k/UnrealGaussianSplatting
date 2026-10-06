#pragma once

// What the device says about its own memory, through VK_EXT_memory_budget.
//
// Two things ask: the Step 0 probe, which reports it, and the page pool, which
// SIZES ITSELF from it (plan D3). They must agree on which heaps count as VRAM,
// so the query lives here rather than in either of them.
//
// Moved verbatim out of GaussianSplatProbe.cpp; nothing changed.

#include "CoreMinimal.h"
#include "DynamicRHI.h"

#if GAUSSIANSPLAT_WITH_VULKAN
#include "IVulkanDynamicRHI.h"
#endif

namespace GaussianSplatDeviceMemory
{
    // VK_EXT_memory_budget over the device-local heaps of at least 1 GiB (this card also has a 246 MB device-local,
    // host-visible BAR heap, which is not VRAM for our purposes).
    struct FDeviceMemory
    {
        uint64 Usage = 0;
        uint64 Budget = 0;
        uint64 Size = 0;
    };

    inline bool QueryDeviceMemory(FDeviceMemory& Out)
    {
        Out = FDeviceMemory();
#if GAUSSIANSPLAT_WITH_VULKAN
        if (GDynamicRHI == nullptr || RHIGetInterfaceType() != ERHIInterfaceType::Vulkan)
        {
            return false;
        }
        IVulkanDynamicRHI* Rhi = GetIVulkanDynamicRHI();
        const auto GetProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
            Rhi->RHIGetVkInstanceProcAddr("vkGetPhysicalDeviceMemoryProperties2"));
        if (GetProperties2 == nullptr)
        {
            return false;
        }
        VkPhysicalDeviceMemoryBudgetPropertiesEXT Budget{};
        Budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        VkPhysicalDeviceMemoryProperties2 Properties{};
        Properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        Properties.pNext = &Budget;
        GetProperties2(Rhi->RHIGetVkPhysicalDevice(), &Properties);
        for (uint32 Heap = 0; Heap < Properties.memoryProperties.memoryHeapCount; ++Heap)
        {
            const VkMemoryHeap& Info = Properties.memoryProperties.memoryHeaps[Heap];
            if ((Info.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 && Info.size >= (1ull << 30))
            {
                Out.Usage += Budget.heapUsage[Heap];
                Out.Budget += Budget.heapBudget[Heap];
                Out.Size += Info.size;
            }
        }
        return Out.Size > 0;
#else
        return false;
#endif
    }

    inline double ToMiB(uint64 Bytes)
    {
        return static_cast<double>(Bytes) / (1024.0 * 1024.0);
    }
}
