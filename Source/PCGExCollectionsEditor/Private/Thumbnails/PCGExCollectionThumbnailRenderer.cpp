// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Thumbnails/PCGExCollectionThumbnailRenderer.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "ObjectTools.h"
#include "TextureResource.h"
#include "AssetRegistry/AssetData.h"
#include "Core/PCGExAssetCollection.h"
#include "Details/Collections/PCGExCollectionEditorUtils.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "UObject/Package.h"
#include "Misc/ObjectThumbnail.h"
#include "ThumbnailRendering/ThumbnailManager.h"

namespace PCGExCollectionThumbnail
{
	constexpr float BaseThumbnailSize = 256.f;

	// Backdrop grays; EmptyCellColor sits just above BackgroundColor so empty slots read against the gaps.
	const FLinearColor BackgroundColor = FLinearColor(0.016f, 0.016f, 0.018f);
	const FLinearColor EmptyCellColor = FLinearColor(0.024f, 0.024f, 0.027f);
	const FLinearColor DeepCollectionCellColor = FLinearColor(0.06f, 0.05f, 0.09f);
}

bool UPCGExCollectionThumbnailRenderer::CanVisualizeAsset(UObject* Object)
{
	// Empty collections keep the plain class icon instead of an empty grid.
	const UPCGExAssetCollection* Collection = Cast<UPCGExAssetCollection>(Object);
	return Collection && Collection->NumEntries() > 0;
}

void UPCGExCollectionThumbnailRenderer::GetThumbnailSize(UObject* Object, float Zoom, uint32& OutWidth, uint32& OutHeight) const
{
	OutWidth = FMath::TruncToInt(Zoom * PCGExCollectionThumbnail::BaseThumbnailSize);
	OutHeight = OutWidth;
}

void UPCGExCollectionThumbnailRenderer::Draw(UObject* Object, int32 X, int32 Y, uint32 Width, uint32 Height, FRenderTarget* Viewport, FCanvas* Canvas, bool bAdditionalViewFamily)
{
	const UPCGExAssetCollection* Collection = Cast<UPCGExAssetCollection>(Object);
	if (!Collection || Width == 0 || Height == 0)
	{
		return;
	}

	DrawCollection(Collection, X, Y, Width, Height, Viewport, Canvas, /*Depth=*/0);
}

void UPCGExCollectionThumbnailRenderer::DrawCollection(const UPCGExAssetCollection* Collection, float X, float Y, float Width, float Height, FRenderTarget* Viewport, FCanvas* Canvas, int32 Depth)
{
	using namespace PCGExCollectionThumbnail;

	DrawPlaceholder(X, Y, Width, Height, Canvas, BackgroundColor);

	const int32 NumEntries = Collection->NumEntries();
	if (NumEntries <= 0)
	{
		return;
	}

	// Adaptive grid: 1 / 2x2 / 3x3.
	const int32 Side = NumEntries <= 1 ? 1 : NumEntries <= 4 ? 2 : 3;
	const int32 MaxCells = Side * Side;
	const bool bOverflow = NumEntries > MaxCells;
	const int32 NumEntryCells = bOverflow ? MaxCells - 1 : NumEntries;

	const float Pad = Side == 1 ? 0.f : FMath::Max(2.f, Width / 64.f);
	const float CellWidth = (Width - Pad * (Side + 1)) / Side;
	const float CellHeight = (Height - Pad * (Side + 1)) / Side;

	if (CellWidth < 1.f || CellHeight < 1.f)
	{
		return;
	}

	for (int32 CellIndex = 0; CellIndex < MaxCells; CellIndex++)
	{
		const int32 Row = CellIndex / Side;
		const int32 Col = CellIndex % Side;
		const float CellX = X + Pad + Col * (CellWidth + Pad);
		const float CellY = Y + Pad + Row * (CellHeight + Pad);

		if (CellIndex < NumEntryCells)
		{
			// Authored order; empty entry slots stay as placeholders so cell order matches Entries.
			const FPCGExEntryAccessResult Result = Collection->GetEntryRaw(CellIndex);
			const FSoftObjectPath CellPath = Result.IsValid() ? Result.Entry->EDITOR_GetThumbnailAssetPath() : FSoftObjectPath();
			DrawCell(CellPath, CellX, CellY, CellWidth, CellHeight, Viewport, Canvas, Depth);
		}
		else if (bOverflow && CellIndex == MaxCells - 1)
		{
			DrawOverflowCell(NumEntries - NumEntryCells, CellX, CellY, CellWidth, CellHeight, Canvas);
		}
		else
		{
			DrawPlaceholder(CellX, CellY, CellWidth, CellHeight, Canvas, EmptyCellColor);
		}
	}
}

void UPCGExCollectionThumbnailRenderer::DrawCell(const FSoftObjectPath& AssetPath, float X, float Y, float Width, float Height, FRenderTarget* Viewport, FCanvas* Canvas, int32 Depth)
{
	using namespace PCGExCollectionThumbnail;

	if (AssetPath.IsNull())
	{
		DrawPlaceholder(X, Y, Width, Height, Canvas, EmptyCellColor);
		return;
	}

	UObject* LoadedObject = AssetPath.ResolveObject();

	// Nested subcollection: recurse (depth cap also bounds cycles) rather than delegate.
	if (const UPCGExAssetCollection* SubCollection = Cast<UPCGExAssetCollection>(LoadedObject))
	{
		if (Depth < MaxRecursionDepth && SubCollection->NumEntries() > 0)
		{
			DrawCollection(SubCollection, X, Y, Width, Height, Viewport, Canvas, Depth + 1);
		}
		else
		{
			DrawPlaceholder(X, Y, Width, Height, Canvas, DeepCollectionCellColor);
		}
		return;
	}

	// Cached package thumbnail: no asset load, matches the browser's unloaded-asset view.
	if (UTexture2D* CellTexture = GetOrBuildCachedThumbnailTexture(AssetPath))
	{
		FCanvasTileItem Tile(FVector2D(X, Y), CellTexture->GetResource(), FVector2D(Width, Height), FLinearColor::White);
		Tile.BlendMode = SE_BLEND_Opaque;
		Canvas->DrawItem(Tile);
		return;
	}

	// No cached thumbnail (e.g. transient objects): delegate to the loaded child's own renderer if it has one.
	if (LoadedObject)
	{
		if (FThumbnailRenderingInfo* RenderInfo = UThumbnailManager::Get().GetRenderingInfo(LoadedObject);
			RenderInfo && RenderInfo->Renderer && RenderInfo->Renderer->CanVisualizeAsset(LoadedObject))
		{
			DrawDelegatedCell(LoadedObject, RenderInfo->Renderer, X, Y, Width, Height, Canvas);
			return;
		}
	}

	DrawPlaceholder(X, Y, Width, Height, Canvas, EmptyCellColor);
}

void UPCGExCollectionThumbnailRenderer::DrawDelegatedCell(UObject* Object, UThumbnailRenderer* Renderer, float X, float Y, float Width, float Height, FCanvas* Canvas)
{
	const int32 CellWidth = FMath::Max(1, FMath::TruncToInt(Width));
	const int32 CellHeight = FMath::Max(1, FMath::TruncToInt(Height));

	UTextureRenderTarget2D* CellTarget = GetDelegateRenderTarget(CellWidth, CellHeight, DelegateDepth);
	FTextureRenderTargetResource* CellResource = CellTarget ? CellTarget->GameThread_GetRenderTargetResource() : nullptr;
	if (!CellResource)
	{
		DrawPlaceholder(X, Y, Width, Height, Canvas, PCGExCollectionThumbnail::EmptyCellColor);
		return;
	}

	// The child sees a whole target at the origin, exactly as when the thumbnail pool renders it directly.
	{
		TGuardValue<int32> DepthGuard(DelegateDepth, DelegateDepth + 1);

		FCanvas CellCanvas(CellResource, nullptr, FGameTime::GetTimeSinceAppStart(), Canvas->GetFeatureLevel());
		CellCanvas.Clear(FLinearColor::Black);
		Renderer->Draw(Object, 0, 0, CellWidth, CellHeight, CellResource, &CellCanvas, /*bAdditionalViewFamily=*/true);
		CellCanvas.Flush_GameThread();
	}

	FCanvasTileItem Tile(FVector2D(X, Y), CellResource, FVector2D(Width, Height), FLinearColor::White);
	Tile.BlendMode = SE_BLEND_Opaque;
	Canvas->DrawItem(Tile);

	// Submit now: the next delegated cell of this size reuses CellTarget, and render-thread order keeps
	// this tile's read ahead of that overwrite.
	Canvas->Flush_GameThread();
}

UTextureRenderTarget2D* UPCGExCollectionThumbnailRenderer::GetDelegateRenderTarget(int32 Width, int32 Height, int32 Depth)
{
	const FIntVector Key(Width, Height, Depth);
	if (const TObjectPtr<UTextureRenderTarget2D>* Existing = DelegateRenderTargets.Find(Key))
	{
		return Existing->Get();
	}

	if (DelegateRenderTargets.Num() >= MaxDelegateRenderTargets)
	{
		DelegateRenderTargets.Empty(MaxDelegateRenderTargets);
	}

	// Same setup as the editor's thumbnail scratch targets, so delegated cells match a direct render.
	UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
	Target->ClearColor = FLinearColor::Black;
	Target->TargetGamma = GEngine ? GEngine->DisplayGamma : 2.2f;
	Target->RenderTargetFormat = RTF_RGBA8;
	Target->InitAutoFormat(Width, Height);
	Target->UpdateResourceImmediate(/*bClearRenderTarget=*/true);

	DelegateRenderTargets.Add(Key, Target);
	return Target;
}

void UPCGExCollectionThumbnailRenderer::DrawPlaceholder(float X, float Y, float Width, float Height, FCanvas* Canvas, const FLinearColor& Color) const
{
	FCanvasTileItem Tile(FVector2D(X, Y), FVector2D(Width, Height), Color);
	Tile.BlendMode = SE_BLEND_Opaque;
	Canvas->DrawItem(Tile);
}

void UPCGExCollectionThumbnailRenderer::DrawOverflowCell(int32 OverflowCount, float X, float Y, float Width, float Height, FCanvas* Canvas) const
{
	using namespace PCGExCollectionThumbnail;

	DrawPlaceholder(X, Y, Width, Height, Canvas, EmptyCellColor);

	if (!GEngine)
	{
		return;
	}

	FCanvasTextItem TextItem(
		FVector2D(X + Width * 0.5f, Y + Height * 0.5f),
		FText::FromString(FString::Printf(TEXT("+%d"), OverflowCount)),
		GEngine->GetLargeFont(),
		FLinearColor(0.85f, 0.85f, 0.85f));

	TextItem.bCentreX = true;
	TextItem.bCentreY = true;
	TextItem.EnableShadow(FLinearColor::Black);
	TextItem.Scale = FVector2D(FMath::Max(1.f, Width / 42.f));
	Canvas->DrawItem(TextItem);
}

UTexture2D* UPCGExCollectionThumbnailRenderer::GetOrBuildCachedThumbnailTexture(const FSoftObjectPath& AssetPath)
{
	const FAssetData AssetData = PCGExCollectionEditorUtils::ResolveEntryAssetData(AssetPath);
	if (!AssetData.IsValid())
	{
		return nullptr;
	}

	const FSoftObjectPath ResolvedPath = AssetData.GetSoftObjectPath();

	// Cache-first: a present key is authoritative (invalidated via OnThumbnailDirtied), so hits skip all I/O.
	// A null value negative-caches "no usable thumbnail" so thumbnail-less assets aren't re-read each render.
	if (const TObjectPtr<UTexture2D>* Existing = CellTextureCache.Find(ResolvedPath))
	{
		return Existing->Get();
	}

	EnsureThumbnailDirtyListener();

	// Miss: load the package thumbnail (in-memory first, package-header read for unloaded). Runs once per asset until dirtied.
	const FName ObjectFullName = FName(*AssetData.GetFullName());
	FThumbnailMap ThumbnailMap;

	const FObjectThumbnail* ObjectThumbnail = nullptr;
	if (ThumbnailTools::ConditionallyLoadThumbnailsForObjects({ObjectFullName}, ThumbnailMap))
	{
		ObjectThumbnail = ThumbnailMap.Find(ObjectFullName);
	}

	const int32 ImageWidth = ObjectThumbnail ? ObjectThumbnail->GetImageWidth() : 0;
	const int32 ImageHeight = ObjectThumbnail ? ObjectThumbnail->GetImageHeight() : 0;

	// Negative-cache the unusable cases so the caller delegates without re-reading every render.
	if (!ObjectThumbnail || ObjectThumbnail->IsEmpty() || ImageWidth <= 0 || ImageHeight <= 0)
	{
		StoreCellTexture(ResolvedPath, nullptr);
		return nullptr;
	}

	const TArray<uint8>& ImageData = ObjectThumbnail->GetUncompressedImageData();
	if (ImageData.Num() != ImageWidth * ImageHeight * 4)
	{
		StoreCellTexture(ResolvedPath, nullptr);
		return nullptr;
	}

	UTexture2D* Texture = UTexture2D::CreateTransient(ImageWidth, ImageHeight, PF_B8G8R8A8);
	if (!Texture)
	{
		return nullptr;
	}

	void* MipData = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(MipData, ImageData.GetData(), ImageData.Num());
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();

	Texture->SRGB = true;
	Texture->NeverStream = true;
	Texture->UpdateResource();

	StoreCellTexture(ResolvedPath, Texture);

	return Texture;
}

void UPCGExCollectionThumbnailRenderer::StoreCellTexture(const FSoftObjectPath& ResolvedPath, UTexture2D* Texture)
{
	// Cells are cheap to rebuild, so bound growth by dropping the whole cache when full (no per-entry LRU).
	if (CellTextureCache.Num() >= MaxCacheEntries)
	{
		CellTextureCache.Empty(MaxCacheEntries);
	}
	CellTextureCache.Add(ResolvedPath, Texture);
}

void UPCGExCollectionThumbnailRenderer::EnsureThumbnailDirtyListener()
{
	if (bThumbnailDirtiedBound)
	{
		return;
	}

	// Weak binding: auto-removed when this renderer is destroyed, so no explicit teardown.
	UThumbnailManager::Get().GetOnThumbnailDirtied().AddUObject(this, &UPCGExCollectionThumbnailRenderer::OnThumbnailDirtied);
	bThumbnailDirtiedBound = true;
}

void UPCGExCollectionThumbnailRenderer::OnThumbnailDirtied(const FSoftObjectPath& AssetPath)
{
	// Evict the dirtied asset's cell; next render rebuilds it. (A collection's own path is never a cell key.)
	CellTextureCache.Remove(AssetPath);
}
