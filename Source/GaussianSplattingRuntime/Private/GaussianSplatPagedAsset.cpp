#include "GaussianSplatPagedAsset.h"

#include "GaussianSplatAsset.h"
#include "Serialization/CustomVersion.h"

DEFINE_LOG_CATEGORY_STATIC(LogGaussianSplatPaged, Log, All);

void UGaussianSplatPagedAsset::Serialize(FArchive& Ar)
{
    Super::Serialize(Ar);
    Ar.UsingCustomVersion(FGaussianSplatCustomVersion::GUID);

    // The big tables are serialized by hand rather than as UPROPERTY arrays:
    // the details panel would try to build a row per cell, and every undo would
    // copy the lot through the transaction buffer.
    Ar << Cells;
    Ar << SHPalette;

    // The payload never goes through the transaction buffer. Without this an
    // undo of any property edit reads and writes gigabytes.
    if (Ar.IsTransacting())
    {
        return;
    }

    // A cook inlines bulk data unless told not to (LinkerSave.cpp:665-668), and
    // an inlined district would be read synchronously as part of the package.
    if (Ar.IsSaving())
    {
        Payload.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);
    }

    // Never BULKDATA_MemoryMappedPayload: Linux has no memory-mapped cooked
    // files here, and the loader would look for a .m.ubulk that does not exist.
    Payload.Serialize(Ar, this, INDEX_NONE, /*bAttemptFileMapping*/ false);
}

void UGaussianSplatPagedAsset::PostLoad()
{
    Super::PostLoad();
    RefreshDerivedData();

    // D2: the payload is in RAM before the first client tick, so nothing in the
    // frame loop ever waits on IO. This can take seconds for a district.
    LoadPayloadToMemory();
}

void UGaussianSplatPagedAsset::BeginDestroy()
{
    ReleasePayloadFromMemory();
    Super::BeginDestroy();
}

void UGaussianSplatPagedAsset::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
    Super::GetResourceSizeEx(CumulativeResourceSize);
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(PayloadBytes.Num());
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(Cells.Num() * sizeof(FGaussianSplatPagedCell));
    CumulativeResourceSize.AddDedicatedSystemMemoryBytes(SHPalette.Num() * sizeof(float));
}

void UGaussianSplatPagedAsset::RefreshDerivedData()
{
    // Floor blocks are laid out in cell order, so a cell's start is the running
    // sum of the floor counts before it.
    FloorFirstSlots.SetNumUninitialized(Cells.Num());
    int64 Running = 0;
    for (int32 CellIndex = 0; CellIndex < Cells.Num(); ++CellIndex)
    {
        FloorFirstSlots[CellIndex] = Running;
        Running += Cells[CellIndex].FloorCount;
    }

    if (Running != FloorSplats)
    {
        UE_LOG(
            LogGaussianSplatPaged,
            Warning,
            TEXT("%s: the cell table's floor counts sum to %lld but the header says %lld. Re-import the asset."),
            *GetName(),
            Running,
            FloorSplats);
    }

    // The selection's view of the cell table. Count is the asset's FULL count, not
    // what is resident: a view's take must not change because a page was evicted
    // (plan D6), and the clamp to residency happens at emission.
    SelectionCells.SetNumUninitialized(Cells.Num());
    for (int32 CellIndex = 0; CellIndex < Cells.Num(); ++CellIndex)
    {
        FGaussianSplatCell& Out = SelectionCells[CellIndex];
        Out.BoundsMin = Cells[CellIndex].BoundsMin;
        Out.BoundsMax = Cells[CellIndex].BoundsMax;
        Out.FirstIndex = 0;
        Out.Count = Cells[CellIndex].Count;
    }

    FBox Box(ForceInit);
    for (const FGaussianSplatPagedCell& Cell : Cells)
    {
        Box += FBox(FVector(Cell.BoundsMin), FVector(Cell.BoundsMax));
    }
    Bounds = Cells.IsEmpty() ? FBoxSphereBounds(ForceInit) : FBoxSphereBounds(Box);
}

int32 UGaussianSplatPagedAsset::GetPageSplatCount(int32 CellIndex, int32 PageInCell) const
{
    if (!Cells.IsValidIndex(CellIndex))
    {
        return 0;
    }

    const FGaussianSplatPagedCell& Cell = Cells[CellIndex];
    if (PageInCell < 0 || PageInCell >= Cell.TailPageCount)
    {
        return 0;
    }

    const int64 TailCount = static_cast<int64>(Cell.Count) - Cell.FloorCount;
    const int64 Remaining = TailCount - static_cast<int64>(PageInCell) * PageSplats;
    return static_cast<int32>(FMath::Clamp<int64>(Remaining, 0, PageSplats));
}

bool UGaussianSplatPagedAsset::LoadPayloadToMemory()
{
    if (IsPayloadResident())
    {
        return true;
    }

    const int64 SizeOnDisk = Payload.GetBulkDataSize();
    if (SizeOnDisk <= 0)
    {
        return Cells.IsEmpty();   // an empty asset is not an error
    }

    const int64 Expected = GetPayloadBytes();
    if (SizeOnDisk != Expected)
    {
        UE_LOG(
            LogGaussianSplatPaged,
            Error,
            TEXT("%s: the payload is %lld bytes but the tables describe %lld. Nothing was loaded; re-import."),
            *GetName(),
            SizeOnDisk,
            Expected);
        return false;
    }

    const double StartTime = FPlatformTime::Seconds();

    // One copy, not two. Lock()/GetCopy() would read into an IoDispatcher buffer
    // and then memcpy into ours, which is a 2x RAM peak -- 11 GB on a district.
    PayloadBytes.SetNumUninitialized(SizeOnDisk);
    TUniquePtr<IBulkDataIORequest> Request(
        Payload.CreateStreamingRequest(AIOP_High, nullptr, PayloadBytes.GetData()));

    if (!Request.IsValid() || !Request->WaitCompletion())
    {
        UE_LOG(LogGaussianSplatPaged, Error, TEXT("%s: the payload read failed. Nothing was loaded."), *GetName());
        PayloadBytes.Empty();
        return false;
    }

    UE_LOG(
        LogGaussianSplatPaged,
        Display,
        TEXT("%s: payload %.1f MiB in RAM in %.2f s (%lld splats in %d cells: %lld floor, %lld tail pages)"),
        *GetName(),
        SizeOnDisk / (1024.0 * 1024.0),
        FPlatformTime::Seconds() - StartTime,
        TotalSplats,
        Cells.Num(),
        FloorSplats,
        TailPages);
    return true;
}

void UGaussianSplatPagedAsset::ReleasePayloadFromMemory()
{
    PayloadBytes.Empty();
}

void UGaussianSplatPagedAsset::SetPayloadFromMemory(TArray64<uint8>&& Bytes)
{
    const int64 Size = Bytes.Num();

    Payload.RemoveBulkData();
    // A cook inlines bulk data unless told not to, and the flag has to be set
    // before the data goes in.
    Payload.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);
    if (Size > 0)
    {
        Payload.Lock(LOCK_READ_WRITE);
        void* Dest = Payload.Realloc(Size);
        FMemory::Memcpy(Dest, Bytes.GetData(), Size);
        Payload.Unlock();
    }

    // Keep the baked bytes as the resident copy rather than reading them back:
    // the asset has not been saved, so there is nothing on disk to read.
    PayloadBytes = MoveTemp(Bytes);
}
