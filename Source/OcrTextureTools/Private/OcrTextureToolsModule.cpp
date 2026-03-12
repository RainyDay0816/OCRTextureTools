#include "OcrTextureToolsModule.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/Texture2D.h"
#include "Factories/Factory.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformTime.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "OcrTextureToolsSettings.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Subsystems/ImportSubsystem.h"
#include "Widgets/Notifications/SNotificationList.h"

DEFINE_LOG_CATEGORY_STATIC(LogOcrTextureTools, Log, All);

namespace
{
	FString BuildIncompleteKey(const FManagedTextureInfo& Info)
	{
		return FString::Printf(TEXT("%s|%s"), *Info.TargetFolderPath, *Info.MaterialGroupName);
	}
}

bool FManagedTextureGroup::IsComplete() const
{
	return BaseColor != nullptr && Normal != nullptr && Orm != nullptr;
}

FString FManagedTextureGroup::DescribeMissing() const
{
	TArray<FString> MissingKinds;
	if (!BaseColor)
	{
		MissingKinds.Add(TEXT("BaseColor"));
	}
	if (!Normal)
	{
		MissingKinds.Add(TEXT("Normal"));
	}
	if (!Orm)
	{
		MissingKinds.Add(TEXT("ORM"));
	}

	return FString::Join(MissingKinds, TEXT(", "));
}

int32 FManagedTextureGroup::GetAvailableCount() const
{
	return (BaseColor ? 1 : 0) + (Normal ? 1 : 0) + (Orm ? 1 : 0);
}

void FOcrTextureToolsModule::StartupModule()
{
	RegisterDelegates();
}

void FOcrTextureToolsModule::ShutdownModule()
{
	UnregisterDelegates();
	ReportedIncompleteGroups.Reset();
	PendingTextureOperations.Reset();

	if (PendingFolderTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PendingFolderTickerHandle);
		PendingFolderTickerHandle.Reset();
	}
}

void FOcrTextureToolsModule::RegisterDelegates()
{
	if (!GEditor)
	{
		return;
	}

	UImportSubsystem* ImportSubsystem = GEditor->GetEditorSubsystem<UImportSubsystem>();
	if (!ImportSubsystem)
	{
		UE_LOG(LogOcrTextureTools, Warning, TEXT("Import subsystem not available; texture automation disabled."));
		return;
	}

	if (!PostImportHandle.IsValid())
	{
		PostImportHandle = ImportSubsystem->OnAssetPostImport.AddRaw(this, &FOcrTextureToolsModule::HandleAssetPostImport);
	}

	if (!ReimportHandle.IsValid())
	{
		ReimportHandle = ImportSubsystem->OnAssetReimport.AddRaw(this, &FOcrTextureToolsModule::HandleAssetReimport);
	}

	CachedImportSubsystem = ImportSubsystem;
}

void FOcrTextureToolsModule::UnregisterDelegates()
{
	UImportSubsystem* ImportSubsystem = CachedImportSubsystem.Get();
	if (!ImportSubsystem && GEditor)
	{
		ImportSubsystem = GEditor->GetEditorSubsystem<UImportSubsystem>();
	}

	if (ImportSubsystem)
	{
		if (PostImportHandle.IsValid())
		{
			ImportSubsystem->OnAssetPostImport.Remove(PostImportHandle);
		}

		if (ReimportHandle.IsValid())
		{
			ImportSubsystem->OnAssetReimport.Remove(ReimportHandle);
		}
	}

	PostImportHandle.Reset();
	ReimportHandle.Reset();
	CachedImportSubsystem.Reset();
}

void FOcrTextureToolsModule::HandleAssetPostImport(UFactory* InFactory, UObject* InCreatedObject)
{
	ProcessImportedObject(InCreatedObject, TEXT("import"));
}

void FOcrTextureToolsModule::HandleAssetReimport(UObject* InCreatedObject)
{
	ProcessImportedObject(InCreatedObject, TEXT("reimport"));
}

void FOcrTextureToolsModule::ProcessImportedObject(UObject* InObject, const TCHAR* InReason)
{
	UTexture2D* Texture = Cast<UTexture2D>(InObject);
	if (!Texture)
	{
		return;
	}

	FManagedTextureInfo TextureInfo;
	if (!TryBuildManagedTextureInfo(Texture, TextureInfo))
	{
		return;
	}

	QueuePendingTextureOperation(Texture, TextureInfo);
}

bool FOcrTextureToolsModule::TryBuildManagedTextureInfo(UTexture2D* Texture, FManagedTextureInfo& OutInfo) const
{
	if (!Texture)
	{
		return false;
	}

	const UOcrTextureToolsSettings* Settings = GetDefault<UOcrTextureToolsSettings>();
	if (!Settings || !Settings->bEnableAutomaticProcessing)
	{
		return false;
	}

	EManagedTextureKind Kind = EManagedTextureKind::Unknown;
	FString MaterialGroupName;
	if (!TryParseManagedTextureName(Texture->GetName(), Kind, MaterialGroupName))
	{
		return false;
	}

	if (!Texture->AssetImportData)
	{
		UE_LOG(LogOcrTextureTools, Warning, TEXT("Texture %s has no asset import data; skipping auto-organization."), *Texture->GetPathName());
		return false;
	}

	FString SourceFilename = Texture->AssetImportData->GetFirstFilename();
	if (SourceFilename.IsEmpty())
	{
		UE_LOG(LogOcrTextureTools, Warning, TEXT("Texture %s has no source filename; skipping auto-organization."), *Texture->GetPathName());
		return false;
	}

	FPaths::NormalizeFilename(SourceFilename);
	const FString SourceDirectory = FPaths::GetPath(SourceFilename);

	if (!Settings->WatchedSourceRoot.Path.IsEmpty())
	{
		FString WatchedRoot = Settings->WatchedSourceRoot.Path;
		FPaths::NormalizeDirectoryName(WatchedRoot);

		if (!FPaths::IsUnderDirectory(SourceDirectory, WatchedRoot) && !SourceDirectory.Equals(WatchedRoot, ESearchCase::IgnoreCase))
		{
			return false;
		}
	}

	const FString SourceFolderLeaf = FPaths::GetCleanFilename(SourceDirectory);
	if (SourceFolderLeaf.IsEmpty())
	{
		UE_LOG(LogOcrTextureTools, Warning, TEXT("Texture %s source folder could not be resolved from %s."), *Texture->GetPathName(), *SourceFilename);
		return false;
	}

	FString TargetRoot = Settings->TargetRootContentPath;
	TargetRoot.RemoveFromEnd(TEXT("/"));
	if (!TargetRoot.StartsWith(TEXT("/Game")))
	{
		UE_LOG(LogOcrTextureTools, Error, TEXT("TargetRootContentPath must start with /Game. Current value: %s"), *TargetRoot);
		return false;
	}

	const FString FolderName = ObjectTools::SanitizeObjectName(SourceFolderLeaf);
	if (FolderName.IsEmpty())
	{
		UE_LOG(LogOcrTextureTools, Warning, TEXT("Source folder %s resolved to an empty Unreal folder name."), *SourceFolderLeaf);
		return false;
	}

	OutInfo.Kind = Kind;
	OutInfo.SourceFilename = SourceFilename;
	OutInfo.SourceFolderName = FolderName;
	OutInfo.MaterialGroupName = MaterialGroupName;
	OutInfo.TargetFolderPath = FString::Printf(TEXT("%s/%s"), *TargetRoot, *FolderName);
	OutInfo.TargetTextureAssetPath = FString::Printf(TEXT("%s/%s"), *OutInfo.TargetFolderPath, *Texture->GetName());
	return true;
}

bool FOcrTextureToolsModule::TryParseManagedTextureName(const FString& TextureName, EManagedTextureKind& OutKind, FString& OutGroupName) const
{
	OutKind = EManagedTextureKind::Unknown;
	OutGroupName.Reset();

	const UOcrTextureToolsSettings* Settings = GetDefault<UOcrTextureToolsSettings>();
	if (!Settings)
	{
		return false;
	}

	const ESearchCase::Type SearchCase = Settings->bCaseSensitiveSuffixMatch
		? ESearchCase::CaseSensitive
		: ESearchCase::IgnoreCase;

	const auto MatchSuffix = [&](const FString& Suffix, EManagedTextureKind Kind) -> bool
	{
		if (Suffix.IsEmpty() || !TextureName.EndsWith(Suffix, SearchCase))
		{
			return false;
		}

		OutKind = Kind;
		OutGroupName = TextureName.LeftChop(Suffix.Len());
		return !OutGroupName.IsEmpty();
	};

	return MatchSuffix(Settings->BaseColorSuffix, EManagedTextureKind::BaseColor)
		|| MatchSuffix(Settings->NormalSuffix, EManagedTextureKind::Normal)
		|| MatchSuffix(Settings->OrmSuffix, EManagedTextureKind::Orm);
}

bool FOcrTextureToolsModule::ApplyTextureRules(UTexture2D* Texture, EManagedTextureKind Kind) const
{
	if (!Texture || Kind != EManagedTextureKind::Orm)
	{
		return false;
	}

	const bool bNeedsSrgbChange = Texture->SRGB;
	const bool bNeedsCompressionChange = Texture->CompressionSettings != TC_Masks;
	if (!bNeedsSrgbChange && !bNeedsCompressionChange)
	{
		return false;
	}

	Texture->Modify();
	Texture->PreEditChange(nullptr);

	if (bNeedsSrgbChange)
	{
		Texture->SRGB = false;
	}

	if (bNeedsCompressionChange)
	{
		Texture->CompressionSettings = TC_Masks;
	}

	Texture->PostEditChange();
	Texture->UpdateResource();
	Texture->MarkPackageDirty();

	UE_LOG(LogOcrTextureTools, Log, TEXT("Normalized ORM texture settings for %s."), *Texture->GetPathName());
	return true;
}

bool FOcrTextureToolsModule::EnsureTargetFolder(const FString& FolderPath) const
{
	UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
	if (!AssetSubsystem)
	{
		return false;
	}

	return AssetSubsystem->DoesDirectoryExist(FolderPath) || AssetSubsystem->MakeDirectory(FolderPath);
}

bool FOcrTextureToolsModule::MoveTextureToManagedFolder(UTexture2D* Texture, const FManagedTextureInfo& Info) const
{
	if (!Texture)
	{
		return false;
	}

	UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
	if (!AssetSubsystem)
	{
		UE_LOG(LogOcrTextureTools, Error, TEXT("Editor asset subsystem unavailable; cannot move %s."), *Texture->GetPathName());
		return false;
	}

	const FString CurrentAssetPath = Texture->GetOutermost()->GetName();
	if (CurrentAssetPath.Equals(Info.TargetTextureAssetPath, ESearchCase::CaseSensitive))
	{
		return true;
	}

	if (AssetSubsystem->DoesAssetExist(Info.TargetTextureAssetPath))
	{
		UE_LOG(
			LogOcrTextureTools,
			Warning,
			TEXT("Target asset %s already exists. Leaving imported texture %s in place."),
			*Info.TargetTextureAssetPath,
			*CurrentAssetPath);
		NotifyUser(FString::Printf(TEXT("Target texture already exists: %s"), *Info.TargetTextureAssetPath), false);
		return false;
	}

	if (!AssetSubsystem->RenameLoadedAsset(Texture, Info.TargetTextureAssetPath))
	{
		UE_LOG(
			LogOcrTextureTools,
			Error,
			TEXT("Failed to move texture %s to %s."),
			*CurrentAssetPath,
			*Info.TargetTextureAssetPath);
		NotifyUser(FString::Printf(TEXT("Failed to move texture: %s"), *Texture->GetName()), false);
		return false;
	}

	UE_LOG(
		LogOcrTextureTools,
		Log,
		TEXT("Moved texture %s to %s based on source folder %s."),
		*CurrentAssetPath,
		*Info.TargetTextureAssetPath,
		*Info.SourceFolderName);
	return true;
}

bool FOcrTextureToolsModule::GatherManagedTextureGroups(const FString& TargetFolderPath, TMap<FString, FManagedTextureGroup>& OutGroups) const
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");

	TArray<FAssetData> AssetDataArray;
	if (!AssetRegistryModule.Get().GetAssetsByPath(FName(*TargetFolderPath), AssetDataArray, false, false))
	{
		return false;
	}

	for (const FAssetData& AssetData : AssetDataArray)
	{
		if (AssetData.AssetClassPath != UTexture2D::StaticClass()->GetClassPathName())
		{
			continue;
		}

		EManagedTextureKind Kind = EManagedTextureKind::Unknown;
		FString GroupName;
		if (!TryParseManagedTextureName(AssetData.AssetName.ToString(), Kind, GroupName))
		{
			continue;
		}

		UTexture2D* Texture = Cast<UTexture2D>(AssetData.GetAsset());
		if (!Texture)
		{
			continue;
		}

		FManagedTextureGroup& Group = OutGroups.FindOrAdd(GroupName);
		switch (Kind)
		{
		case EManagedTextureKind::BaseColor:
			Group.BaseColor = Texture;
			break;
		case EManagedTextureKind::Normal:
			Group.Normal = Texture;
			break;
		case EManagedTextureKind::Orm:
			Group.Orm = Texture;
			break;
		default:
			break;
		}
	}

	return true;
}

UMaterialInterface* FOcrTextureToolsModule::LoadParentMaterial() const
{
	const UOcrTextureToolsSettings* Settings = GetDefault<UOcrTextureToolsSettings>();
	if (!Settings)
	{
		return nullptr;
	}

	return Cast<UMaterialInterface>(Settings->ParentMaterialPath.TryLoad());
}

UMaterialInstanceConstant* FOcrTextureToolsModule::FindOrCreateMaterialInstance(const FManagedTextureInfo& Info, UMaterialInterface* ParentMaterial, bool& bOutCreated) const
{
	bOutCreated = false;

	UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
	if (!AssetSubsystem)
	{
		return nullptr;
	}

	const FString InstanceName = ObjectTools::SanitizeObjectName(FString::Printf(TEXT("MI_%s"), *Info.MaterialGroupName));
	const FString InstanceAssetPath = FString::Printf(TEXT("%s/%s"), *Info.TargetFolderPath, *InstanceName);

	if (AssetSubsystem->DoesAssetExist(InstanceAssetPath))
	{
		return Cast<UMaterialInstanceConstant>(AssetSubsystem->LoadAsset(InstanceAssetPath));
	}

	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = ParentMaterial;

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	UObject* CreatedAsset = AssetToolsModule.Get().CreateAsset(
		InstanceName,
		Info.TargetFolderPath,
		UMaterialInstanceConstant::StaticClass(),
		Factory);

	bOutCreated = CreatedAsset != nullptr;
	return Cast<UMaterialInstanceConstant>(CreatedAsset);
}

bool FOcrTextureToolsModule::UpdateMaterialInstance(const FManagedTextureInfo& Info, const FManagedTextureGroup& Group) const
{
	UMaterialInterface* ParentMaterial = LoadParentMaterial();
	if (!ParentMaterial)
	{
		UE_LOG(LogOcrTextureTools, Error, TEXT("Parent material could not be loaded from plugin settings. Material group %s was not created."), *Info.MaterialGroupName);
		NotifyUser(TEXT("Parent material could not be loaded from plugin settings."), false);
		return false;
	}

	bool bCreated = false;
	UMaterialInstanceConstant* MaterialInstance = FindOrCreateMaterialInstance(Info, ParentMaterial, bCreated);
	if (!MaterialInstance)
	{
		UE_LOG(LogOcrTextureTools, Error, TEXT("Failed to create or load MI for material group %s."), *Info.MaterialGroupName);
		NotifyUser(FString::Printf(TEXT("Failed to create material instance: MI_%s"), *Info.MaterialGroupName), false);
		return false;
	}

	const UOcrTextureToolsSettings* Settings = GetDefault<UOcrTextureToolsSettings>();
	check(Settings);

	UMaterialEditingLibrary::SetMaterialInstanceParent(MaterialInstance, ParentMaterial);

	TArray<FName> TextureParameterNames;
	UMaterialEditingLibrary::GetTextureParameterNames(ParentMaterial, TextureParameterNames);

	const auto HasTextureParameter = [&TextureParameterNames](const FName ParameterName) -> bool
	{
		return ParameterName != NAME_None && TextureParameterNames.Contains(ParameterName);
	};

	bool bAllParametersApplied = true;

	if (HasTextureParameter(Settings->BaseColorParameterName))
	{
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, Settings->BaseColorParameterName, Group.BaseColor);
	}
	else if (Settings->BaseColorParameterName != NAME_None)
	{
		UE_LOG(LogOcrTextureTools, Warning, TEXT("Texture parameter %s was not found on parent material %s."), *Settings->BaseColorParameterName.ToString(), *ParentMaterial->GetPathName());
		bAllParametersApplied = false;
	}

	if (HasTextureParameter(Settings->NormalParameterName))
	{
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, Settings->NormalParameterName, Group.Normal);
	}
	else if (Settings->NormalParameterName != NAME_None)
	{
		UE_LOG(LogOcrTextureTools, Warning, TEXT("Texture parameter %s was not found on parent material %s."), *Settings->NormalParameterName.ToString(), *ParentMaterial->GetPathName());
		bAllParametersApplied = false;
	}

	if (HasTextureParameter(Settings->OrmParameterName))
	{
		UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MaterialInstance, Settings->OrmParameterName, Group.Orm);
	}
	else if (Settings->OrmParameterName != NAME_None)
	{
		UE_LOG(LogOcrTextureTools, Warning, TEXT("Texture parameter %s was not found on parent material %s."), *Settings->OrmParameterName.ToString(), *ParentMaterial->GetPathName());
		bAllParametersApplied = false;
	}

	UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
	MaterialInstance->MarkPackageDirty();

	const TCHAR* ActionWord = bCreated ? TEXT("Created") : TEXT("Updated");
	UE_LOG(
		LogOcrTextureTools,
		Log,
		TEXT("%s material instance %s in %s for material group %s."),
		ActionWord,
		*MaterialInstance->GetPathName(),
		*Info.SourceFolderName,
		*Info.MaterialGroupName);

	if (bAllParametersApplied)
	{
		NotifyUser(FString::Printf(TEXT("%s MI_%s"), ActionWord, *Info.MaterialGroupName), true);
	}
	else
	{
		NotifyUser(FString::Printf(TEXT("%s MI_%s with parameter mismatches"), ActionWord, *Info.MaterialGroupName), false);
	}

	return bAllParametersApplied;
}

void FOcrTextureToolsModule::NotifyUser(const FString& Message, bool bIsSuccess) const
{
	const UOcrTextureToolsSettings* Settings = GetDefault<UOcrTextureToolsSettings>();
	if (!Settings || !Settings->bShowEditorNotifications)
	{
		return;
	}

	FNotificationInfo NotificationInfo(FText::FromString(Message));
	NotificationInfo.bUseLargeFont = false;
	NotificationInfo.bFireAndForget = true;
	NotificationInfo.ExpireDuration = bIsSuccess ? 4.0f : 6.0f;

	if (TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(NotificationInfo))
	{
		Notification->SetCompletionState(bIsSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}
}

void FOcrTextureToolsModule::ReportIncompleteGroup(const FManagedTextureInfo& Info, const FManagedTextureGroup& Group, const TCHAR* InReason)
{
	const FString MissingDescription = Group.DescribeMissing();
	UE_LOG(
		LogOcrTextureTools,
		Log,
		TEXT("Material group %s in %s is incomplete after %s. Found %d/3 textures; missing %s."),
		*Info.MaterialGroupName,
		*Info.TargetFolderPath,
		InReason,
		Group.GetAvailableCount(),
		*MissingDescription);

	const FString Key = BuildIncompleteKey(Info);
	if (ReportedIncompleteGroups.Contains(Key))
	{
		return;
	}

	ReportedIncompleteGroups.Add(Key);
	NotifyUser(FString::Printf(TEXT("Incomplete material group: %s (missing %s)"), *Info.MaterialGroupName, *MissingDescription), false);
}

void FOcrTextureToolsModule::ClearIncompleteReport(const FManagedTextureInfo& Info)
{
	ReportedIncompleteGroups.Remove(BuildIncompleteKey(Info));
}

UEditorAssetSubsystem* FOcrTextureToolsModule::GetEditorAssetSubsystem() const
{
	return GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
}

void FOcrTextureToolsModule::QueuePendingTextureOperation(UTexture2D* Texture, const FManagedTextureInfo& Info)
{
	FPendingTextureOperation Operation;
	Operation.Texture = Texture;
	Operation.Info = Info;

	PendingTextureOperations.Add(Info.TargetTextureAssetPath, Operation);
	PendingTextureOperationDeadline = FPlatformTime::Seconds() + 1.5;

	if (!PendingFolderTickerHandle.IsValid())
	{
		PendingFolderTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FOcrTextureToolsModule::FlushPendingTextureOperations),
			0.25f);
	}
}

bool FOcrTextureToolsModule::FlushPendingTextureOperations(float DeltaTime)
{
	if (FPlatformTime::Seconds() < PendingTextureOperationDeadline)
	{
		return true;
	}

	TArray<FPendingTextureOperation> PendingOperations;
	PendingTextureOperations.GenerateValueArray(PendingOperations);
	PendingTextureOperations.Reset();
	PendingTextureOperationDeadline = 0.0;

	TMap<FString, FManagedTextureInfo> FolderInfos;
	for (const FPendingTextureOperation& Operation : PendingOperations)
	{
		if (ProcessPendingTextureOperation(Operation))
		{
			FolderInfos.FindOrAdd(Operation.Info.TargetFolderPath) = Operation.Info;
		}
	}

	TArray<FManagedTextureInfo> UniqueFolderInfos;
	FolderInfos.GenerateValueArray(UniqueFolderInfos);
	for (const FManagedTextureInfo& FolderInfo : UniqueFolderInfos)
	{
		ProcessManagedFolder(FolderInfo);
	}

	if (PendingTextureOperations.IsEmpty())
	{
		PendingFolderTickerHandle.Reset();
		return false;
	}

	return true;
}

bool FOcrTextureToolsModule::ProcessPendingTextureOperation(const FPendingTextureOperation& PendingOperation)
{
	UTexture2D* Texture = PendingOperation.Texture.Get();
	if (!Texture)
	{
		return false;
	}

	const FManagedTextureInfo& TextureInfo = PendingOperation.Info;

	ApplyTextureRules(Texture, TextureInfo.Kind);

	if (!EnsureTargetFolder(TextureInfo.TargetFolderPath))
	{
		UE_LOG(
			LogOcrTextureTools,
			Error,
			TEXT("Failed to ensure target folder %s for %s."),
			*TextureInfo.TargetFolderPath,
			*Texture->GetPathName());
		NotifyUser(FString::Printf(TEXT("Failed to create folder: %s"), *TextureInfo.TargetFolderPath), false);
		return false;
	}

	return MoveTextureToManagedFolder(Texture, TextureInfo);
}

void FOcrTextureToolsModule::ProcessManagedFolder(const FManagedTextureInfo& FolderInfo)
{
	TMap<FString, FManagedTextureGroup> Groups;
	if (!GatherManagedTextureGroups(FolderInfo.TargetFolderPath, Groups))
	{
		UE_LOG(
			LogOcrTextureTools,
			Warning,
			TEXT("Failed to scan managed material folder %s after import settled."),
			*FolderInfo.TargetFolderPath);
		return;
	}

	for (const TPair<FString, FManagedTextureGroup>& Pair : Groups)
	{
		FManagedTextureInfo GroupInfo = FolderInfo;
		GroupInfo.MaterialGroupName = Pair.Key;

		if (!Pair.Value.IsComplete())
		{
			ReportIncompleteGroup(GroupInfo, Pair.Value, TEXT("batched import"));
			continue;
		}

		ClearIncompleteReport(GroupInfo);
		UpdateMaterialInstance(GroupInfo, Pair.Value);
	}
}

IMPLEMENT_MODULE(FOcrTextureToolsModule, OcrTextureTools)
