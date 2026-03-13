#include "OcrTextureToolsModule.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/StaticMesh.h"
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
#include "UObject/UnrealType.h"
#include "Widgets/Notifications/SNotificationList.h"

DEFINE_LOG_CATEGORY_STATIC(LogOcrTextureTools, Log, All);

namespace
{
	struct FManagedMaterialInstanceAsset
	{
		FString AssetName;
		FString AssetPath;
		FString NormalizedName;
	};

	struct FManagedTextureAssetDataGroup
	{
		bool bHasBaseColor = false;
		bool bHasNormal = false;
		bool bHasOrm = false;
		FAssetData BaseColorAssetData;
		FAssetData NormalAssetData;
		FAssetData OrmAssetData;
	};

	FString BuildIncompleteKey(const FManagedTextureInfo& Info)
	{
		return FString::Printf(TEXT("%s|%s"), *Info.TargetFolderPath, *Info.MaterialGroupName);
	}

	bool IsAllDigits(const FString& Text)
	{
		if (Text.IsEmpty())
		{
			return false;
		}

		for (const TCHAR Character : Text)
		{
			if (!FChar::IsDigit(Character))
			{
				return false;
			}
		}

		return true;
	}

	FString StripNumericSuffix(const FString& Text)
	{
		int32 UnderscoreIndex = INDEX_NONE;
		if (!Text.FindLastChar(TEXT('_'), UnderscoreIndex))
		{
			return Text;
		}

		const FString Suffix = Text.Mid(UnderscoreIndex + 1);
		return IsAllDigits(Suffix) ? Text.Left(UnderscoreIndex) : Text;
	}

	FString TrimManagedContentRoot(FString ContentRoot)
	{
		ContentRoot.RemoveFromEnd(TEXT("/"));
		return ContentRoot;
	}

	FString NormalizeManagedMaterialToken(FString Token, const FString& SourceFolderName)
	{
		Token.TrimStartAndEndInline();

		if (Token.StartsWith(TEXT("MI_"), ESearchCase::IgnoreCase))
		{
			Token.RightChopInline(3, EAllowShrinking::No);
		}
		else if (Token.StartsWith(TEXT("M_"), ESearchCase::IgnoreCase))
		{
			Token.RightChopInline(2, EAllowShrinking::No);
		}

		const FString SourcePrefix = SourceFolderName + TEXT("_");
		if (!SourceFolderName.IsEmpty() && Token.StartsWith(SourcePrefix, ESearchCase::IgnoreCase))
		{
			Token.RightChopInline(SourcePrefix.Len(), EAllowShrinking::No);
		}

		return StripNumericSuffix(Token);
	}

	FString BuildManagedMaterialFolderPath(const FString& MaterialRootPath, const FString& SourceFolderName)
	{
		return FString::Printf(TEXT("%s/%s"), *MaterialRootPath, *SourceFolderName);
	}

	FString ResolveManagedAssetNameFromSourceFilename(const FString& SourceFilename, const FString& FallbackAssetName)
	{
		const FString SourceAssetLeaf = ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(SourceFilename));
		return SourceAssetLeaf.IsEmpty() ? FallbackAssetName : SourceAssetLeaf;
	}

	FString BuildStaticMeshFallbackFolderPath(const FManagedStaticMeshInfo& Info)
	{
		const FString MeshFolderPath = FPaths::GetPath(Info.TargetMeshAssetPath);
		return MeshFolderPath.IsEmpty()
			? FString()
			: FString::Printf(TEXT("%s/_ImportFallback/%s"), *MeshFolderPath, *Info.SourceFolderName);
	}

	FString BuildStaticMeshDuplicateFolderPath(const FManagedStaticMeshInfo& Info)
	{
		const FString MeshFolderPath = FPaths::GetPath(Info.TargetMeshAssetPath);
		return MeshFolderPath.IsEmpty()
			? FString()
			: FString::Printf(TEXT("%s/_DuplicateImports"), *MeshFolderPath);
	}

	FString BuildAssetPath(const FString& FolderPath, const FString& AssetName)
	{
		return FString::Printf(TEXT("%s/%s"), *FolderPath, *AssetName);
	}

	FString ResolveUniqueAssetPath(UEditorAssetSubsystem* AssetSubsystem, const FString& DesiredAssetPath)
	{
		if (!AssetSubsystem || !AssetSubsystem->DoesAssetExist(DesiredAssetPath))
		{
			return DesiredAssetPath;
		}

		FString UniquePackageName;
		FString UniqueAssetName;
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		AssetToolsModule.Get().CreateUniqueAssetName(DesiredAssetPath, TEXT(""), UniquePackageName, UniqueAssetName);
		return UniquePackageName;
	}

	FString ResolveStaticMeshSlotToken(const FStaticMaterial& StaticMaterial)
	{
		if (StaticMaterial.ImportedMaterialSlotName != NAME_None)
		{
			return StaticMaterial.ImportedMaterialSlotName.ToString();
		}

		if (StaticMaterial.MaterialSlotName != NAME_None)
		{
			return StaticMaterial.MaterialSlotName.ToString();
		}

		return StaticMaterial.MaterialInterface ? StaticMaterial.MaterialInterface->GetName() : FString();
	}

	void AddUniqueMaterialCandidate(TArray<FString>& CandidateNames, const FString& CandidateName)
	{
		if (!CandidateName.IsEmpty())
		{
			CandidateNames.AddUnique(CandidateName);
		}
	}

	FString GetNormalizedImportFilename(const UMaterialInterface* Material)
	{
		if (!Material || !Material->AssetImportData)
		{
			return FString();
		}

		FString ImportFilename = Material->AssetImportData->GetFirstFilename();
		FPaths::NormalizeFilename(ImportFilename);
		return ImportFilename;
	}
}

bool FManagedTextureGroup::IsComplete() const
{
	return bHasBaseColor && bHasNormal && bHasOrm;
}

FString FManagedTextureGroup::DescribeMissing() const
{
	TArray<FString> MissingKinds;
	if (!bHasBaseColor)
	{
		MissingKinds.Add(TEXT("BaseColor"));
	}
	if (!bHasNormal)
	{
		MissingKinds.Add(TEXT("Normal"));
	}
	if (!bHasOrm)
	{
		MissingKinds.Add(TEXT("ORM"));
	}

	return FString::Join(MissingKinds, TEXT(", "));
}

int32 FManagedTextureGroup::GetAvailableCount() const
{
	return (bHasBaseColor ? 1 : 0) + (bHasNormal ? 1 : 0) + (bHasOrm ? 1 : 0);
}

void FOcrTextureToolsModule::StartupModule()
{
	RegisterDelegates();
}

void FOcrTextureToolsModule::ShutdownModule()
{
	UnregisterDelegates();
	ReportedConfigurationErrors.Reset();
	ReportedIncompleteGroups.Reset();
	PendingTextureOperations.Reset();
	PendingStaticMeshOperations.Reset();
	PendingFallbackMaterialOperations.Reset();

	if (PendingImportTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PendingImportTickerHandle);
		PendingImportTickerHandle.Reset();
	}

	PendingImportOperationDeadline = 0.0;
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
		UE_LOG(LogOcrTextureTools, Warning, TEXT("Import subsystem not available; import automation disabled."));
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
	if (Texture)
	{
		ProcessImportedTexture(Texture, InReason);
		return;
	}

	if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(InObject))
	{
		ProcessImportedStaticMesh(StaticMesh, InReason);
		return;
	}

	if (UMaterialInterface* Material = Cast<UMaterialInterface>(InObject))
	{
		ProcessImportedFallbackMaterial(Material, InReason);
	}
}

void FOcrTextureToolsModule::ProcessImportedTexture(UTexture2D* Texture, const TCHAR* InReason)
{
	FManagedTextureInfo TextureInfo;
	if (!TryBuildManagedTextureInfo(Texture, TextureInfo))
	{
		return;
	}

	QueuePendingTextureOperation(Texture, TextureInfo);
}

void FOcrTextureToolsModule::ProcessImportedStaticMesh(UStaticMesh* StaticMesh, const TCHAR* InReason)
{
	FManagedStaticMeshInfo StaticMeshInfo;
	if (!TryBuildManagedStaticMeshInfo(StaticMesh, StaticMeshInfo))
	{
		return;
	}

	QueuePendingStaticMeshOperation(StaticMesh, StaticMeshInfo);
}

void FOcrTextureToolsModule::ProcessImportedFallbackMaterial(UMaterialInterface* Material, const TCHAR* InReason)
{
	FManagedFallbackMaterialInfo MaterialInfo;
	if (!TryBuildManagedFallbackMaterialInfo(Material, MaterialInfo))
	{
		return;
	}

	QueuePendingFallbackMaterialOperation(Material, MaterialInfo);
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
		const FString ErrorMessage = FString::Printf(TEXT("TargetRootContentPath must be set and start with /Game. Current value: %s"), *TargetRoot);
		UE_LOG(LogOcrTextureTools, Error, TEXT("%s"), *ErrorMessage);
		ReportConfigurationError(TEXT("TargetRootContentPath"), ErrorMessage);
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

bool FOcrTextureToolsModule::TryBuildManagedStaticMeshInfo(UStaticMesh* StaticMesh, FManagedStaticMeshInfo& OutInfo) const
{
	if (!StaticMesh)
	{
		return false;
	}

	const UOcrTextureToolsSettings* Settings = GetDefault<UOcrTextureToolsSettings>();
	if (!Settings || !Settings->bEnableAutomaticProcessing)
	{
		return false;
	}

	UAssetImportData* AssetImportData = StaticMesh->GetAssetImportData();
	if (!AssetImportData)
	{
		UE_LOG(LogOcrTextureTools, Warning, TEXT("Static mesh %s has no asset import data; skipping automation."), *StaticMesh->GetPathName());
		return false;
	}

	FString SourceFilename = AssetImportData->GetFirstFilename();
	if (SourceFilename.IsEmpty())
	{
		UE_LOG(LogOcrTextureTools, Warning, TEXT("Static mesh %s has no source filename; skipping automation."), *StaticMesh->GetPathName());
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
		UE_LOG(LogOcrTextureTools, Warning, TEXT("Static mesh %s source folder could not be resolved from %s."), *StaticMesh->GetPathName(), *SourceFilename);
		return false;
	}

	FString MaterialRootPath = TrimManagedContentRoot(Settings->TargetRootContentPath);
	if (!MaterialRootPath.StartsWith(TEXT("/Game")))
	{
		const FString ErrorMessage = FString::Printf(TEXT("TargetRootContentPath must be set and start with /Game before static mesh material assignment can run. Current value: %s"), *MaterialRootPath);
		UE_LOG(LogOcrTextureTools, Error, TEXT("%s"), *ErrorMessage);
		ReportConfigurationError(TEXT("TargetRootContentPath"), ErrorMessage);
		return false;
	}

	const FString SourceFolderName = ObjectTools::SanitizeObjectName(SourceFolderLeaf);
	if (SourceFolderName.IsEmpty())
	{
		UE_LOG(LogOcrTextureTools, Warning, TEXT("Static mesh source folder %s resolved to an empty Unreal name."), *SourceFolderLeaf);
		return false;
	}

	OutInfo.SourceFilename = SourceFilename;
	OutInfo.SourceFolderName = SourceFolderName;
	OutInfo.MeshAssetName = ResolveManagedAssetNameFromSourceFilename(SourceFilename, StaticMesh->GetName());
	OutInfo.MaterialFolderPath = BuildManagedMaterialFolderPath(MaterialRootPath, SourceFolderName);

	FString StaticMeshTargetRootPath = TrimManagedContentRoot(Settings->StaticMeshTargetRootContentPath);
	if (StaticMeshTargetRootPath.IsEmpty())
	{
		OutInfo.TargetMeshAssetPath = StaticMesh->GetOutermost()->GetName();
		return true;
	}

	if (!StaticMeshTargetRootPath.StartsWith(TEXT("/Game")))
	{
		const FString ErrorMessage = FString::Printf(TEXT("StaticMeshTargetRootContentPath must be set and start with /Game. Current value: %s"), *StaticMeshTargetRootPath);
		UE_LOG(LogOcrTextureTools, Error, TEXT("%s"), *ErrorMessage);
		ReportConfigurationError(TEXT("StaticMeshTargetRootContentPath"), ErrorMessage);
		return false;
	}

	OutInfo.TargetMeshAssetPath = BuildAssetPath(StaticMeshTargetRootPath, OutInfo.MeshAssetName);
	return true;
}

bool FOcrTextureToolsModule::TryBuildManagedFallbackMaterialInfo(UMaterialInterface* Material, FManagedFallbackMaterialInfo& OutInfo) const
{
	if (!Material || !Material->AssetImportData)
	{
		return false;
	}

	const UOcrTextureToolsSettings* Settings = GetDefault<UOcrTextureToolsSettings>();
	if (!Settings || !Settings->bEnableAutomaticProcessing)
	{
		return false;
	}

	FString SourceFilename = Material->AssetImportData->GetFirstFilename();
	if (SourceFilename.IsEmpty())
	{
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

	FString StaticMeshTargetRootPath = TrimManagedContentRoot(Settings->StaticMeshTargetRootContentPath);
	if (!StaticMeshTargetRootPath.StartsWith(TEXT("/Game")))
	{
		const FString ErrorMessage = FString::Printf(TEXT("StaticMeshTargetRootContentPath must be set and start with /Game before fallback material archiving can run. Current value: %s"), *StaticMeshTargetRootPath);
		UE_LOG(LogOcrTextureTools, Error, TEXT("%s"), *ErrorMessage);
		ReportConfigurationError(TEXT("StaticMeshTargetRootContentPath"), ErrorMessage);
		return false;
	}

	const FString SourceFolderLeaf = FPaths::GetCleanFilename(SourceDirectory);
	const FString SourceFolderName = ObjectTools::SanitizeObjectName(SourceFolderLeaf);
	if (SourceFolderName.IsEmpty())
	{
		return false;
	}

	const FString CurrentAssetPath = Material->GetOutermost()->GetName();
	const FString TargetFolderPath = FString::Printf(TEXT("%s/_ImportFallback/%s"), *StaticMeshTargetRootPath, *SourceFolderName);
	const FString TargetAssetPath = FString::Printf(TEXT("%s/%s"), *TargetFolderPath, *Material->GetName());

	if (CurrentAssetPath.Equals(TargetAssetPath, ESearchCase::CaseSensitive))
	{
		return false;
	}

	OutInfo.SourceFilename = SourceFilename;
	OutInfo.SourceFolderName = SourceFolderName;
	OutInfo.TargetFolderPath = TargetFolderPath;
	OutInfo.TargetAssetPath = TargetAssetPath;
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

bool FOcrTextureToolsModule::MoveStaticMeshToManagedFolder(UStaticMesh* StaticMesh, const FManagedStaticMeshInfo& Info) const
{
	if (!StaticMesh || Info.TargetMeshAssetPath.IsEmpty())
	{
		return false;
	}

	UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
	if (!AssetSubsystem)
	{
		UE_LOG(LogOcrTextureTools, Error, TEXT("Editor asset subsystem unavailable; cannot move static mesh %s."), *StaticMesh->GetPathName());
		return false;
	}

	const FString CurrentAssetPath = StaticMesh->GetOutermost()->GetName();
	if (CurrentAssetPath.Equals(Info.TargetMeshAssetPath, ESearchCase::CaseSensitive))
	{
		return true;
	}

	const FString DuplicateFolderPath = BuildStaticMeshDuplicateFolderPath(Info);
	if (!DuplicateFolderPath.IsEmpty())
	{
		const FString DuplicateFolderPrefix = DuplicateFolderPath + TEXT("/");
		if (CurrentAssetPath.StartsWith(DuplicateFolderPrefix, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	FString DestinationAssetPath = Info.TargetMeshAssetPath;
	if (AssetSubsystem->DoesAssetExist(Info.TargetMeshAssetPath))
	{
		if (DuplicateFolderPath.IsEmpty() || !EnsureTargetFolder(DuplicateFolderPath))
		{
			UE_LOG(
				LogOcrTextureTools,
				Error,
				TEXT("Failed to ensure duplicate static mesh folder %s for %s."),
				*DuplicateFolderPath,
				*CurrentAssetPath);
			NotifyUser(FString::Printf(TEXT("Failed to create duplicate static mesh folder: %s"), *DuplicateFolderPath), false);
			return false;
		}

		DestinationAssetPath = ResolveUniqueAssetPath(AssetSubsystem, BuildAssetPath(DuplicateFolderPath, Info.MeshAssetName));
		UE_LOG(
			LogOcrTextureTools,
			Warning,
			TEXT("Target static mesh %s already exists. Routing imported mesh %s to duplicate path %s."),
			*Info.TargetMeshAssetPath,
			*CurrentAssetPath,
			*DestinationAssetPath);
	}

	const FString TargetFolderPath = FPaths::GetPath(DestinationAssetPath);
	if (TargetFolderPath.IsEmpty() || !EnsureTargetFolder(TargetFolderPath))
	{
		UE_LOG(LogOcrTextureTools, Error, TEXT("Failed to ensure static mesh target folder %s for %s."), *TargetFolderPath, *CurrentAssetPath);
		NotifyUser(FString::Printf(TEXT("Failed to create static mesh folder: %s"), *TargetFolderPath), false);
		return false;
	}

	if (!AssetSubsystem->RenameLoadedAsset(StaticMesh, DestinationAssetPath))
	{
		UE_LOG(
			LogOcrTextureTools,
			Error,
			TEXT("Failed to move static mesh %s to %s."),
			*CurrentAssetPath,
			*DestinationAssetPath);
		NotifyUser(FString::Printf(TEXT("Failed to move static mesh: %s"), *StaticMesh->GetName()), false);
		return false;
	}

	UE_LOG(
		LogOcrTextureTools,
		Log,
		TEXT("Moved static mesh %s to %s based on source folder %s."),
		*CurrentAssetPath,
		*DestinationAssetPath,
		*Info.SourceFolderName);
	return true;
}

bool FOcrTextureToolsModule::MoveFallbackMaterialToManagedFolder(UMaterialInterface* Material, const FManagedFallbackMaterialInfo& Info) const
{
	if (!Material)
	{
		return false;
	}

	UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
	if (!AssetSubsystem)
	{
		return false;
	}

	const FString CurrentAssetPath = Material->GetOutermost()->GetName();
	if (CurrentAssetPath.Equals(Info.TargetAssetPath, ESearchCase::CaseSensitive))
	{
		return true;
	}

	if (!EnsureTargetFolder(Info.TargetFolderPath))
	{
		UE_LOG(
			LogOcrTextureTools,
			Warning,
			TEXT("Failed to ensure fallback material folder %s for %s."),
			*Info.TargetFolderPath,
			*CurrentAssetPath);
		return false;
	}

	FString DestinationAssetPath = ResolveUniqueAssetPath(AssetSubsystem, Info.TargetAssetPath);

	if (!AssetSubsystem->RenameLoadedAsset(Material, DestinationAssetPath))
	{
		UE_LOG(
			LogOcrTextureTools,
			Warning,
			TEXT("Failed to move imported fallback material %s to %s."),
			*CurrentAssetPath,
			*DestinationAssetPath);
		return false;
	}

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

	TMap<FString, FManagedTextureAssetDataGroup> AssetGroups;
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

		FManagedTextureAssetDataGroup& Group = AssetGroups.FindOrAdd(GroupName);
		switch (Kind)
		{
		case EManagedTextureKind::BaseColor:
			Group.bHasBaseColor = true;
			Group.BaseColorAssetData = AssetData;
			break;
		case EManagedTextureKind::Normal:
			Group.bHasNormal = true;
			Group.NormalAssetData = AssetData;
			break;
		case EManagedTextureKind::Orm:
			Group.bHasOrm = true;
			Group.OrmAssetData = AssetData;
			break;
		default:
			break;
		}
	}

	for (const TPair<FString, FManagedTextureAssetDataGroup>& Pair : AssetGroups)
	{
		const FManagedTextureAssetDataGroup& AssetGroup = Pair.Value;
		FManagedTextureGroup& Group = OutGroups.FindOrAdd(Pair.Key);
		Group.bHasBaseColor = AssetGroup.bHasBaseColor;
		Group.bHasNormal = AssetGroup.bHasNormal;
		Group.bHasOrm = AssetGroup.bHasOrm;

		if (!Group.IsComplete())
		{
			continue;
		}

		Group.BaseColor = Cast<UTexture2D>(AssetGroup.BaseColorAssetData.GetAsset());
		Group.Normal = Cast<UTexture2D>(AssetGroup.NormalAssetData.GetAsset());
		Group.Orm = Cast<UTexture2D>(AssetGroup.OrmAssetData.GetAsset());

		if (!Group.BaseColor)
		{
			Group.bHasBaseColor = false;
		}
		if (!Group.Normal)
		{
			Group.bHasNormal = false;
		}
		if (!Group.Orm)
		{
			Group.bHasOrm = false;
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

bool FOcrTextureToolsModule::ArchiveImportedFallbackMaterials(UStaticMesh* StaticMesh, const FManagedStaticMeshInfo& Info) const
{
	if (!StaticMesh)
	{
		return false;
	}

	UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
	if (!AssetSubsystem)
	{
		return false;
	}

	const FString FallbackFolderPath = BuildStaticMeshFallbackFolderPath(Info);
	if (FallbackFolderPath.IsEmpty())
	{
		return false;
	}

	FString NormalizedSourceFilename = Info.SourceFilename;
	FPaths::NormalizeFilename(NormalizedSourceFilename);

	TMap<FString, UMaterialInterface*> MaterialsToArchive;
	for (const FStaticMaterial& StaticMaterial : StaticMesh->GetStaticMaterials())
	{
		UMaterialInterface* CurrentMaterial = StaticMaterial.MaterialInterface;
		if (!CurrentMaterial)
		{
			continue;
		}

		const FString CurrentMaterialPath = CurrentMaterial->GetOutermost()->GetName();
		if (!CurrentMaterialPath.StartsWith(TEXT("/Game")))
		{
			continue;
		}

		if (CurrentMaterialPath.StartsWith(FallbackFolderPath, ESearchCase::IgnoreCase)
			|| CurrentMaterialPath.StartsWith(Info.MaterialFolderPath, ESearchCase::IgnoreCase))
		{
			continue;
		}

		const FString SlotToken = ResolveStaticMeshSlotToken(StaticMaterial);
		const FString NormalizedSlotToken = NormalizeManagedMaterialToken(SlotToken, Info.SourceFolderName);
		const FString NormalizedMaterialName = NormalizeManagedMaterialToken(CurrentMaterial->GetName(), Info.SourceFolderName);
		const FString MaterialImportFilename = GetNormalizedImportFilename(CurrentMaterial);

		const bool bMatchesImportSource = !MaterialImportFilename.IsEmpty() && MaterialImportFilename.Equals(NormalizedSourceFilename, ESearchCase::IgnoreCase);
		const bool bMatchesSlotName = !NormalizedSlotToken.IsEmpty() && NormalizedMaterialName.Equals(NormalizedSlotToken, ESearchCase::IgnoreCase);
		if (!bMatchesImportSource && !bMatchesSlotName)
		{
			continue;
		}

		MaterialsToArchive.Add(CurrentMaterialPath, CurrentMaterial);
	}

	if (MaterialsToArchive.IsEmpty())
	{
		return false;
	}

	if (!EnsureTargetFolder(FallbackFolderPath))
	{
		UE_LOG(
			LogOcrTextureTools,
			Warning,
			TEXT("Failed to ensure fallback material folder %s for static mesh %s."),
			*FallbackFolderPath,
			*StaticMesh->GetPathName());
		return false;
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	int32 ArchivedCount = 0;

	for (const TPair<FString, UMaterialInterface*>& Pair : MaterialsToArchive)
	{
		UMaterialInterface* Material = Pair.Value;
		if (!Material)
		{
			continue;
		}

		const FString BaseAssetPath = FString::Printf(TEXT("%s/%s"), *FallbackFolderPath, *Material->GetName());
		FString DestinationAssetPath = BaseAssetPath;

		if (AssetSubsystem->DoesAssetExist(DestinationAssetPath))
		{
			FString UniquePackageName;
			FString UniqueAssetName;
			AssetToolsModule.Get().CreateUniqueAssetName(BaseAssetPath, TEXT(""), UniquePackageName, UniqueAssetName);
			DestinationAssetPath = UniquePackageName;
		}

		if (!AssetSubsystem->RenameLoadedAsset(Material, DestinationAssetPath))
		{
			UE_LOG(
				LogOcrTextureTools,
				Warning,
				TEXT("Failed to move imported fallback material %s to %s."),
				*Pair.Key,
				*DestinationAssetPath);
			continue;
		}

		ArchivedCount++;
	}

	if (ArchivedCount > 0)
	{
		UE_LOG(
			LogOcrTextureTools,
			Log,
			TEXT("Archived %d imported fallback materials for %s into %s."),
			ArchivedCount,
			*StaticMesh->GetPathName(),
			*FallbackFolderPath);
	}

	return ArchivedCount > 0;
}

bool FOcrTextureToolsModule::ApplyManagedMaterialInstances(UStaticMesh* StaticMesh, const FManagedStaticMeshInfo& Info) const
{
	if (!StaticMesh)
	{
		return false;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");

	TArray<FAssetData> AssetDataArray;
	if (!AssetRegistryModule.Get().GetAssetsByPath(FName(*Info.MaterialFolderPath), AssetDataArray, false, false))
	{
		UE_LOG(
			LogOcrTextureTools,
			Warning,
			TEXT("Managed material folder %s could not be scanned for static mesh %s."),
			*Info.MaterialFolderPath,
			*StaticMesh->GetPathName());
		return false;
	}

	TArray<FManagedMaterialInstanceAsset> ManagedMaterials;
	for (const FAssetData& AssetData : AssetDataArray)
	{
		if (AssetData.AssetClassPath != UMaterialInstanceConstant::StaticClass()->GetClassPathName())
		{
			continue;
		}

		FManagedMaterialInstanceAsset& ManagedMaterial = ManagedMaterials.AddDefaulted_GetRef();
		ManagedMaterial.AssetName = AssetData.AssetName.ToString();
		ManagedMaterial.AssetPath = AssetData.GetObjectPathString();
		ManagedMaterial.NormalizedName = NormalizeManagedMaterialToken(ManagedMaterial.AssetName, Info.SourceFolderName);
	}

	if (ManagedMaterials.IsEmpty())
	{
		UE_LOG(
			LogOcrTextureTools,
			Warning,
			TEXT("No managed material instances were found in %s for static mesh %s."),
			*Info.MaterialFolderPath,
			*StaticMesh->GetPathName());
		return false;
	}

	UEditorAssetSubsystem* AssetSubsystem = GetEditorAssetSubsystem();
	if (!AssetSubsystem)
	{
		return false;
	}

	const TArray<FStaticMaterial>& StaticMaterials = StaticMesh->GetStaticMaterials();
	if (StaticMaterials.IsEmpty())
	{
		return false;
	}

	TMap<int32, UMaterialInterface*> PendingAssignments;
	TArray<FString> MissingSlots;

	for (int32 MaterialIndex = 0; MaterialIndex < StaticMaterials.Num(); ++MaterialIndex)
	{
		const FStaticMaterial& StaticMaterial = StaticMaterials[MaterialIndex];
		const FString SlotToken = ResolveStaticMeshSlotToken(StaticMaterial);
		const FString NormalizedSlotToken = NormalizeManagedMaterialToken(SlotToken, Info.SourceFolderName);

		if (NormalizedSlotToken.IsEmpty())
		{
			continue;
		}

		TArray<FString> CandidateAssetNames;
		AddUniqueMaterialCandidate(CandidateAssetNames, FString::Printf(TEXT("MI_%s_%s"), *Info.SourceFolderName, *NormalizedSlotToken));
		AddUniqueMaterialCandidate(CandidateAssetNames, FString::Printf(TEXT("MI_%s"), *NormalizedSlotToken));

		const FString RawNormalizedCurrentMaterial = StaticMaterial.MaterialInterface
			? NormalizeManagedMaterialToken(StaticMaterial.MaterialInterface->GetName(), Info.SourceFolderName)
			: FString();
		if (!RawNormalizedCurrentMaterial.IsEmpty() && RawNormalizedCurrentMaterial != NormalizedSlotToken)
		{
			AddUniqueMaterialCandidate(CandidateAssetNames, FString::Printf(TEXT("MI_%s_%s"), *Info.SourceFolderName, *RawNormalizedCurrentMaterial));
			AddUniqueMaterialCandidate(CandidateAssetNames, FString::Printf(TEXT("MI_%s"), *RawNormalizedCurrentMaterial));
		}

		const FManagedMaterialInstanceAsset* MatchedAsset = nullptr;
		for (const FString& CandidateAssetName : CandidateAssetNames)
		{
			MatchedAsset = ManagedMaterials.FindByPredicate(
				[&CandidateAssetName](const FManagedMaterialInstanceAsset& ManagedMaterial)
				{
					return ManagedMaterial.AssetName.Equals(CandidateAssetName, ESearchCase::IgnoreCase);
				});
			if (MatchedAsset)
			{
				break;
			}
		}

		if (!MatchedAsset)
		{
			TArray<const FManagedMaterialInstanceAsset*> NormalizedMatches;
			for (const FManagedMaterialInstanceAsset& ManagedMaterial : ManagedMaterials)
			{
				if (ManagedMaterial.NormalizedName.Equals(NormalizedSlotToken, ESearchCase::IgnoreCase))
				{
					NormalizedMatches.Add(&ManagedMaterial);
				}
			}

			if (NormalizedMatches.Num() == 1)
			{
				MatchedAsset = NormalizedMatches[0];
			}
			else if (NormalizedMatches.Num() > 1)
			{
				UE_LOG(
					LogOcrTextureTools,
					Warning,
					TEXT("Multiple managed material instances in %s matched slot %s on %s; leaving slot unchanged."),
					*Info.MaterialFolderPath,
					*SlotToken,
					*StaticMesh->GetPathName());
				MissingSlots.AddUnique(SlotToken);
				continue;
			}
		}

		if (!MatchedAsset)
		{
			MissingSlots.AddUnique(SlotToken);
			continue;
		}

		UMaterialInterface* TargetMaterial = Cast<UMaterialInterface>(AssetSubsystem->LoadAsset(MatchedAsset->AssetPath));
		if (!TargetMaterial)
		{
			UE_LOG(
				LogOcrTextureTools,
				Warning,
				TEXT("Managed material instance %s could not be loaded for slot %s on %s."),
				*MatchedAsset->AssetPath,
				*SlotToken,
				*StaticMesh->GetPathName());
			MissingSlots.AddUnique(SlotToken);
			continue;
		}

		if (StaticMaterial.MaterialInterface != TargetMaterial)
		{
			PendingAssignments.Add(MaterialIndex, TargetMaterial);
		}
	}

	if (!PendingAssignments.IsEmpty())
	{
		for (int32 LODIndex = 0; LODIndex < StaticMesh->GetNumSourceModels(); ++LODIndex)
		{
			StaticMesh->GetMeshDescription(LODIndex);
		}

		FProperty* ChangedProperty = FindFProperty<FProperty>(UStaticMesh::StaticClass(), UStaticMesh::GetStaticMaterialsName());
		check(ChangedProperty);

		StaticMesh->Modify();
		StaticMesh->PreEditChange(ChangedProperty);

		for (const TPair<int32, UMaterialInterface*>& Assignment : PendingAssignments)
		{
			StaticMesh->GetStaticMaterials()[Assignment.Key].MaterialInterface = Assignment.Value;
		}

		FPropertyChangedEvent PropertyChangedEvent(ChangedProperty);
		StaticMesh->PostEditChangeProperty(PropertyChangedEvent);
		StaticMesh->MarkPackageDirty();
	}

	if (!MissingSlots.IsEmpty())
	{
		UE_LOG(
			LogOcrTextureTools,
			Warning,
			TEXT("Static mesh %s did not find managed material instances for slots: %s."),
			*StaticMesh->GetPathName(),
			*FString::Join(MissingSlots, TEXT(", ")));
	}

	if (!PendingAssignments.IsEmpty())
	{
		UE_LOG(
			LogOcrTextureTools,
			Log,
			TEXT("Updated %d static mesh material slots on %s from managed folder %s."),
			PendingAssignments.Num(),
			*StaticMesh->GetPathName(),
			*Info.MaterialFolderPath);
		NotifyUser(FString::Printf(TEXT("Updated %s material slots"), *StaticMesh->GetName()), MissingSlots.IsEmpty());
	}

	return true;
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

void FOcrTextureToolsModule::ReportConfigurationError(const FString& Key, const FString& Message) const
{
	if (Key.IsEmpty() || ReportedConfigurationErrors.Contains(Key))
	{
		return;
	}

	ReportedConfigurationErrors.Add(Key);
	NotifyUser(Message, false);
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
	PendingImportOperationDeadline = FPlatformTime::Seconds() + 1.5;

	if (!PendingImportTickerHandle.IsValid())
	{
		PendingImportTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FOcrTextureToolsModule::FlushPendingImportOperations),
			0.25f);
	}
}

void FOcrTextureToolsModule::QueuePendingStaticMeshOperation(UStaticMesh* StaticMesh, const FManagedStaticMeshInfo& Info)
{
	FPendingStaticMeshOperation Operation;
	Operation.StaticMesh = StaticMesh;
	Operation.Info = Info;

	PendingStaticMeshOperations.Add(StaticMesh->GetOutermost()->GetName(), Operation);
	PendingImportOperationDeadline = FPlatformTime::Seconds() + 1.5;

	if (!PendingImportTickerHandle.IsValid())
	{
		PendingImportTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FOcrTextureToolsModule::FlushPendingImportOperations),
			0.25f);
	}
}

void FOcrTextureToolsModule::QueuePendingFallbackMaterialOperation(UMaterialInterface* Material, const FManagedFallbackMaterialInfo& Info)
{
	FPendingFallbackMaterialOperation Operation;
	Operation.Material = Material;
	Operation.Info = Info;

	PendingFallbackMaterialOperations.Add(Info.TargetAssetPath, Operation);
	PendingImportOperationDeadline = FPlatformTime::Seconds() + 1.5;

	if (!PendingImportTickerHandle.IsValid())
	{
		PendingImportTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FOcrTextureToolsModule::FlushPendingImportOperations),
			0.25f);
	}
}

bool FOcrTextureToolsModule::FlushPendingImportOperations(float DeltaTime)
{
	if (FPlatformTime::Seconds() < PendingImportOperationDeadline)
	{
		return true;
	}

	TArray<FPendingTextureOperation> PendingTextureOps;
	PendingTextureOperations.GenerateValueArray(PendingTextureOps);
	PendingTextureOperations.Reset();

	TArray<FPendingStaticMeshOperation> PendingStaticMeshOps;
	PendingStaticMeshOperations.GenerateValueArray(PendingStaticMeshOps);
	PendingStaticMeshOperations.Reset();

	TArray<FPendingFallbackMaterialOperation> PendingFallbackMaterialOps;
	PendingFallbackMaterialOperations.GenerateValueArray(PendingFallbackMaterialOps);
	PendingFallbackMaterialOperations.Reset();

	PendingImportOperationDeadline = 0.0;

	TMap<FString, FManagedTextureInfo> FolderInfos;
	for (const FPendingTextureOperation& Operation : PendingTextureOps)
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

	for (const FPendingFallbackMaterialOperation& Operation : PendingFallbackMaterialOps)
	{
		ProcessPendingFallbackMaterialOperation(Operation);
	}

	for (const FPendingStaticMeshOperation& Operation : PendingStaticMeshOps)
	{
		ProcessPendingStaticMeshOperation(Operation);
	}

	if (PendingTextureOperations.IsEmpty() && PendingStaticMeshOperations.IsEmpty() && PendingFallbackMaterialOperations.IsEmpty())
	{
		PendingImportTickerHandle.Reset();
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

bool FOcrTextureToolsModule::ProcessPendingStaticMeshOperation(const FPendingStaticMeshOperation& PendingOperation)
{
	UStaticMesh* StaticMesh = PendingOperation.StaticMesh.Get();
	if (!StaticMesh)
	{
		return false;
	}

	const FManagedStaticMeshInfo& StaticMeshInfo = PendingOperation.Info;
	if (!MoveStaticMeshToManagedFolder(StaticMesh, StaticMeshInfo))
	{
		const FString CurrentAssetPath = StaticMesh->GetOutermost()->GetName();
		if (!CurrentAssetPath.Equals(StaticMeshInfo.TargetMeshAssetPath, ESearchCase::CaseSensitive))
		{
			return false;
		}
	}

	ArchiveImportedFallbackMaterials(StaticMesh, StaticMeshInfo);
	ApplyManagedMaterialInstances(StaticMesh, StaticMeshInfo);
	return true;
}

bool FOcrTextureToolsModule::ProcessPendingFallbackMaterialOperation(const FPendingFallbackMaterialOperation& PendingOperation)
{
	UMaterialInterface* Material = PendingOperation.Material.Get();
	if (!Material)
	{
		return false;
	}

	return MoveFallbackMaterialToManagedFolder(Material, PendingOperation.Info);
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
