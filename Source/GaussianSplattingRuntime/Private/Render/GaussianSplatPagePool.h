#pragma once

// ONE pool of splat slots for the whole process (plan D3).
//
// Not one per world: a pool per UWorld would mean two of them during PIE and a
// fresh one on every map change, which on a 10 GB card is the difference between
// working and not. Worlds and assets are clients -- they register and unregister
// -- and a level switch invalidates the page table instead of freeing memory.
//
// Layout: three parallel structured buffers indexed by the same slot number.
//
//   PackedA   16 B/slot   position, scale, rotation
//   PackedB    4 B/slot   colour and opacity
//   SHIndex    4 B/slot   index into the asset's palette
//
// The slot space holds two kinds of allocation. FLOOR blocks are variable-size,
// pinned ranges -- a cell's most important splats, which are never evicted, so a
// visible cell always has something to draw. PAGES are fixed 4,096-slot blocks
// handed out from a free list; they are what streaming will move. Both come from
// one allocator, because assets register after the pool exists and a fixed floor
// prefix would have to guess their total size in advance.

#include "CoreMinimal.h"
#include "RenderResource.h"
#include "RenderGraphResources.h"
#include "RendererInterface.h"

class UGaussianSplatPagedAsset;

// Where one asset's splats live in the pool.
struct FGaussianSplatPoolResidency
{
    const UGaussianSplatPagedAsset* Asset = nullptr;

    // The pinned floor block: one contiguous range for the whole asset, in the
    // same cell order as the asset's own floor block, so cell c's floor is at
    // FloorFirstSlot + Asset->GetFloorFirstSlot(c).
    int64 FloorFirstSlot = 0;
    int64 FloorSlots = 0;

    // Per cell, how many of its tail pages are resident, and where the first one
    // went. Pages of one cell are allocated contiguously when the free list
    // allows, which is what lets the range emitter merge them into one entry.
    TArray<int32> ResidentTailPages;
    TArray<int32> FirstTailPageSlotIndex;   // in PAGES, not slots; INDEX_NONE when none

    // What a view may actually draw from this cell: FloorCount + resident tail
    // splats. The selection clamps its take to this, and any shortfall is a miss.
    int32 GetResidentSplats(int32 CellIndex) const;

    int64 ResidentSplats = 0;
    bool bComplete = false;    // every page of every cell is resident

    // Per asset, not per pool, and never evicted (plan D3). Cell bounds are two
    // float4 per cell -- tens of KB -- and the SH palette is the asset's own, so
    // neither belongs in the slot space that streaming moves around.
    TRefCountPtr<FRDGPooledBuffer> CellBounds;
    TRefCountPtr<FRDGPooledBuffer> SHPalette;

    FRHIShaderResourceView* GetCellBoundsSRV() const { return CellBounds.IsValid() ? CellBounds->GetSRV() : nullptr; }
    FRHIShaderResourceView* GetSHPaletteSRV() const { return SHPalette.IsValid() ? SHPalette->GetSRV() : nullptr; }
};

// One contiguous stretch of pool slots. A cell's take becomes a handful of these:
// its floor block is one, and its tail pages are one more when they were handed
// out contiguously, which the allocator does whenever it can. So a freshly filled
// cell costs TWO range entries, not one per page -- which is what keeps the
// lookup's range table small enough to stay in L1 (plan D6).
struct FGaussianSplatPoolRun
{
    uint32 FirstSlot = 0;
    uint32 Count = 0;
};

// The pool runs covering a cell's first Take splats, in dispatch order, merged
// where they are adjacent. Returns the number of splats actually covered, which
// is less than Take when the cell is not fully resident -- that shortfall is the
// miss the stats line reports and the gate asserts is zero in sync mode.
uint32 BuildPoolRuns(
    const UGaussianSplatPagedAsset& Asset,
    const FGaussianSplatPoolResidency& Residency,
    int32 CellIndex,
    uint32 Take,
    TArray<FGaussianSplatPoolRun>& OutRuns);

class FGaussianSplatPagePool final : public FRenderResource
{
public:
    static FGaussianSplatPagePool& Get();

    // True when r.GaussianSplat.PagedAssets is on. Everything paged is behind it;
    // with it off the legacy path runs byte for byte as before.
    static bool ArePagedAssetsEnabled();

    // Sizes and allocates the pool if it does not exist yet. Safe to call from
    // the game thread; the allocation itself is done on the render thread.
    void EnsureAllocated();

    // Uploads what the asset holds, clamped to what is left. Returns the
    // residency, which the draw path reads. Registering twice is a no-op.
    const FGaussianSplatPoolResidency* RegisterAsset(const UGaussianSplatPagedAsset* Asset);
    void UnregisterAsset(const UGaussianSplatPagedAsset* Asset);
    const FGaussianSplatPoolResidency* FindResidency(const UGaussianSplatPagedAsset* Asset) const;

    // Released before the RHI shuts down, not after: a pooled buffer outliving
    // the RHI is a crash on exit (Step 0's validation run found the pattern).
    virtual void ReleaseRHI() override;
    virtual FString GetFriendlyName() const override { return TEXT("GaussianSplatPagePool"); }

    bool IsAllocated() const { return TotalSlots > 0; }
    bool IsInVideoMemory() const { return bInVideoMemory; }
    int64 GetTotalSlots() const { return TotalSlots; }
    int64 GetFreeSlots() const { return (FreePages.Num() * SlotsPerPage) + (TotalSlots - NextUnusedSlot); }

    // A structured pooled buffer creates its SRV in its own constructor, so these
    // need no command list and no cache of ours.
    FRHIShaderResourceView* GetPackedASRV() const { return PackedA.IsValid() ? PackedA->GetSRV() : nullptr; }
    FRHIShaderResourceView* GetPackedBSRV() const { return PackedB.IsValid() ? PackedB->GetSRV() : nullptr; }
    FRHIShaderResourceView* GetSHIndexSRV() const { return SHIndex.IsValid() ? SHIndex->GetSRV() : nullptr; }

    static constexpr int64 SlotsPerPage = 4096;
    static constexpr int64 BytesPerSlot = 24;      // 16 + 4 + 4; the SH buffer is allocated even for SH0 captures

private:
    // The derivation, logged term by term (plan D3). Returns bytes.
    int64 DeriveSizeBytes() const;

    // Hands out a run of pages, contiguous when the free list allows. Returns the
    // first page index, or INDEX_NONE.
    int32 AllocatePages(int32 PageCount);
    void FreePagesOf(FGaussianSplatPoolResidency& Residency);

    // A pinned range for a floor block. Floors are never freed until the asset
    // unregisters, so they are taken from the front of the untouched space.
    int64 AllocateFloorRange(int64 Slots);

    // The asset's own two small buffers, built once when it registers.
    void BuildAssetBuffers(const UGaussianSplatPagedAsset& Asset, FGaussianSplatPoolResidency& Residency);

    void UploadRange(int64 FirstSlot, const uint8* SourceA, const uint8* SourceB, const uint8* SourceSH, int64 Slots);

    TRefCountPtr<FRDGPooledBuffer> PackedA;
    TRefCountPtr<FRDGPooledBuffer> PackedB;
    TRefCountPtr<FRDGPooledBuffer> SHIndex;

    int64 TotalSlots = 0;
    int64 NextUnusedSlot = 0;      // everything past this has never been handed out
    TArray<int32> FreePages;       // page indices returned by an unregister
    bool bInVideoMemory = false;

    // Set when the derivation came out at zero, so the refusal is logged once rather than
    // twice a frame for as long as the map is open. Cleared when a size CVar changes.
    bool bSizingFailed = false;
    int32 FailedSettings = 0;

    TMap<const UGaussianSplatPagedAsset*, FGaussianSplatPoolResidency> Residencies;
};
