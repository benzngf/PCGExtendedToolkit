// Copyright 2026 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Helpers/PCGExStreamingHelpers.h"

#include "CoreMinimal.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Core/PCGExContext.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "UObject/SoftObjectPath.h"

namespace PCGExHelpers
{
	TSharedPtr<FStreamableHandle> LoadBlocking_AnyThread(const FSoftObjectPath& Path, FPCGExContext* InContext)
	{
		// Thread-safe synchronous asset loading. UAssetManager requires game-thread access,
		// so when called from a worker thread, dispatch to game thread and block until complete.
		// The context tracks the handle to prevent premature GC of loaded assets.
		//
		// DEADLOCK WARNING: "AnyThread" means callable from any thread, NOT completable without the
		// game thread -- the off-thread branch below marshals to the game thread and blocks
		// (ExecuteOnMainThreadAndWait). The streamable manager structurally requires the game thread
		// to complete a load (FlushAsyncLoading is GT/ALT-only; FStreamableHandle completion and
		// release are game-thread-only), so this cannot be made truly worker-completable.
		// Therefore: DO NOT call this from a path that the task can be cancelled mid-flight on a worker
		// (e.g. a PCGEx element's Boot()/Execute()). PCG's FPCGGraphExecutor::Cancel() blocks the game
		// thread waiting on that worker task BEFORE it calls Abort(), so the game thread never services
		// the marshaled load -> the worker waits on the GT, the GT waits on the worker -> hard deadlock.
		// For those paths use the async dependency flow instead: FPCGExContext::AddAssetDependency()
		// during prepare (which pauses the context, so Cancel() takes PCG's no-wait paused-task path),
		// or PCGExHelpers::Load() / FPickUnpacker::UnpackPinDeferred() + ResolveDeferred().
		TSharedPtr<FStreamableHandle> Handle;
		if (IsInGameThread())
		{
			Handle = UAssetManager::GetStreamableManager().RequestSyncLoad(Path);
			if (InContext)
			{
				InContext->TrackAssetsHandle(Handle);
			}
		}
		else
		{
			PCGExMT::ExecuteOnMainThreadAndWait([&]()
			{
				Handle = LoadBlocking_AnyThread(Path, InContext);
			});
		}

		return Handle;
	}

	TSharedPtr<FStreamableHandle> LoadBlocking_AnyThread(const TSharedPtr<TSet<FSoftObjectPath>>& Paths, FPCGExContext* InContext)
	{
		// DEADLOCK WARNING: see the single-path overload above. Off the game thread this marshals to
		// the GT and blocks, so it must not be used on a worker path that can be cancelled mid-flight
		// (PCGEx element Boot()/Execute()). Prefer the async dependency flow there.
		TSharedPtr<FStreamableHandle> Handle;
		if (IsInGameThread())
		{
			Handle = UAssetManager::GetStreamableManager().RequestSyncLoad(Paths->Array());
			if (InContext)
			{
				InContext->TrackAssetsHandle(Handle);
			}
		}
		else
		{
			PCGExMT::ExecuteOnMainThreadAndWait([&]()
			{
				Handle = LoadBlocking_AnyThread(Paths, InContext);
			});
		}
		return Handle;
	}

	void Load(const TSharedPtr<PCGExMT::FTaskManager>& TaskManager, FGetPaths&& GetPathsFunc, FOnLoadEnd&& OnLoadEnd)
	{
		check(TaskManager);

		// Async asset loading integrated with the PCGEx task system.
		// Dispatches to game thread (required by UAssetManager), creates a LoadToken
		// to keep the task manager alive during the async load, and fires OnLoadEnd
		// when the streamable manager completes. The token is released in both the
		// success callback and the early-completion/failure path to ensure the task
		// group's completion count stays correct.
		PCGExMT::ExecuteOnMainThread(TaskManager, [GetPathsFunc, OnLoadEnd, TaskManager]()
		{
			TArray<FSoftObjectPath> Paths = GetPathsFunc();

			if (Paths.IsEmpty())
			{
				OnLoadEnd(false, nullptr);
				return;
			}

			TWeakPtr<PCGExMT::FAsyncToken> LoadToken = TaskManager->TryCreateToken(FName("LoadToken"));
			const TSharedPtr<FStreamableHandle> LoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
				MoveTemp(Paths),
				[OnLoadEnd, LoadToken](TSharedPtr<FStreamableHandle> InHandle) // NOLINT(performance-unnecessary-value-param)
				{
					OnLoadEnd(true, InHandle);
					PCGEX_ASYNC_RELEASE_CAPTURED_TOKEN(LoadToken)
				});

			// Handle already-completed or failed loads (assets were cached or paths invalid).
			if (!LoadHandle || !LoadHandle->IsActive())
			{
				if (!LoadHandle || !LoadHandle->HasLoadCompleted())
				{
					OnLoadEnd(false, LoadHandle);
				}
				else
				{
					OnLoadEnd(true, LoadHandle);
				}
				PCGEX_ASYNC_RELEASE_TOKEN(LoadToken)
			}
		});
	}

	void SafeReleaseHandle(TSharedPtr<FStreamableHandle>& InHandle)
	{
		if (!InHandle.IsValid())
		{
			return;
		}

		if (IsInGameThread())
		{
			InHandle->ReleaseHandle();
			InHandle.Reset();
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread, [Handle = MoveTemp(InHandle)]()
			{
				if (Handle.IsValid())
				{
					Handle->ReleaseHandle();
				}
			});
		}
	}

	void SafeReleaseHandles(TArray<TSharedPtr<FStreamableHandle>>& InHandles)
	{
		if (InHandles.IsEmpty())
		{
			return;
		}

		if (IsInGameThread())
		{
			for (TSharedPtr<FStreamableHandle>& Handle : InHandles)
			{
				if (Handle.IsValid())
				{
					Handle->ReleaseHandle();
				}
			}
			InHandles.Empty();
		}
		else
		{
			AsyncTask(ENamedThreads::GameThread, [Handles = MoveTemp(InHandles)]()
			{
				for (const TSharedPtr<FStreamableHandle>& Handle : Handles)
				{
					if (Handle.IsValid())
					{
						Handle->ReleaseHandle();
					}
				}
			});
		}
	}
	
}
