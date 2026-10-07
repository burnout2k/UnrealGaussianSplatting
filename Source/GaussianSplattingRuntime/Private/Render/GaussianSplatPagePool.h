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

    // Where each of the asset's tail pages lives, indexed by its GLOBAL tail page
    // number (Cell.FirstTailPage + k), in PAGES not slots; INDEX_NONE when the
    // page is not resident. Step 1a filled this contiguously in one go; streaming
    // adds and removes single entries, so the map has to be per page rather than
    // per cell (a cell's first slot plus a count no longer describes it).
    TArray<int32> TailPageSlot;

    // Per cell, how many of its tail pages are resident. Residency is always a
    // PREFIX of a cell's pages (D4 evicts a cell's last page first), so this one
    // number still answers "what may this cell draw", and the range emitter walks
    // the slots to merge the adjacent ones.
    TArray<int32> ResidentTailPages;

    // Streaming bookkeeping, per global tail page. Residency may depend on
    // history; the drawn set may not (D9).
    TArray<int32> LastNeededTick;    // for the LRU order
    TArray<int32> EvictedTick;       // for the protected age and the thrash counter

    // What a view may actually draw from this cell: FloorCount + resident tail
    // splats. The selection clamps its take to this, and any shortfall is a miss.
    int32 GetResidentSplats(int32 CellIndex) const;

    // The pool slot a cell's page k sits at, or INDEX_NONE.
    int32 GetTailPageSlot(const UGaussianSplatPagedAsset& Asset, int32 CellIndex, int32 PageInCell) const;

    int64 ResidentSplats = 0;

    // Maintained by ApplyStreamPlan. Counting it by walking TailPageSlot would be
    // 1.45M iterations per tick on Lublin, against a 0.5 ms gate budget.
    int32 ResidentPageCount = 0;

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

// One page the gate wants moved this tick. The page is named by its GLOBAL tail
// page number (Cell.FirstTailPage + k) so the pool needs no cell arithmetic.
struct FGaussianSplatStreamPage
{
    const UGaussianSplatPagedAsset* Asset = nullptr;
    int32 GlobalPage = 0;
    int32 CellIndex = 0;
};

// One tick's decision, built on the game thread and handed to the pool whole, so
// the render thread sees a consistent page table rather than a stream of edits.
struct FGaussianSplatStreamPlan
{
    // Evictions are applied BEFORE uploads, so a tick that swaps pages needs no
    // spare capacity -- which is the whole point at a capped pool.
    TArray<FGaussianSplatStreamPage> Evict;

    // Required pages first, then wanted. Everything below RequiredUploads is
    // uploaded in this tick whatever it costs (D4); the rest is prefetch and
    // stops at the byte cap.
    TArray<FGaussianSplatStreamPage> Upload;
    int32 RequiredUploads = 0;

    // The overflow multiplier the gate solved, for the stats line. 1.0 = the
    // required set fitted and the overflow path did not engage.
    float OverflowMultiplier = 1.0f;

    bool IsEmpty() const { return Evict.IsEmpty() && Upload.IsEmpty(); }
};

// What one tick of streaming cost and did, for the stats line (D8).
struct FGaussianSplatStreamStats
{
    int32 PagesUploaded = 0;
    int32 PagesEvicted = 0;
    int32 PagesRequiredNotUploaded = 0;   // the cap bit, or the pool is full
    int64 UploadBytes = 0;
    double UploadMs = 0.0;

    // Pages re-uploaded within the protected age of their own eviction. Thrash
    // shows up as upload time, never as a wrong picture, so nothing else would
    // report it (review M5).
    int32 ThrashCount = 0;

    // Carried through for the stats line; the pool does not act on it.
    float OverflowMultiplierUnused = 1.0f;
};

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

    // True when r.GaussianSplat.Stream is on. With it off an asset is uploaded
    // whole at registration, exactly as Step 1a does, and no gate runs.
    static bool IsStreamingEnabled();

    // Uploads what the asset holds, clamped to what is left. Returns the
    // residency, which the draw path reads. Registering twice is a no-op.
    // With streaming on this uploads the PINNED FLOOR ONLY and leaves every tail
    // page to the gate, so the asset may be far larger than the pool.
    const FGaussianSplatPoolResidency* RegisterAsset(const UGaussianSplatPagedAsset* Asset);

    // Applies one tick's decision: evictions first, then uploads. Called from the
    // game thread; the copies are enqueued on the render thread.
    void ApplyStreamPlan(FGaussianSplatStreamPlan&& Plan, int32 TickNumber);

    // How many pages are free right now, which is what the gate fits against.
    int32 GetFreePageCount() const;

    const FGaussianSplatStreamStats& GetLastStreamStats() const { return LastStreamStats; }

    // Every registered asset, for the gate's walk.
    void GetRegisteredAssets(TArray<const UGaussianSplatPagedAsset*>& Out) const;
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
    // first page index, or INDEX_NONE. The whole-asset path (streaming off).
    int32 AllocatePages(int32 PageCount);

    // One page at a time, from the free list first and the untouched frontier
    // after. The streaming path; returns INDEX_NONE when the pool is full.
    int32 AcquirePage();
    void ReleasePage(int32 PageIndex);
    void FreePagesOf(FGaussianSplatPoolResidency& Residency);

    // A pinned range for a floor block. Floors are never freed until the asset
    // unregisters, so they are taken from the front of the untouched space.
    int64 AllocateFloorRange(int64 Slots);

    // The asset's own two small buffers, built once when it registers.
    void BuildAssetBuffers(const UGaussianSplatPagedAsset& Asset, FGaussianSplatPoolResidency& Residency);

    void UploadRange(int64 FirstSlot, const uint8* SourceA, const uint8* SourceB, const uint8* SourceSH, int64 Slots);

    // D5's per-tick path: one BUF_Dynamic staging buffer filled by a ParallelFor
    // over pages, then ONE scatter dispatch that writes all three pool buffers.
    // Batched per asset, because HasSH is uniform across a dispatch, and split so
    // no single command carries more than MaxUploadMBPerCommand.
    void UploadPageBatch(
        const UGaussianSplatPagedAsset& Asset,
        const TArray<int32>& GlobalPages,
        const TArray<int32>& DestPageSlots);

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

    FGaussianSplatStreamStats LastStreamStats;

    // The gate's tick count, used for the LRU order and the protected age. Not a
    // frame number: the gate may run on ticks that draw nothing.
    int32 StreamTick = 0;
};
